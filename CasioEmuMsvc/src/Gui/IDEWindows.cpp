#include "IDEWindows.h"
#include "SysDialog.h"
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <sstream>

#ifndef _WIN32
#ifdef __ANDROID__
#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <termios.h>
inline pid_t android_forkpty(int* amaster, char* name, const struct termios* termp, const struct winsize* winp) {
    int master, slave;
    if (openpty(&master, &slave, name, termp, winp) < 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }
    if (pid == 0) {
        close(master);
        setsid();
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        return 0;
    }
    close(slave);
    *amaster = master;
    return pid;
}
#define forkpty android_forkpty
#elif defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define popen _popen
#define pclose _pclose

// Define ConPTY types and attributes if compiling on Windows
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif

typedef void* HPCON;
typedef HRESULT(WINAPI* PFNCREATEPSEUDOCONSOLE)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT(WINAPI* PFNCLOSEPSEUDOCONSOLE)(HPCON);
#endif
#include <fcntl.h>
#include <thread>
#include <signal.h>

// ================= CODE EDITOR =================

CodeEditorWindow::CodeEditorWindow(const std::string& path) : UIWindow(path.c_str()), m_filePath(path) {
    std::filesystem::path p(path);
    name = strdup(p.filename().string().c_str());

    std::ifstream t(path);
    if (t.good()) {
        std::string str((std::istreambuf_iterator<char>(t)),
                         std::istreambuf_iterator<char>());
        m_editor.SetLanguageDefinition(TextEditor::LanguageDefinition::C());
        m_editor.SetText(str);
        m_isModified = false;
        m_firstTime = true;
    }
}

CodeEditorWindow::~CodeEditorWindow() {
    // Note: We don't free `name` if UIWindow doesn't expect us to.
    // However, since we used strdup, we should probably handle it if UIWindow doesn't.
    // Currently, UIWindow has `const char* name{};` and doesn't own it by default.
    // For simplicity, we just let it leak a tiny string per file, or free it.
    free((void*)name);
}

void CodeEditorWindow::SaveFile() {
    if (m_filePath.empty()) return;
    std::ofstream t(m_filePath);
    if (t.good()) {
        t << m_editor.GetText();
        m_isModified = false;
    }
}

void CodeEditorWindow::Render() {
    extern ImGuiID g_dock_main_id;
    if (g_dock_main_id != 0) {
        ImGui::SetNextWindowDockID(g_dock_main_id, ImGuiCond_Appearing);
    }
    UIWindow::Render();
}

void CodeEditorWindow::RenderCore() {
    // Hotkey triggers
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        SaveFile();
    }

    if (m_editor.IsTextChanged()) {
        m_isModified = true;
    }

    if (ImGui::Button("Save")) {
        SaveFile();
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo")) {
        m_editor.Undo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Redo")) {
        m_editor.Redo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        m_editor.Copy();
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste")) {
        m_editor.Paste();
    }
    ImGui::SameLine();
    if (ImGui::Button("Run (F5)")) {
        if (RunDebugWindow::Instance) {
            RunDebugWindow::Instance->RunCurrentFile(this);
        }
    }
    ImGui::Spacing();
    
    // Update tab name if modified
    std::string newName = std::filesystem::path(m_filePath).filename().string();
    if (m_isModified) newName += " *";
    
    if (strcmp(name, newName.c_str()) != 0) {
        free((void*)name);
        name = strdup(newName.c_str());
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    m_editor.Render("Editor");
    
    if (ImGui::BeginPopupContextItem("EditorContextMenu")) {
        if (ImGui::Selectable("Copy", false, m_editor.HasSelection() ? 0 : ImGuiSelectableFlags_Disabled)) {
            m_editor.Copy();
        }
        if (ImGui::Selectable("Paste")) {
            m_editor.Paste();
        }
        if (ImGui::Selectable("Cut", false, m_editor.HasSelection() ? 0 : ImGuiSelectableFlags_Disabled)) {
            m_editor.Cut();
        }
        if (ImGui::Selectable("Delete", false, m_editor.HasSelection() ? 0 : ImGuiSelectableFlags_Disabled)) {
            m_editor.Delete();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Select All")) {
            m_editor.SelectAll();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ================= RUN & DEBUG =================
RunDebugWindow* RunDebugWindow::Instance = nullptr;

RunDebugWindow::RunDebugWindow() {
    Instance = this;
}

RunDebugWindow::~RunDebugWindow() {
    if (m_isCompiling) {
        m_isCompiling = false;
        if (m_compileThread.joinable()) {
            m_compileThread.join();
        }
    }
}

void RunDebugWindow::SetCompilerPath(const std::string& type, const std::string& path) {
    if (type == "rac") m_racCompilerPath = path;
    else if (type == "hd") m_hdCompilerPath = path;
    
    if (TerminalWindow::Instance) {
        TerminalWindow::Instance->AppendLog("[Config] " + type + " compiler path set to: " + path + "\n");
    }
    SaveUIState();
}

void RunDebugWindow::RunCurrentFile(CodeEditorWindow* editor) {
    CodeEditorWindow* activeEditor = editor ? editor : GetFocusedCodeEditor();
    if (!activeEditor) {
        if (TerminalWindow::Instance) {
            TerminalWindow::Instance->AppendLog("[Error] No active file editor to run.\n");
        }
        return;
    }
    
    std::string currentPath = activeEditor->m_filePath;
    if (currentPath.empty()) {
        if (TerminalWindow::Instance) {
            TerminalWindow::Instance->AppendLog("[Error] No file open to run.\n");
        }
        return;
    }
    
    activeEditor->SaveFile();
    
    std::filesystem::path filePath(currentPath);
    std::string ext = filePath.extension().string();
    
    std::string cmd;
    
    if (ext == ".rsc") {
        if (m_racCompilerPath.empty()) {
            if (WorkspaceExplorer::Instance && !WorkspaceExplorer::Instance->GetWorkspaceRoot().empty()) {
                std::filesystem::path wsRoot(WorkspaceExplorer::Instance->GetWorkspaceRoot());
                if (std::filesystem::exists(wsRoot / "main.py")) {
                    m_racCompilerPath = wsRoot.string();
                    if (TerminalWindow::Instance) {
                        TerminalWindow::Instance->AppendLog("[Auto-Detect] Found RAC Compiler at workspace root: " + m_racCompilerPath + "\n");
                    }
                }
            }
        }
        
        if (!m_racCompilerPath.empty()) {
            std::string pathStr = m_racCompilerPath;
            while (!pathStr.empty() && (pathStr.back() == '/' || pathStr.back() == '\\')) {
                pathStr.pop_back();
            }
            std::filesystem::path compPath(pathStr);
            if (!std::filesystem::exists(compPath / "main.py") && std::filesystem::exists(compPath.parent_path() / "main.py")) {
                m_racCompilerPath = compPath.parent_path().string();
                if (TerminalWindow::Instance) {
                    TerminalWindow::Instance->AppendLog("[Auto-Correct] Adjusted RAC Compiler path to parent: " + m_racCompilerPath + "\n");
                }
            }
        }

        if (m_racCompilerPath.empty()) {
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("[Error] RAC Compiler path not set. Use Run > Set RAC Compiler Path.\n");
            }
            return;
        }
        std::string runCmd = m_racRunCommandBuf;
        if (runCmd.empty()) {
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("[Error] RAC Run Command not set. Use Run > Edit RAC Run Command.\n");
            }
            return;
        }
        cmd = "cd \"" + m_racCompilerPath + "\" && " + runCmd + " \"" + currentPath + "\" 2>&1";
    }
    else if (ext == ".asm") {
        if (m_hdCompilerPath.empty()) {
            if (WorkspaceExplorer::Instance && !WorkspaceExplorer::Instance->GetWorkspaceRoot().empty()) {
                std::filesystem::path wsRoot(WorkspaceExplorer::Instance->GetWorkspaceRoot());
                if (std::filesystem::exists(wsRoot / "main.py")) {
                    m_hdCompilerPath = wsRoot.string();
                    if (TerminalWindow::Instance) {
                        TerminalWindow::Instance->AppendLog("[Auto-Detect] Found HD Compiler at workspace root: " + m_hdCompilerPath + "\n");
                    }
                }
            }
        }
        
        if (!m_hdCompilerPath.empty()) {
            std::string pathStr = m_hdCompilerPath;
            while (!pathStr.empty() && (pathStr.back() == '/' || pathStr.back() == '\\')) {
                pathStr.pop_back();
            }
            std::filesystem::path compPath(pathStr);
            if (!std::filesystem::exists(compPath / "main.py") && std::filesystem::exists(compPath.parent_path() / "main.py")) {
                m_hdCompilerPath = compPath.parent_path().string();
                if (TerminalWindow::Instance) {
                    TerminalWindow::Instance->AppendLog("[Auto-Correct] Adjusted HD Compiler path to parent: " + m_hdCompilerPath + "\n");
                }
            }
        }

        if (m_hdCompilerPath.empty()) {
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("[Error] HD Compiler path not set. Use Run > Set HD Compiler Path.\n");
            }
            return;
        }
        std::string runCmd = m_hdRunCommandBuf;
        if (runCmd.empty()) {
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("[Error] HD Run Command not set. Use Run > Edit HD Run Command.\n");
            }
            return;
        }
        cmd = "cd \"" + m_hdCompilerPath + "\" && " + runCmd + " \"" + currentPath + "\" 2>&1";
    }
    else {
        if (TerminalWindow::Instance) {
            TerminalWindow::Instance->AppendLog("[Info] Unsupported file type: " + ext + ". Only .rsc and .asm are supported.\n");
        }
        return;
    }
    
