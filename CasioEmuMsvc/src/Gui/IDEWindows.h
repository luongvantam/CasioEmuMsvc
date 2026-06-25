#pragma once
#include "Ui.hpp"
#include "imgui/TextEditor.h"
#include "imgui/imgui.h"
#include <string>
#include <mutex>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>

#ifdef _WIN32
using pid_t = int;
#endif

class CodeEditorWindow : public UIWindow {
public:
    CodeEditorWindow(const std::string& path);
    ~CodeEditorWindow();
    virtual void RenderCore() override;
    virtual void Render() override;
    void SaveFile();

    std::string m_filePath;
    TextEditor m_editor;
    bool m_isModified = false;
    bool m_firstTime = true;
    bool m_docked_once = false;
};

class RunDebugWindow {
public:
    RunDebugWindow();
    ~RunDebugWindow();
    void Update();
    
    void RunCurrentFile(class CodeEditorWindow* editor = nullptr);
    void SetCompilerPath(const std::string& type, const std::string& path);

    std::string GetRacCompilerPath() const { return m_racCompilerPath; }
    std::string GetHdCompilerPath() const { return m_hdCompilerPath; }

    static RunDebugWindow* Instance;

    char m_racRunCommandBuf[256] = "python3 main.py -f hex 580vnx";
    char m_hdRunCommandBuf[256] = "python3 main.py 580vnx";
    
    bool m_showRacCommandEditor = false;
    bool m_showHdCommandEditor = false;

private:
    std::string m_racCompilerPath;
    std::string m_hdCompilerPath;
    
    std::thread m_compileThread;
    std::atomic<bool> m_isCompiling{false};
    std::atomic<bool> m_compileFinished{false};
    std::string m_compileOutput;
};

class WorkspaceExplorer : public UIWindow {
public:
    WorkspaceExplorer();
    virtual void RenderCore() override;
    
    static WorkspaceExplorer* Instance;
    std::string GetWorkspaceRoot() const { return m_workspaceRoot; }
    void SetWorkspaceRoot(const std::string& root);
    
private:
    void DrawDirectory(const std::string& path);
    std::string m_workspaceRoot;

public:
    struct FileNode {
        std::string path;
        std::string filename;
        bool is_directory;
        std::vector<FileNode> children;
    };
    FileNode BuildNode(const std::string& path);
    void RefreshCache();
    void DrawNode(const FileNode& node);
    
    bool m_forceRefresh = true;
    double m_lastRefreshTime = 0.0;
    std::vector<FileNode> m_cachedDirs;
    std::vector<FileNode> m_cachedFiles;
};

class TerminalInstance {
public:
    TerminalInstance(const std::string& shellPath);
    ~TerminalInstance();

    void SpawnProcess();
    void CleanupProcess();
    void RestartShell();

    void AppendLog(const std::string& text);
    void ClearLog();
    void SendInput(const std::string& input);
    void SetShellPath(const std::string& path);
    std::string GetShellPath() const { return m_shellPath; }

    void Render(int index);
    bool m_shouldClose = false;

private:
    struct TextSegment {
        std::string text;
        ImVec4 color;
    };
    
    struct LogLine {
        std::vector<TextSegment> segments;
    };

    std::vector<LogLine> m_logLines;
    size_t m_maxLines = 1000;
    
    char m_inputBuf[1024];
    bool m_scrollToBottom;
    
#ifndef _WIN32
    int m_ptyMasterFd;
    pid_t m_ptyPid;
#else
    void* m_hChildStdInWrite;
    void* m_hChildStdOutRead;
    void* m_hProcess;
    void* m_hPC;
#endif

    std::mutex m_logMutex;
    
    ImVec4 m_currentStyleColor;
    bool m_currentStyleBold;
    bool m_pendingCarriageReturn;
    std::string m_shellPath;

    std::thread m_readThread;
    std::atomic<bool> m_isRunning{false};

    void AppendLogFromPty(const std::string& text);
    void ParseAndAddText(const std::string& rawText);
    void AddStringToCurrentLine(const std::string& str, const ImVec4& color);
};

class TerminalWindow : public UIWindow {
public:
    TerminalWindow();
    ~TerminalWindow();
    virtual void RenderCore() override;
    
    void AppendLog(const std::string& text);
    void ClearLog();
    void SendInput(const std::string& input);
    void OnWorkspaceChanged(const std::string& newPath);
    void SetShellPath(const std::string& path);
    std::string GetShellPath() const;
    void RestartShell();
    static TerminalWindow* Instance;

    TerminalInstance* GetActiveTerminal();
    
private:
    std::vector<std::unique_ptr<TerminalInstance>> m_terminals;
    int m_activeTerminalIdx = -1;
    void AddNewTerminal(const std::string& shellPath = "");
};

class CompilerOutputWindow : public UIWindow {
public:
    CompilerOutputWindow();
    virtual void RenderCore() override;
    
    void SetOutputText(const std::string& text);
    void Clear();

    static CompilerOutputWindow* Instance;

private:
    struct ParsedInjection {
        uint32_t address;
        std::vector<uint8_t> bytes;
        char injectAddressStr[32];
    };
    std::vector<ParsedInjection> m_injections;
    std::string m_outputText;
    std::string m_rawBytesText;
    
    void ParseOutput();
};