#if defined(__ANDROID__) || defined(__IOS__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    if (TerminalWindow::Instance) {
        TerminalWindow::Instance->AppendLog("[Error] Running external compilers is not supported on this mobile platform.\n");
    }
    return;
#endif
    
    if (CompilerOutputWindow::Instance) {
        CompilerOutputWindow::Instance->Clear();
    }
    
    if (m_isCompiling) {
        if (TerminalWindow::Instance) {
            TerminalWindow::Instance->AppendLog("[Error] Compilation already in progress. Please wait.\n");
        }
        return;
    }
    
    if (TerminalWindow::Instance) {
        TerminalWindow::Instance->AppendLog("--- RUN: " + filePath.filename().string() + " ---\n$ " + cmd + "\n");
    }
    
    if (m_compileThread.joinable()) {
        m_compileThread.join();
    }
    m_isCompiling = true;
    m_compileFinished = false;
    m_compileOutput.clear();
    
    m_compileThread = std::thread([this, cmd]() {
        std::string compilerOutput;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[256];
            while (m_isCompiling && fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                if (TerminalWindow::Instance) {
                    TerminalWindow::Instance->AppendLog(buffer);
                }
                compilerOutput += buffer;
            }
            int status = pclose(pipe);
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("--- FINISHED (exit " + std::to_string(status) + ") ---\n");
            }
        } else {
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("[Error] Failed to execute command.\n");
            }
        }
        m_compileOutput = compilerOutput;
        m_compileFinished = true;
        m_isCompiling = false;
    });
}

void RunDebugWindow::Update() {
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
        RunCurrentFile();
    }
    if (m_compileFinished) {
        m_compileFinished = false;
        if (CompilerOutputWindow::Instance) {
            CompilerOutputWindow::Instance->SetOutputText(m_compileOutput);
            ImGui::SetWindowFocus("Compiler Output");
        }
    }
}

// ================= WORKSPACE EXPLORER =================
WorkspaceExplorer* WorkspaceExplorer::Instance = nullptr;

WorkspaceExplorer::WorkspaceExplorer() : UIWindow("Workspace Explorer") {
    Instance = this;
}

void WorkspaceExplorer::SetWorkspaceRoot(const std::string& root) {
    m_workspaceRoot = root;
    m_forceRefresh = true;
    SaveUIState();
    if (TerminalWindow::Instance) {
        TerminalWindow::Instance->OnWorkspaceChanged(root);
    }
}

WorkspaceExplorer::FileNode WorkspaceExplorer::BuildNode(const std::string& path) {
    FileNode node;
    node.path = path;
    node.filename = std::filesystem::path(path).filename().string();
    node.is_directory = std::filesystem::is_directory(path);
    
    if (node.is_directory) {
        std::vector<FileNode> dirs;
        std::vector<FileNode> files;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            std::string filename = entry.path().filename().string();
            if (filename == ".git" || filename == "__pycache__" || filename == "build" || filename == ".DS_Store") continue;
            
            if (entry.is_directory()) {
                dirs.push_back(BuildNode(entry.path().string()));
            } else {
                FileNode fileNode;
                fileNode.path = entry.path().string();
                fileNode.filename = filename;
                fileNode.is_directory = false;
                files.push_back(fileNode);
            }
        }
        
        auto sortFunc = [](const FileNode& a, const FileNode& b) {
            return a.filename < b.filename;
        };
        std::sort(dirs.begin(), dirs.end(), sortFunc);
        std::sort(files.begin(), files.end(), sortFunc);
        
        node.children.insert(node.children.end(), dirs.begin(), dirs.end());
        node.children.insert(node.children.end(), files.begin(), files.end());
    }
    return node;
}

void WorkspaceExplorer::RefreshCache() {
    if (m_workspaceRoot.empty() || !std::filesystem::exists(m_workspaceRoot)) return;
    
    std::vector<FileNode> dirs;
    std::vector<FileNode> files;
    
    for (const auto& entry : std::filesystem::directory_iterator(m_workspaceRoot)) {
        std::string filename = entry.path().filename().string();
        if (filename == ".git" || filename == "__pycache__" || filename == "build" || filename == ".DS_Store") continue;
        
        if (entry.is_directory()) {
            dirs.push_back(BuildNode(entry.path().string()));
        } else {
            FileNode fileNode;
            fileNode.path = entry.path().string();
            fileNode.filename = filename;
            fileNode.is_directory = false;
            files.push_back(fileNode);
        }
    }
    
    auto sortFunc = [](const FileNode& a, const FileNode& b) {
        return a.filename < b.filename;
    };
    std::sort(dirs.begin(), dirs.end(), sortFunc);
    std::sort(files.begin(), files.end(), sortFunc);
    
    m_cachedDirs = dirs;
    m_cachedFiles = files;
    m_lastRefreshTime = ImGui::GetTime();
    m_forceRefresh = false;
}

void WorkspaceExplorer::DrawNode(const FileNode& node) {
    if (node.is_directory) {
        if (ImGui::TreeNode(node.filename.c_str())) {
            for (const auto& child : node.children) {
                DrawNode(child);
            }
            ImGui::TreePop();
        }
    } else {
        if (ImGui::Selectable(node.filename.c_str())) {
            OpenFileInEditor(node.path);
        }
    }
    
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Copy Path")) {
            ImGui::SetClipboardText(node.path.c_str());
        }
        if (ImGui::MenuItem("Delete")) {
            try {
                if (node.is_directory) std::filesystem::remove_all(node.path);
                else std::filesystem::remove(node.path);
                m_forceRefresh = true;
            } catch (const std::exception& e) {
            }
        }
        ImGui::EndPopup();
    }
}

void WorkspaceExplorer::RenderCore() {
    if (m_workspaceRoot.empty()) {
        ImGui::TextWrapped("No folder open.");
        ImGui::Spacing();
        if (ImGui::Button("Open Folder", ImVec2(-1, 0))) {
            SystemDialogs::OpenFolderDialog([this](std::filesystem::path p) {
                m_workspaceRoot = p.string();
                m_forceRefresh = true;
            });
        }
        return;
    }
    
    if (m_forceRefresh || (ImGui::GetTime() - m_lastRefreshTime > 2.0)) {
        RefreshCache();
    }
    
    std::filesystem::path rootPath(m_workspaceRoot);
    ImGui::TextDisabled("%s", rootPath.filename().string().c_str());
    ImGui::Separator();
    
    for (const auto& dir : m_cachedDirs) {
        DrawNode(dir);
    }
    for (const auto& file : m_cachedFiles) {
        DrawNode(file);
    }
}

// ================= TERMINAL WINDOW =================
TerminalInstance::TerminalInstance(const std::string& shellPath) : m_scrollToBottom(false), m_pendingCarriageReturn(false), m_shellPath(shellPath) {
    memset(m_inputBuf, 0, sizeof(m_inputBuf));
    m_currentStyleColor = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    m_currentStyleBold = false;
    
    m_logLines.push_back(LogLine());
    SpawnProcess();
}

TerminalInstance::~TerminalInstance() {
    CleanupProcess();
}

void TerminalInstance::SetShellPath(const std::string& path) {
    m_shellPath = path;
    RestartShell();
}

void TerminalInstance::RestartShell() {
    CleanupProcess();
    ClearLog();
    SpawnProcess();
}

void TerminalInstance::SpawnProcess() {
#if defined(__IOS__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    AppendLog("[Error] Terminal is not supported on iOS because process execution is forbidden.\n");
    return;
#endif

#ifndef _WIN32
    m_ptyMasterFd = -1;
    m_ptyPid = -1;

    // Spawn interactive shell using forkpty
    int master;
    struct winsize ws;
    ws.ws_col = 80;
    ws.ws_row = 24;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        AppendLog("[Error] Failed to spawn process. Check permissions or environment.\n");
        return;
    }
    if (pid == 0) {
        // Child shell process starts in default directory
        setenv("TERM", "xterm-256color", 1);
        
        // Execute the user configured shell or fall back to standard shells
        if (m_shellPath.find(' ') != std::string::npos) {
            execl("/bin/sh", "sh", "-c", m_shellPath.c_str(), NULL);
        } else {
            execl(m_shellPath.c_str(), m_shellPath.c_str(), "-l", NULL);
            execl(m_shellPath.c_str(), m_shellPath.c_str(), NULL);
        }
        
        execl("/bin/zsh", "zsh", "-l", NULL);
        execl("/bin/bash", "bash", "-l", NULL);
        execl("/bin/sh", "sh", NULL);
        execl("/system/bin/sh", "sh", NULL);
        exit(1);
    } else if (pid > 0) {
        m_ptyMasterFd = master;
        m_ptyPid = pid;
        
        // Put master fd in non-blocking mode
        int flags = fcntl(m_ptyMasterFd, F_GETFL, 0);
        fcntl(m_ptyMasterFd, F_SETFL, flags | O_NONBLOCK);
        
        m_isRunning = true;
        // Start background reading thread
        m_readThread = std::thread([this]() {
            char buf[4096];
            while (m_isRunning && m_ptyMasterFd != -1) {
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(m_ptyMasterFd, &readfds);
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000; // 100ms
                
                int select_res = select(m_ptyMasterFd + 1, &readfds, NULL, NULL, &timeout);
                if (select_res > 0) {
                    ssize_t n = read(m_ptyMasterFd, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        AppendLogFromPty(buf);
                    } else if (n == 0) {
                        break;
                    }
                } else if (select_res < 0) {
                    if (errno != EINTR) {
                        break;
                    }
                }
            }
        });
    }
#else
    m_hChildStdInWrite = nullptr;
    m_hChildStdOutRead = nullptr;
    m_hProcess = nullptr;
    m_hPC = nullptr;

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    PFNCREATEPSEUDOCONSOLE pfnCreatePseudoConsole = (PFNCREATEPSEUDOCONSOLE)GetProcAddress(hKernel32, "CreatePseudoConsole");
    PFNCLOSEPSEUDOCONSOLE pfnClosePseudoConsole = (PFNCLOSEPSEUDOCONSOLE)GetProcAddress(hKernel32, "ClosePseudoConsole");

    bool useConPTY = false;

    if (pfnCreatePseudoConsole && pfnClosePseudoConsole) {
        HANDLE hPipeInSideA = INVALID_HANDLE_VALUE;  // parent write
        HANDLE hPipeInSideB = INVALID_HANDLE_VALUE;  // child read (conpty input)
        HANDLE hPipeOutSideA = INVALID_HANDLE_VALUE; // parent read
        HANDLE hPipeOutSideB = INVALID_HANDLE_VALUE; // child write (conpty output)

        if (CreatePipe(&hPipeInSideB, &hPipeInSideA, NULL, 0) &&
            CreatePipe(&hPipeOutSideA, &hPipeOutSideB, NULL, 0)) 
        {
            HPCON hPC = INVALID_HANDLE_VALUE;
            COORD size = { 80, 24 };
            HRESULT hr = pfnCreatePseudoConsole(size, hPipeInSideB, hPipeOutSideB, 0, &hPC);
            if (SUCCEEDED(hr)) {
                CloseHandle(hPipeInSideB);
                CloseHandle(hPipeOutSideB);

                STARTUPINFOEXW siEx;
                ZeroMemory(&siEx, sizeof(STARTUPINFOEXW));
                siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
                siEx.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
                siEx.StartupInfo.wShowWindow = SW_HIDE;

                SIZE_T attrListSize = 0;
                InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
                
                std::vector<BYTE> attrList(attrListSize);
                siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrList.data());

                if (InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrListSize)) {
                    if (UpdateProcThreadAttribute(
                            siEx.lpAttributeList,
                            0,
                            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                            hPC,
                            sizeof(HPCON),
                            NULL,
                            NULL)) 
                    {
                        PROCESS_INFORMATION piProcInfo;
                        ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

                        int wLen = MultiByteToWideChar(CP_UTF8, 0, m_shellPath.c_str(), -1, NULL, 0);
                        std::vector<wchar_t> wCmd(wLen);
                        MultiByteToWideChar(CP_UTF8, 0, m_shellPath.c_str(), -1, &wCmd[0], wLen);

                        BOOL bSuccess = CreateProcessW(
                            NULL,
                            wCmd.data(),
                            NULL,
                            NULL,
                            FALSE,
                            EXTENDED_STARTUPINFO_PRESENT,
                            NULL,
                            NULL,
                            &siEx.StartupInfo,
                            &piProcInfo
                        );

                        if (bSuccess) {
                            m_hProcess = piProcInfo.hProcess;
                            m_hChildStdInWrite = hPipeInSideA;
                            m_hChildStdOutRead = hPipeOutSideA;
                            m_hPC = hPC;
                            CloseHandle(piProcInfo.hThread);

                            m_isRunning = true;
                            // Start background reading thread
                            m_readThread = std::thread([this]() {
                                char buf[4096];
                                DWORD dwRead;
                                while (m_isRunning && m_hChildStdOutRead != nullptr) {
                                    if (ReadFile((HANDLE)m_hChildStdOutRead, buf, sizeof(buf) - 1, &dwRead, NULL) && dwRead > 0) {
                                        buf[dwRead] = '\0';
                                        AppendLogFromPty(buf);
                                    } else {
                                        break;
                                    }
                                }
                            });

                            DeleteProcThreadAttributeList(siEx.lpAttributeList);
                            useConPTY = true;
                        }
                    }
                    if (!useConPTY) {
                        DeleteProcThreadAttributeList(siEx.lpAttributeList);
                    }
                }
                if (!useConPTY) {
                    pfnClosePseudoConsole(hPC);
                }
            } else {
                CloseHandle(hPipeInSideB);
                CloseHandle(hPipeOutSideB);
            }
            if (!useConPTY) {
                CloseHandle(hPipeInSideA);
                CloseHandle(hPipeOutSideA);
            }
        }
    }

    if (!useConPTY) {
        // Fallback to redirected pipes
        HANDLE hChildStdInRead, hChildStdInWrite;
        HANDLE hChildStdOutRead, hChildStdOutWrite;

        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        if (CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &saAttr, 0)) {
            if (SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
                if (CreatePipe(&hChildStdInRead, &hChildStdInWrite, &saAttr, 0)) {
                    if (SetHandleInformation(hChildStdInWrite, HANDLE_FLAG_INHERIT, 0)) {
                        m_hChildStdInWrite = hChildStdInWrite;
                        m_hChildStdOutRead = hChildStdOutRead;

                        STARTUPINFOW siStartInfo;
                        PROCESS_INFORMATION piProcInfo;
                        ZeroMemory(&siStartInfo, sizeof(STARTUPINFOW));
                        siStartInfo.cb = sizeof(STARTUPINFOW);
                        siStartInfo.hStdError = hChildStdOutWrite;
                        siStartInfo.hStdOutput = hChildStdOutWrite;
                        siStartInfo.hStdInput = hChildStdInRead;
                        siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

                        ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

                        int wLen = MultiByteToWideChar(CP_UTF8, 0, m_shellPath.c_str(), -1, NULL, 0);
                        std::vector<wchar_t> wCmd(wLen);
                        MultiByteToWideChar(CP_UTF8, 0, m_shellPath.c_str(), -1, &wCmd[0], wLen);

                        BOOL bSuccess = CreateProcessW(
                            NULL, 
                            &wCmd[0], 
                            NULL, 
                            NULL, 
                            TRUE, 
                            CREATE_NO_WINDOW, 
                            NULL, 
                            NULL, 
                            &siStartInfo, 
                            &piProcInfo
                        );

                        if (bSuccess) {
                            m_hProcess = piProcInfo.hProcess;
                            CloseHandle(piProcInfo.hThread);
                            CloseHandle(hChildStdOutWrite);
                            CloseHandle(hChildStdInRead);

                            m_isRunning = true;
                            m_readThread = std::thread([this]() {
                                char buf[4096];
                                DWORD dwRead;
                                while (m_isRunning && m_hChildStdOutRead != nullptr) {
                                    if (ReadFile((HANDLE)m_hChildStdOutRead, buf, sizeof(buf) - 1, &dwRead, NULL) && dwRead > 0) {
                                        buf[dwRead] = '\0';
                                        AppendLogFromPty(buf);
                                    } else {
                                        break;
                                    }
                                }
                            });
                        } else {
                            CloseHandle(hChildStdOutWrite);
                            CloseHandle(hChildStdInRead);
                            CloseHandle(hChildStdOutRead);
                            CloseHandle(hChildStdInWrite);
                            m_hChildStdInWrite = nullptr;
                            m_hChildStdOutRead = nullptr;
                            AppendLog("[Error] Failed to create Windows shell process: " + m_shellPath + "\n");
                        }
                    } else {
                        CloseHandle(hChildStdOutWrite);
                        CloseHandle(hChildStdInRead);
                        CloseHandle(hChildStdOutRead);
                        CloseHandle(hChildStdInWrite);
                    }
                } else {
                    CloseHandle(hChildStdOutWrite);
                    CloseHandle(hChildStdOutRead);
                }
            } else {
                CloseHandle(hChildStdOutWrite);
                CloseHandle(hChildStdOutRead);
            }
        }
    }
#endif
}

void TerminalInstance::CleanupProcess() {
    m_isRunning = false;
#ifndef _WIN32
    if (m_ptyPid > 0) {
        kill(m_ptyPid, SIGKILL);
        m_ptyPid = -1;
    }
    if (m_ptyMasterFd != -1) {
        close(m_ptyMasterFd);
        m_ptyMasterFd = -1;
    }
#else
    if (m_hProcess != nullptr) {
        TerminateProcess((HANDLE)m_hProcess, 1);
        CloseHandle((HANDLE)m_hProcess);
        m_hProcess = nullptr;
    }
    if (m_hChildStdOutRead != nullptr) {
        CloseHandle((HANDLE)m_hChildStdOutRead);
        m_hChildStdOutRead = nullptr;
    }
    if (m_hChildStdInWrite != nullptr) {
        CloseHandle((HANDLE)m_hChildStdInWrite);
        m_hChildStdInWrite = nullptr;
    }
    if (m_hPC != nullptr) {
        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        PFNCLOSEPSEUDOCONSOLE pfnClosePseudoConsole = (PFNCLOSEPSEUDOCONSOLE)GetProcAddress(hKernel32, "ClosePseudoConsole");
        if (pfnClosePseudoConsole) {
            pfnClosePseudoConsole(m_hPC);
        }
        m_hPC = nullptr;
    }
#endif
    if (m_readThread.joinable()) {
        m_readThread.join();
    }
}

void TerminalInstance::AppendLog(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    ParseAndAddText(text);
    m_scrollToBottom = true;
}

void TerminalInstance::ClearLog() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logLines.clear();
    m_logLines.push_back(LogLine());
}

void TerminalInstance::SendInput(const std::string& input) {
#ifndef _WIN32
    if (m_ptyMasterFd != -1) {
        write(m_ptyMasterFd, input.c_str(), input.size());
    }
#else
    if (m_hChildStdInWrite != nullptr) {
        if (m_hPC == nullptr) {
            AppendLog(input);
        }
        DWORD dwWritten;
        WriteFile((HANDLE)m_hChildStdInWrite, input.c_str(), (DWORD)input.size(), &dwWritten, NULL);
    }
#endif
}

void TerminalInstance::AppendLogFromPty(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    ParseAndAddText(text);
    m_scrollToBottom = true;
}

void TerminalInstance::ParseAndAddText(const std::string& rawText) {
    std::string textAccum;
    auto flushAccum = [this, &textAccum]() {
        if (!textAccum.empty()) {
            AddStringToCurrentLine(textAccum, m_currentStyleColor);
            textAccum.clear();
        }
    };
    
    for (size_t i = 0; i < rawText.size(); ++i) {
        char c = rawText[i];
        
        if (m_pendingCarriageReturn) {
            if (c == '\n') {
                m_pendingCarriageReturn = false;
            } else {
                if (!m_logLines.empty()) {
                    m_logLines.back().segments.clear();
                }
                m_pendingCarriageReturn = false;
            }
        }
        
        if (c == '\x1b') {
            if (i + 1 < rawText.size() && rawText[i + 1] == '[') {
                flushAccum();
                size_t start = i + 2;
                size_t end = start;
                while (end < rawText.size() && rawText[end] != 'm' && !((rawText[end] >= 'A' && rawText[end] <= 'Z') || (rawText[end] >= 'a' && rawText[end] <= 'z'))) {
                    end++;
                }
                
                if (end < rawText.size() && rawText[end] == 'm') {
                    std::string paramsStr = rawText.substr(start, end - start);
                    std::vector<int> params;
                    std::stringstream ss(paramsStr);
                    std::string param;
                    while (std::getline(ss, param, ';')) {
                        if (!param.empty()) {
                            try {
                                params.push_back(std::stoi(param));
                            } catch (...) {}
                        }
                    }
                    if (params.empty()) {
                        params.push_back(0);
                    }
                    
                    for (int p : params) {
                        if (p == 0) {
                            m_currentStyleColor = ImGui::GetStyle().Colors[ImGuiCol_Text];
                            m_currentStyleBold = false;
                        } else if (p == 1) {
                            m_currentStyleBold = true;
                        } else if (p >= 30 && p <= 37) {
                            int colorIndex = p - 30;
                            static const ImVec4 ansiPalette[] = {
                                ImVec4(0.0f, 0.0f, 0.0f, 1.0f), ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                                ImVec4(0.15f, 0.85f, 0.15f, 1.0f), ImVec4(0.85f, 0.85f, 0.15f, 1.0f),
                                ImVec4(0.15f, 0.45f, 0.95f, 1.0f), ImVec4(0.85f, 0.15f, 0.85f, 1.0f),
                                ImVec4(0.15f, 0.85f, 0.85f, 1.0f), ImVec4(0.95f, 0.95f, 0.95f, 1.0f)
                            };
                            m_currentStyleColor = ansiPalette[colorIndex];
                        } else if (p >= 90 && p <= 97) {
                            int colorIndex = p - 90;
                            static const ImVec4 ansiPaletteBright[] = {
                                ImVec4(0.4f, 0.4f, 0.4f, 1.0f), ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                ImVec4(0.35f, 1.0f, 0.35f, 1.0f), ImVec4(1.0f, 1.0f, 0.35f, 1.0f),
                                ImVec4(0.35f, 0.65f, 1.0f, 1.0f), ImVec4(1.0f, 0.35f, 1.0f, 1.0f),
                                ImVec4(0.35f, 1.0f, 1.0f, 1.0f), ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                            };
                            m_currentStyleColor = ansiPaletteBright[colorIndex];
                        } else if (p == 39) {
                            m_currentStyleColor = ImGui::GetStyle().Colors[ImGuiCol_Text];
                        }
                    }
                }
                i = end;
                continue;
            }
        }
        
        if (c == '\n') {
            flushAccum();
            m_logLines.push_back(LogLine());
            if (m_logLines.size() > m_maxLines) {
                m_logLines.erase(m_logLines.begin());
            }
        } else if (c == '\r') {
            flushAccum();
            m_pendingCarriageReturn = true;
        } else if (c == '\b') {
            flushAccum();
            if (!m_logLines.empty() && !m_logLines.back().segments.empty()) {
                auto& lastSeg = m_logLines.back().segments.back();
                if (!lastSeg.text.empty()) {
                    lastSeg.text.pop_back();
                    if (lastSeg.text.empty()) {
                        m_logLines.back().segments.pop_back();
                    }
                }
            }
        } else if (c != '\a') {
            textAccum.push_back(c);
        }
    }
    flushAccum();
}

void TerminalInstance::AddStringToCurrentLine(const std::string& str, const ImVec4& color) {
    if (m_logLines.empty()) {
        m_logLines.push_back(LogLine());
    }
    auto& line = m_logLines.back();
    if (!line.segments.empty() && 
        line.segments.back().color.x == color.x &&
        line.segments.back().color.y == color.y &&
        line.segments.back().color.z == color.z &&
        line.segments.back().color.w == color.w) {
        line.segments.back().text += str;
    } else {
        TextSegment seg;
        seg.text = str;
        seg.color = color;
        line.segments.push_back(seg);
    }
}

void TerminalInstance::Render(int index) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
    
    std::string childId = "LogRegion_" + std::to_string(index);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    ImGui::BeginChild(childId.c_str(), ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NavFlattened);
    ImGui::PopStyleColor();

    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
    }
    
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        ImGuiIO& io = ImGui::GetIO();
        for (int n = 0; n < io.InputQueueCharacters.Size; n++) {
            ImWchar c = io.InputQueueCharacters[n];
            if (c >= 32 && c <= 126) {
                std::string s(1, (char)c);
                SendInput(s);
            }
        }
        
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            SendInput("\r");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            SendInput("\b");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            SendInput("\t");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            SendInput("\x1b[A");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            SendInput("\x1b[B");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            SendInput("\x1b[C");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            SendInput("\x1b[D");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            SendInput("\x1b");
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
            SendInput("\x03");
        }
    }

    std::lock_guard<std::mutex> lock(m_logMutex);
    for (const auto& line : m_logLines) {
        if (line.segments.empty()) {
            ImGui::NewLine();
        } else {
            for (size_t i = 0; i < line.segments.size(); ++i) {
                const auto& seg = line.segments[i];
                ImGui::TextColored(seg.color, "%s", seg.text.c_str());
                if (i < line.segments.size() - 1) {
                    ImGui::SameLine(0.0f, 0.0f);
                }
            }
        }
    }
    
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        if ((int)(ImGui::GetTime() * 2) % 2 == 0) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "\xe2\x96\x88"); // Block cursor
        }
    }
    
    if (m_scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        m_scrollToBottom = false;
    }
    ImGui::EndChild();
    
    ImGui::PopStyleVar();
}

// ================= TERMINAL WINDOW MANAGER =================
TerminalWindow* TerminalWindow::Instance = nullptr;

TerminalWindow::TerminalWindow() : UIWindow("Terminal") {
    Instance = this;
    AddNewTerminal();

    if (WorkspaceExplorer::Instance && !WorkspaceExplorer::Instance->GetWorkspaceRoot().empty()) {
        OnWorkspaceChanged(WorkspaceExplorer::Instance->GetWorkspaceRoot());
    }
}

TerminalWindow::~TerminalWindow() {
    m_terminals.clear();
}

void TerminalWindow::AddNewTerminal(const std::string& shellPath) {
    std::string finalShell = shellPath;
    if (finalShell.empty()) {
#ifdef __ANDROID__
        finalShell = "/system/bin/sh";
#elif defined(_WIN32)
        finalShell = "cmd.exe";
#else
        finalShell = "/bin/zsh";
#endif
    }
    m_terminals.push_back(std::make_unique<TerminalInstance>(finalShell));
    m_activeTerminalIdx = (int)m_terminals.size() - 1;
}

TerminalInstance* TerminalWindow::GetActiveTerminal() {
    if (m_activeTerminalIdx >= 0 && m_activeTerminalIdx < (int)m_terminals.size()) {
        return m_terminals[m_activeTerminalIdx].get();
    }
    return nullptr;
}

void TerminalWindow::SetShellPath(const std::string& path) {
    if (auto* term = GetActiveTerminal()) {
        term->SetShellPath(path);
        SaveUIState();
    }
}

std::string TerminalWindow::GetShellPath() const {
    if (m_activeTerminalIdx >= 0 && m_activeTerminalIdx < (int)m_terminals.size()) {
        return m_terminals[m_activeTerminalIdx]->GetShellPath();
    }
#ifdef __ANDROID__
    return "/system/bin/sh";
#elif defined(_WIN32)
    return "cmd.exe";
#else
    return "/bin/zsh";
#endif
}

void TerminalWindow::RestartShell() {
    if (auto* term = GetActiveTerminal()) {
        term->RestartShell();
    }
}

void TerminalWindow::AppendLog(const std::string& text) {
    if (m_terminals.empty()) AddNewTerminal();
    if (auto* term = GetActiveTerminal()) {
        term->AppendLog(text);
    }
}

void TerminalWindow::ClearLog() {
    if (auto* term = GetActiveTerminal()) {
        term->ClearLog();
    }
}

void TerminalWindow::SendInput(const std::string& input) {
    if (auto* term = GetActiveTerminal()) {
        term->SendInput(input);
    }
}

void TerminalWindow::OnWorkspaceChanged(const std::string& newPath) {
    if (newPath.empty()) return;
    for (auto& term : m_terminals) {
        if (term->GetShellPath().rfind("ssh", 0) == 0) continue;
#ifdef _WIN32
        term->SendInput("cd /d \"" + newPath + "\"\n");
#else
        term->SendInput("cd \"" + newPath + "\"\n");
#endif
    }
}

void TerminalWindow::RenderCore() {
    float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginGroup();
    
    ImGui::SetNextItemWidth(40.0f);
    if (ImGui::BeginCombo("##AddTerminalCombo", "+", ImGuiComboFlags_NoArrowButton)) {
#ifdef __ANDROID__
        if (ImGui::Selectable("sh")) { AddNewTerminal("/system/bin/sh"); }
        if (ImGui::Selectable("bash (Termux)")) { AddNewTerminal("/data/data/com.termux/files/usr/bin/bash"); }
#elif defined(_WIN32)
        if (ImGui::Selectable("cmd.exe")) { AddNewTerminal("cmd.exe"); }
        if (ImGui::Selectable("powershell.exe")) { AddNewTerminal("powershell.exe"); }
        if (ImGui::Selectable("bash (Git Bash)")) { AddNewTerminal("C:\\Program Files\\Git\\bin\\bash.exe"); }
        if (ImGui::Selectable("wsl.exe")) { AddNewTerminal("wsl.exe"); }
#else
        if (ImGui::Selectable("zsh")) { AddNewTerminal("/bin/zsh"); }
        if (ImGui::Selectable("bash")) { AddNewTerminal("/bin/bash"); }
        if (ImGui::Selectable("sh")) { AddNewTerminal("/bin/sh"); }
#endif
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New Terminal Profile");
    
    UIHelpers::WrapSameLine(80.0f);
    if (ImGui::Button("-##ClearAllTerminals") && !m_terminals.empty() && m_activeTerminalIdx >= 0) {
        m_terminals[m_activeTerminalIdx]->m_shouldClose = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Kill Terminal");

    ImGui::EndGroup();
    UIHelpers::WrapSameLine(100.0f);

    if (m_terminals.empty()) {
        ImGui::TextDisabled("No active terminals");
        return;
    }

    if (ImGui::BeginTabBar("TerminalTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        for (int i = 0; i < (int)m_terminals.size(); ++i) {
            bool open = true;
            std::string tabName = "Term " + std::to_string(i + 1);
            std::string path = m_terminals[i]->GetShellPath();
            if (path.find("cmd.exe") != std::string::npos) tabName += " (cmd)";
            else if (path.find("powershell") != std::string::npos) tabName += " (pwsh)";
            else if (path.find("bash") != std::string::npos) tabName += " (bash)";
            else if (path.find("zsh") != std::string::npos) tabName += " (zsh)";
            
            tabName += "###TermTab_" + std::to_string(i);

            ImGuiTabItemFlags flags = (i == m_activeTerminalIdx) ? ImGuiTabItemFlags_SetSelected : 0;
            
            if (ImGui::BeginTabItem(tabName.c_str(), &open, flags)) {
                m_activeTerminalIdx = i;
                m_terminals[i]->Render(i);
                ImGui::EndTabItem();
            }

            if (!open) m_terminals[i]->m_shouldClose = true;
        }
        ImGui::EndTabBar();
    }

    // Cleanup closed tabs
    for (auto it = m_terminals.begin(); it != m_terminals.end(); ) {
        if ((*it)->m_shouldClose) {
            it = m_terminals.erase(it);
        } else {
            ++it;
        }
    }
    
    if (m_activeTerminalIdx >= (int)m_terminals.size()) {
        m_activeTerminalIdx = (int)m_terminals.size() - 1;
    }
}

// ================= COMPILER OUTPUT WINDOW =================
CompilerOutputWindow* CompilerOutputWindow::Instance = nullptr;

static std::string strip_ansi_escapes(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    bool in_escape = false;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\x1b' || input[i] == '\033') {
            in_escape = true;
            if (i + 1 < input.size() && input[i + 1] == '[') {
                i++;
            }
            continue;
        }
        if (in_escape) {
            if ((input[i] >= 'a' && input[i] <= 'z') || (input[i] >= 'A' && input[i] <= 'Z')) {
                in_escape = false;
            }
            continue;
        }
        output.push_back(input[i]);
    }
    return output;
}

static bool is_valid_hex_address(const std::string& str, uint32_t& addr) {
    if (str.empty()) return false;
    std::string clean = str;
    if (clean.length() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
        clean = clean.substr(2);
    }
    if (clean.empty()) return false;
    for (char c : clean) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    try {
        addr = std::stoul(str, nullptr, 16);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_hex_bytes_line(const std::string& str, std::vector<uint8_t>& bytes) {
    std::stringstream byteStream(str);
    std::string byteStr;
    bool found_any = false;
    while (byteStream >> byteStr) {
        if (byteStr.empty() || byteStr.length() > 2) {
            return false;
        }
        for (char c : byteStr) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        try {
            uint8_t val = (uint8_t)std::stoul(byteStr, nullptr, 16);
            bytes.push_back(val);
            found_any = true;
        } catch (...) {
            return false;
        }
    }
    return found_any;
}

CompilerOutputWindow::CompilerOutputWindow() : UIWindow("Compiler Output") {
    Instance = this;
    m_outputText.clear();
    m_rawBytesText.clear();
    m_injections.clear();
}

void CompilerOutputWindow::Clear() {
    m_outputText.clear();
    m_rawBytesText.clear();
    m_injections.clear();
}

void CompilerOutputWindow::SetOutputText(const std::string& text) {
    m_outputText = text;
    ParseOutput();
}

void CompilerOutputWindow::ParseOutput() {
    m_injections.clear();
    m_rawBytesText.clear();
    
    std::string cleanText = strip_ansi_escapes(m_outputText);
    std::stringstream ss(cleanText);
    std::string line;
    bool in_byte_block = false;
    uint32_t injectAddress = 0;
    std::vector<uint8_t> compiledBytes;
    
    while (std::getline(ss, line)) {
        // Trim leading and trailing whitespace
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(first, last - first + 1);
        
        // Look for the block header line starting with === or --- and containing ->
        bool is_header = false;
        if (trimmed.find("->") != std::string::npos) {
            if (trimmed.rfind("===", 0) == 0 || trimmed.rfind("---", 0) == 0) {
                is_header = true;
            }
        }
        
        if (is_header) {
            if (in_byte_block && !compiledBytes.empty()) {
                ParsedInjection inj;
                inj.address = injectAddress;
                inj.bytes = compiledBytes;
                snprintf(inj.injectAddressStr, sizeof(inj.injectAddressStr), "0x%04X", injectAddress);
                m_injections.push_back(inj);
                compiledBytes.clear();
            }
            
            in_byte_block = true;
            
            // Extract inject address
            // Check for parenthesis: e.g. === 0xd730 -> 0xd7da (0xe9e0 -> EA8A) ===
            size_t parenOpen = trimmed.find('(');
            size_t parenClose = trimmed.find(')');
            if (parenOpen != std::string::npos && parenClose != std::string::npos && parenClose > parenOpen) {
                std::string parenContent = trimmed.substr(parenOpen + 1, parenClose - parenOpen - 1);
                size_t arrowPos = parenContent.find("->");
                if (arrowPos != std::string::npos) {
                    std::string addrStr = parenContent.substr(0, arrowPos);
                    addrStr.erase(0, addrStr.find_first_not_of(" \t"));
                    addrStr.erase(addrStr.find_last_not_of(" \t") + 1);
                    try {
                        injectAddress = std::stoul(addrStr, nullptr, 16);
                    } catch (...) {}
                }
            } else {
                // No parenthesis: extract before first ->
                size_t prefixLen = 0;
                while (prefixLen < trimmed.length() && (trimmed[prefixLen] == '=' || trimmed[prefixLen] == '-')) prefixLen++;
                size_t arrowPos = trimmed.find("->");
                if (arrowPos != std::string::npos && arrowPos > prefixLen) {
                    std::string addrStr = trimmed.substr(prefixLen, arrowPos - prefixLen);
                    addrStr.erase(0, addrStr.find_first_not_of(" \t"));
                    addrStr.erase(addrStr.find_last_not_of(" \t") + 1);
                    try {
                        injectAddress = std::stoul(addrStr, nullptr, 16);
                    } catch (...) {}
                }
            }
            continue;
        }
        
        if (in_byte_block) {
            // End of block marker: line starting with === or --- but not containing ->
            bool is_end_marker = false;
            if (trimmed.rfind("===", 0) == 0 || trimmed.rfind("---", 0) == 0) {
                if (trimmed.find("->") == std::string::npos) {
                    is_end_marker = true;
                }
            }
            if (is_end_marker) {
                in_byte_block = false;
                if (!compiledBytes.empty()) {
                    ParsedInjection inj;
                    inj.address = injectAddress;
                    inj.bytes = compiledBytes;
                    snprintf(inj.injectAddressStr, sizeof(inj.injectAddressStr), "0x%04X", injectAddress);
                    m_injections.push_back(inj);
                    compiledBytes.clear();
                }
                continue;
            }
            
            // Parse line of bytes
            std::stringstream byteStream(trimmed);
            std::string byteStr;
            while (byteStream >> byteStr) {
                if (byteStr.length() >= 2) {
                    try {
                        size_t idx = 0;
                        uint8_t val = (uint8_t)std::stoul(byteStr, &idx, 16);
                        if (idx > 0) {
                            compiledBytes.push_back(val);
                        }
                    } catch (...) {}
                }
            }
        }
    }
    
    if (in_byte_block && !compiledBytes.empty()) {
        ParsedInjection inj;
        inj.address = injectAddress;
        inj.bytes = compiledBytes;
        snprintf(inj.injectAddressStr, sizeof(inj.injectAddressStr), "0x%04X", injectAddress);
        m_injections.push_back(inj);
    }
    
    // Fallback: if no blocks were found using the standard header pattern, look for <addr>: <bytes> lines
    if (m_injections.empty()) {
        std::stringstream ss2(cleanText);
        while (std::getline(ss2, line)) {
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            size_t last = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(first, last - first + 1);
            
            size_t colonPos = trimmed.find(':');
            if (colonPos != std::string::npos) {
                std::string left = trimmed.substr(0, colonPos);
                std::string right = trimmed.substr(colonPos + 1);
                
                // Trim left and right
                size_t l_first = left.find_first_not_of(" \t");
                size_t l_last = left.find_last_not_of(" \t");
                if (l_first == std::string::npos) continue;
                std::string left_trimmed = left.substr(l_first, l_last - l_first + 1);
                
                size_t r_first = right.find_first_not_of(" \t");
                size_t r_last = right.find_last_not_of(" \t");
                std::string right_trimmed = (r_first == std::string::npos) ? "" : right.substr(r_first, r_last - r_first + 1);
                
                uint32_t addr = 0;
                std::vector<uint8_t> bytes;
                if (is_valid_hex_address(left_trimmed, addr) && parse_hex_bytes_line(right_trimmed, bytes)) {
                    ParsedInjection inj;
                    inj.address = addr;
                    inj.bytes = bytes;
                    snprintf(inj.injectAddressStr, sizeof(inj.injectAddressStr), "0x%04X", addr);
                    m_injections.push_back(inj);
                }
            }
        }
    }
    
    // Save final parsed results
    if (!m_injections.empty()) {
        for (const auto& inj : m_injections) {
            for (uint8_t val : inj.bytes) {
                char hex_buf[4];
                snprintf(hex_buf, sizeof(hex_buf), "%02X ", val);
                m_rawBytesText += hex_buf;
            }
        }
    }
}

void CompilerOutputWindow::RenderCore() {
    bool hasInjections = !m_injections.empty();
    
    if (hasInjections) {
        // Render premium sections table at the top
        ImGui::Text("Parsed Injection Sections:");
        ImGui::Dummy(ImVec2(0, 3));
        
        if (ImGui::BeginTable("InjectionsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Original Target", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Custom Inject Addr", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            
            for (size_t i = 0; i < m_injections.size(); ++i) {
                auto& inj = m_injections[i];
                ImGui::TableNextRow();
                
                // Col 1: Section
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Block %zu", i + 1);
                
                // Col 2: Original Target Address
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%04X", inj.address);
                
                // Col 3: Size
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu B", inj.bytes.size());
                
                // Col 4: Custom Inject address field
                ImGui::TableSetColumnIndex(3);
                ImGui::PushID((int)i);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText("##AddrInput", inj.injectAddressStr, sizeof(inj.injectAddressStr));
                ImGui::PopID();
                
                // Col 5: Individual Inject Action
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID((int)i);
                if (ImGui::Button("Inject")) {
                    if (n_ram_buffer && me_mmu) {
                        uint32_t finalAddr = 0;
                        try {
                            finalAddr = std::stoul(inj.injectAddressStr, nullptr, 16);
                        } catch (...) {}
                        
                        if (finalAddr != 0 || inj.injectAddressStr[0] != '\0') {
                            for (size_t b = 0; b < inj.bytes.size(); ++b) {
                                me_mmu->WriteData(finalAddr + b, inj.bytes[b], 0);
                            }
                            if (TerminalWindow::Instance) {
                                char buf[256];
                                snprintf(buf, sizeof(buf), "[Compiler Output] Injected Block %zu (%zu bytes) to RAM at 0x%04X.\n", 
                                         i + 1, inj.bytes.size(), finalAddr);
                                TerminalWindow::Instance->AppendLog(buf);
                            }
                        }
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        
        ImGui::Dummy(ImVec2(0, 5));
        
        // Actions Row
        if (ImGui::Button("Inject All")) {
            if (n_ram_buffer && me_mmu) {
                size_t totalBytes = 0;
                for (size_t i = 0; i < m_injections.size(); ++i) {
                    auto& inj = m_injections[i];
                    uint32_t finalAddr = 0;
                    try {
                        finalAddr = std::stoul(inj.injectAddressStr, nullptr, 16);
                    } catch (...) {}
                    
                    for (size_t b = 0; b < inj.bytes.size(); ++b) {
                        me_mmu->WriteData(finalAddr + b, inj.bytes[b], 0);
                    }
                    totalBytes += inj.bytes.size();
                }
                if (TerminalWindow::Instance) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "[Compiler Output] Injected all %zu blocks (%zu bytes total) to their respective target addresses.\n", 
                             m_injections.size(), totalBytes);
                    TerminalWindow::Instance->AppendLog(buf);
                }
            }
        }
        
        UIHelpers::WrapSameLine(120.0f);
        if (ImGui::Button("Copy All Bytes")) {
            ImGui::SetClipboardText(m_rawBytesText.c_str());
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->AppendLog("[Compiler Output] Copied raw compiled bytes to clipboard.\n");
            }
        }
        
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 3));
    } else {
        ImGui::TextDisabled("No compiled injections available.");
    }
}
