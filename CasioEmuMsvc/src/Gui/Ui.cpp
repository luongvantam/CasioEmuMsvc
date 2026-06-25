#include "Ui.hpp"
#include "hex.hpp"
#include "5800FileSystem.h"
#include "AddressWindow.h"
#include "BitmapViewer.h"
#include "CallAnalysis.h"
#include "Chipset/Chipset.hpp"
#include "Chipset/MMU.hpp"
#include "CodeViewer.hpp"
#include "Editors.h"
#include "HwController.h"
#include "Injector.hpp"
#include "LabelFile.h"
#include "LabelViewer.h"
#include "MemBreakPoint.hpp"
#include "Random.hpp"
#include "Setting.h"
#include "SearchWindow.hpp"
#include "VariableWindow.h"
#include "WatchWindow.hpp"
#include "PluginLogWindow.hpp"
#include "SnapshotWindow.h"
#include "CalculatorWindow.h"
#include "ThemeManager.h"
#include "IDEWindows.h"
#include "SysDialog.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_sdlrenderer2.h"
#include <Gui.h>
#include <SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
/*#include <fstream>

void DebugLog(const std::string& msg) {
    static std::ofstream log("debug_log.txt", std::ios::app);
    if (log.is_open()) {
        log << msg << std::endl;
        log.flush();
    }
}*/

CodeEditorWindow* GetFocusedCodeEditor() {
    CodeEditorWindow* lastActive = nullptr;
    for (auto* w : windows) {
        if (CodeEditorWindow* editor = dynamic_cast<CodeEditorWindow*>(w)) {
            if (editor->open) {
                lastActive = editor;
            }
        }
    }
    return lastActive;
}

extern ImGuiID g_dock_main_id;
void OpenFileInEditor(const std::string& path) {
    if (path.empty()) {
        CodeEditorWindow* editor = GetFocusedCodeEditor();
        if (editor) editor->open = false;
        return;
    }
    
    for (auto* w : windows) {
        if (CodeEditorWindow* editor = dynamic_cast<CodeEditorWindow*>(w)) {
            if (editor->m_filePath == path) {
                editor->open = true;
                editor->BringToFront();
                return;
            }
        }
    }
    
    CodeEditorWindow* newEditor = new CodeEditorWindow(path);
    if (g_dock_main_id != 0) {
        ImGui::DockBuilderDockWindow(newEditor->name, g_dock_main_id);
    }
    windows.push_back(newEditor);
}

#ifdef ENABLE_SENTRY
#include <sentry.h>
#endif
#include <sdl_win32_extra.h>
bool show_sentry_feedback = false;
char sentry_user_comments[1024] = "";
char sentry_user_email[128] = "";
char sentry_user_name[128] = "";



char* n_ram_buffer = 0;
casioemu::MMU* me_mmu = 0;
SDL_Window* window = 0;
SDL_Renderer* renderer = 0;

std::vector<Label> g_labels;

CodeViewer* code_viewer = nullptr;
UIWindow* setting_window = nullptr;
UIWindow* search_window = nullptr;
Injector* injector = nullptr;
int top_bar_size = 0;
Breakpoints* membp = 0;

std::vector<UIWindow*> windows{};

std::string ui_state_fn = "ui_state.txt";
bool ui_ready = false;
bool swap_portrait_layout = false;
bool swap_landscape_layout = false;
ImGuiID g_dock_main_id = 0;

void SaveUIState() {
    if (!ui_ready) return;

    std::string tmp = ui_state_fn + ".tmp";

    std::ofstream f(tmp, std::ios::out | std::ios::trunc);
    if (!f.is_open()) return;

    for (auto* w : windows) {
        if (!w) continue;
        f << w->name << "=" << (w->open ? 1 : 0) << "\n";
    }

    if (RunDebugWindow::Instance) {
        f << "rac_compiler_path=" << RunDebugWindow::Instance->GetRacCompilerPath() << "\n";
        f << "hd_compiler_path=" << RunDebugWindow::Instance->GetHdCompilerPath() << "\n";
        f << "rac_run_command=" << RunDebugWindow::Instance->m_racRunCommandBuf << "\n";
        f << "hd_run_command=" << RunDebugWindow::Instance->m_hdRunCommandBuf << "\n";
    }
    if (WorkspaceExplorer::Instance) {
        f << "workspace_root=" << WorkspaceExplorer::Instance->GetWorkspaceRoot() << "\n";
    }
    if (TerminalWindow::Instance) {
        f << "terminal_shell=" << TerminalWindow::Instance->GetShellPath() << "\n";
    }

    f << "swap_portrait_layout=" << (swap_portrait_layout ? 1 : 0) << "\n";
    f << "swap_landscape_layout=" << (swap_landscape_layout ? 1 : 0) << "\n";

    f.close();
    std::filesystem::rename(tmp, ui_state_fn);
}
#ifdef __IOS__
#include "iOSNativeBridge.h"
#endif
static float screenshot_toast_timer = 0.0f;
static char ssh_host[128] = "";
static char ssh_user[128] = "";
static int ssh_port = 22;
static bool show_ssh_modal = false;
static bool show_custom_shell_modal = false;
static char custom_shell_path[512] = "";
void RenderDebuggerToolbar() {
    bool isCustom = false;
#if defined(__ANDROID__) || defined(IOS) || defined(__IOS__)
    isCustom = true;
#endif

    bool opened = false;
    if (isCustom) {
#if defined(__ANDROID__) || defined(IOS) || defined(__IOS__)
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        bool is_portrait = (viewport->WorkSize.x < viewport->WorkSize.y) || (viewport->WorkSize.x < 900.0f);
        float safeAreaFallback = is_portrait ? 35.0f : 15.0f;
        float safeAreaTop = viewport->WorkPos.y > 0.1f ? viewport->WorkPos.y : safeAreaFallback;
        float headerY = safeAreaTop;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, headerY));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, ImGui::GetFrameHeight() + 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y + 4.0f));
        
        opened = ImGui::Begin("##DebuggerToolbar", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking);
#endif
    } else {
        opened = ImGui::BeginMainMenuBar();
#if !defined(__ANDROID__) && !defined(IOS)
        if (opened) {
            if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                if (window) {
                    ImVec2 delta = ImGui::GetIO().MouseDelta;
                    int x, y;
                    SDL_GetWindowPosition(window, &x, &y);
                    SDL_SetWindowPosition(window, x + (int)delta.x, y + (int)delta.y);
                }
            }
        }
#endif
    }

    if (opened) {
        bool showMenu = true;
        if (isCustom) {
            showMenu = ImGui::BeginMenuBar();
        }
        
        if (showMenu) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open File...", "Ctrl+P")) {
                    SystemDialogs::OpenFileDialog([](std::filesystem::path p) {
                        OpenFileInEditor(p.string());
                    });
                }
                if (ImGui::MenuItem("Open Folder...", "Ctrl+O")) {
                    if (WorkspaceExplorer::Instance) {
                        SystemDialogs::OpenFolderDialog([](std::filesystem::path p) {
                            WorkspaceExplorer::Instance->SetWorkspaceRoot(p.string());
                        });
                    }
                }
                if (ImGui::MenuItem("Save", "Ctrl+S")) {
                    if (CodeEditorWindow* active = GetFocusedCodeEditor()) {
                        active->SaveFile();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close File")) {
                    OpenFileInEditor("");
                }
                if (ImGui::MenuItem("Close Folder")) {
                    if (WorkspaceExplorer::Instance) {
                        WorkspaceExplorer::Instance->SetWorkspaceRoot("");
                    }
                }
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
                ImGui::Separator();
                if (ImGui::MenuItem("Cut", "Ctrl+X")) {}
                if (ImGui::MenuItem("Copy", "Ctrl+C")) {}
                if (ImGui::MenuItem("Paste", "Ctrl+V")) {}
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Explorer")) {
                    ImGui::SetWindowFocus("Workspace Explorer");
                }
                if (ImGui::MenuItem("Debugger")) {
                    ImGui::SetWindowFocus("Debugger Menu");
                }
                if (ImGui::MenuItem("Terminal")) {
                    ImGui::SetWindowFocus("Terminal");
                }
                if (ImGui::MenuItem("Theme")) {
                    ImGui::SetWindowFocus("Theme");
                }
                ImGui::Separator();
                int open_count = 0;
                for (auto* w : windows) {
                    if (w && w->open && strcmp(w->name, "Calculator") != 0) open_count++;
                }
                bool is_any_open = (open_count > 0);
                if (ImGui::MenuItem(is_any_open ? "Close All / Open All" : "Open All / Close All")) {
                    for (auto* w : windows) {
                        if (w) {
                            if (strcmp(w->name, "Calculator") == 0) {
                                w->open = true;
                            } else {
                                w->open = !is_any_open;
                            }
                        }
                    }
                    SaveUIState();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ThemeManager::Instance().Settings().isDarkMode ? "Switch to Light" : "Switch to Dark")) {
                    if (ThemeManager::Instance().Settings().isDarkMode)
                        ThemeManager::Instance().SetLightMode();
                    else
                        ThemeManager::Instance().SetDarkMode();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Layout")) {
                    if (ImGui::MenuItem("Swap Editor & Terminal (Landscape)", nullptr, &swap_landscape_layout)) {
                        SaveUIState();
                    }
                    if (ImGui::MenuItem("Swap Calculator & Editor/Tabs (Portrait)", nullptr, &swap_portrait_layout)) {
                        SaveUIState();
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Terminal")) {
                if (ImGui::MenuItem("Clear Terminal")) {
                    if (TerminalWindow::Instance) {
                        TerminalWindow::Instance->ClearLog();
                    }
                }
                if (ImGui::MenuItem("Focus Terminal")) {
                    ImGui::SetWindowFocus("Terminal");
                }
                if (ImGui::MenuItem("Connect SSH...")) {
                    show_ssh_modal = true;
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Select Shell")) {
#ifdef _WIN32
                    if (ImGui::MenuItem("Command Prompt (cmd.exe)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "cmd.exe")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("cmd.exe");
                    }
                    if (ImGui::MenuItem("PowerShell (powershell.exe)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "powershell.exe")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("powershell.exe");
                    }
                    if (ImGui::MenuItem("Git Bash (C:\\Program Files\\Git\\bin\\bash.exe)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "C:\\Program Files\\Git\\bin\\bash.exe")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("C:\\Program Files\\Git\\bin\\bash.exe");
                    }
                    if (ImGui::MenuItem("WSL (wsl.exe)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "wsl.exe")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("wsl.exe");
                    }
#else
                    if (ImGui::MenuItem("Zsh (/bin/zsh)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "/bin/zsh")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("/bin/zsh");
                    }
                    if (ImGui::MenuItem("Bash (/bin/bash)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "/bin/bash")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("/bin/bash");
                    }
                    if (ImGui::MenuItem("Sh (/bin/sh)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "/bin/sh")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("/bin/sh");
                    }
#ifdef __ANDROID__
                    if (ImGui::MenuItem("Android Sh (/system/bin/sh)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "/system/bin/sh")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("/system/bin/sh");
                    }
                    if (ImGui::MenuItem("Termux Bash (/data/data/com.termux/files/usr/bin/bash)", nullptr, TerminalWindow::Instance && TerminalWindow::Instance->GetShellPath() == "/data/data/com.termux/files/usr/bin/bash")) {
                        if (TerminalWindow::Instance) TerminalWindow::Instance->SetShellPath("/data/data/com.termux/files/usr/bin/bash");
                    }
#endif
#endif
                    ImGui::Separator();
                    if (ImGui::MenuItem("Custom Shell...", nullptr, false)) {
                        show_custom_shell_modal = true;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            
            ImGui::Separator();
            
            if (ImGui::BeginMenu("Run")) {
                if (ImGui::MenuItem("Run Current File", "F5")) {
                    if (RunDebugWindow::Instance) {
                        RunDebugWindow::Instance->RunCurrentFile();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Set RAC Compiler Path...")) {
                    SystemDialogs::OpenFolderDialog([](std::filesystem::path p) {
                        if (RunDebugWindow::Instance) RunDebugWindow::Instance->SetCompilerPath("rac", p.string());
                    });
                }
                if (ImGui::MenuItem("Edit RAC Run Command...")) {
                    if (RunDebugWindow::Instance) {
                        RunDebugWindow::Instance->m_showRacCommandEditor = true;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Set HD Compiler Path...")) {
                    SystemDialogs::OpenFolderDialog([](std::filesystem::path p) {
                        if (RunDebugWindow::Instance) RunDebugWindow::Instance->SetCompilerPath("hd", p.string());
                    });
                }
                if (ImGui::MenuItem("Edit HD Run Command...")) {
                    if (RunDebugWindow::Instance) {
                        RunDebugWindow::Instance->m_showHdCommandEditor = true;
                    }
                }
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Emulator")) {
                bool isPaused = m_emu->GetPaused();
                if (ImGui::MenuItem(isPaused ? "Resume" : "Pause", "Space")) {
                    m_emu->SetPaused(!isPaused);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Screenshot (Full)")) {
                    m_emu->screenshot_full_ui = true;
                    m_emu->screenshot_requested = true;
                }
                if (ImGui::MenuItem("Screenshot (Screen Only)")) {
                    m_emu->screenshot_full_ui = false;
                    m_emu->screenshot_requested = true;
                }
                ImGui::Separator();
                if (m_emu->recording_active.load()) {
                    if (ImGui::MenuItem("Stop Recording")) {
                        m_emu->recording_stop_requested = true;
                    }
                } else {
                    if (ImGui::MenuItem("Record (Full)")) {
                        m_emu->recording_full_ui = true;
                        m_emu->recording_requested = true;
                    }
                    if (ImGui::MenuItem("Record (Screen Only)")) {
                        m_emu->recording_full_ui = false;
                        m_emu->recording_requested = true;
                    }
                }
                ImGui::EndMenu();
            }

            if (m_emu->screenshot_taken.exchange(false)) {
                screenshot_toast_timer = 3.0f;
            }

            if (screenshot_toast_timer > 0.0f) {
                ImGui::SameLine(ImGui::GetWindowWidth() - 250.0f);
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "[C] Screenshot Saved!");
                screenshot_toast_timer -= ImGui::GetIO().DeltaTime;
            }

            if (m_emu->recording_active.load()) {
                ImGui::SameLine(ImGui::GetWindowWidth() - (screenshot_toast_timer > 0.0f ? 450.0f : 200.0f));
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "[O] Recording: %u frames", m_emu->recording_frame_count.load());
            }


            if (isCustom) {
                float btnWidth = 40.0f;
                ImGui::SameLine(ImGui::GetWindowWidth() - btnWidth - ImGui::GetStyle().WindowPadding.x);
                if (ImGui::Button(" v ", ImVec2(btnWidth, 0))) {
                    if (SDL_HasScreenKeyboardSupport()) {
                        SDL_StopTextInput();
                    }
                    ImGui::SetWindowFocus(nullptr);
                }
                ImGui::EndMenuBar();
            }
        }
        
        if (isCustom) {
            ImGui::End();
            ImGui::PopStyleVar(4);
        } else {
            ImGui::EndMainMenuBar();
        }
    }

    if (show_ssh_modal) {
        ImGui::OpenPopup("Connect SSH");
        show_ssh_modal = false;
    }

    if (ImGui::BeginPopupModal("Connect SSH", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Host", ssh_host, sizeof(ssh_host));
        ImGui::InputText("Username", ssh_user, sizeof(ssh_user));
        ImGui::InputInt("Port", &ssh_port);

        ImGui::Separator();
        if (ImGui::Button("Connect", ImVec2(120, 0))) {
            if (TerminalWindow::Instance && ssh_host[0] != '\0' && ssh_user[0] != '\0') {
                std::string ssh_cmd = "ssh " + std::string(ssh_user) + "@" + std::string(ssh_host) + " -p " + std::to_string(ssh_port);
                TerminalWindow::Instance->SetShellPath(ssh_cmd);
                ImGui::SetWindowFocus("Terminal");
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (show_custom_shell_modal) {
        if (TerminalWindow::Instance) {
            strncpy(custom_shell_path, TerminalWindow::Instance->GetShellPath().c_str(), sizeof(custom_shell_path) - 1);
            custom_shell_path[sizeof(custom_shell_path) - 1] = '\0';
        }
        ImGui::OpenPopup("Custom Shell");
        show_custom_shell_modal = false;
    }

    if (ImGui::BeginPopupModal("Custom Shell", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter absolute path or command to shell:");
        ImGui::InputText("Shell Path", custom_shell_path, sizeof(custom_shell_path));

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            if (TerminalWindow::Instance && custom_shell_path[0] != '\0') {
                TerminalWindow::Instance->SetShellPath(custom_shell_path);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (RunDebugWindow::Instance) {
        if (RunDebugWindow::Instance->m_showRacCommandEditor) {
            ImGui::OpenPopup("Edit RAC Run Command");
            RunDebugWindow::Instance->m_showRacCommandEditor = false;
        }
        
        if (ImGui::BeginPopupModal("Edit RAC Run Command", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter the terminal command to run the RAC model.");
            ImGui::TextDisabled("Note: The file path will be appended to the command automatically.");
            ImGui::InputText("Command", RunDebugWindow::Instance->m_racRunCommandBuf, sizeof(RunDebugWindow::Instance->m_racRunCommandBuf));
            
            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                SaveUIState();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (RunDebugWindow::Instance->m_showHdCommandEditor) {
            ImGui::OpenPopup("Edit HD Run Command");
            RunDebugWindow::Instance->m_showHdCommandEditor = false;
        }

        if (ImGui::BeginPopupModal("Edit HD Run Command", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter the terminal command to run the HD model.");
            ImGui::TextDisabled("Note: The file path will be appended to the command automatically.");
            ImGui::InputText("Command", RunDebugWindow::Instance->m_hdRunCommandBuf, sizeof(RunDebugWindow::Instance->m_hdRunCommandBuf));
            
            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                SaveUIState();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void LoadUIState() {
    std::ifstream f(ui_state_fn);
    if (!f.is_open()) return;

    std::unordered_map<std::string, bool> state;

    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string name = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        if (name == "rac_compiler_path") {
            if (RunDebugWindow::Instance) {
                RunDebugWindow::Instance->SetCompilerPath("rac", value);
            }
        } else if (name == "hd_compiler_path") {
            if (RunDebugWindow::Instance) {
                RunDebugWindow::Instance->SetCompilerPath("hd", value);
            }
        } else if (name == "rac_run_command") {
            if (RunDebugWindow::Instance) {
                strncpy(RunDebugWindow::Instance->m_racRunCommandBuf, value.c_str(), sizeof(RunDebugWindow::Instance->m_racRunCommandBuf) - 1);
            }
        } else if (name == "hd_run_command") {
            if (RunDebugWindow::Instance) {
                strncpy(RunDebugWindow::Instance->m_hdRunCommandBuf, value.c_str(), sizeof(RunDebugWindow::Instance->m_hdRunCommandBuf) - 1);
            }
        } else if (name == "workspace_root") {
            if (WorkspaceExplorer::Instance) {
                WorkspaceExplorer::Instance->SetWorkspaceRoot(value);
            }
        } else if (name == "terminal_shell") {
            if (TerminalWindow::Instance) {
                TerminalWindow::Instance->SetShellPath(value);
            }
        } else if (name == "swap_portrait_layout") {
            swap_portrait_layout = (value == "1");
        } else if (name == "swap_landscape_layout") {
            swap_landscape_layout = (value == "1");
        } else {
            bool open = (value == "1");
            state[name] = open;
        }
    }

    for (auto* w : windows) {
        if (!w) continue;

        if (state.find(w->name) != state.end())
            w->open = state[w->name];
    }
} 

void RenderStatusBar() {
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	float barHeight = ImGui::GetFrameHeight() + 4.0f;
	
	ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - barHeight));
	ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, barHeight));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.12f, 1.0f));
	
	if (ImGui::Begin("##StatusBar", nullptr, 
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoDocking)) {
		
		// Run/Pause state status indicator
		if (m_emu->GetPaused()) {
			ImGui::TextColored(UIHelpers::kColorWarning, "[||] %s", "StatusBar.Paused"_lc);  // ⏸
		} else {
			ImGui::TextColored(UIHelpers::kColorSuccess, "[>] %s", "StatusBar.Running"_lc); // ▶
		}
		
		ImGui::SameLine(0.0f, 20.0f);
		ImGui::TextDisabled("|");
		ImGui::SameLine(0.0f, 20.0f);
		
		// Current PC
		ImGui::Text("PC: %05X", pc_cache);
		
		ImGui::SameLine(0.0f, 20.0f);
		ImGui::TextDisabled("|");
		ImGui::SameLine(0.0f, 20.0f);
		
		// Breakpoints count
		int bpCount = code_viewer ? (int)code_viewer->GetBreakpointCount() : 0;
		ImGui::Text("BP: %d", bpCount);
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
}

void gui_loop() {
    if (!m_emu->Running())
        return;

    ImGuiIO& io = ImGui::GetIO();
    
#if defined(__ANDROID__) || defined(IOS) || defined(__IOS__)
    ImGuiContext& g = *GImGui;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        bool scrolling_tab_bar = false;
        for (int i = 0; i < g.TabBars.GetMapSize(); i++) {
            if (ImGuiTabBar* tab_bar = g.TabBars.TryGetMapData(i)) {
                if (tab_bar->BarRect.Contains(io.MouseClickedPos[0])) {
                    tab_bar->ScrollingAnim -= io.MouseDelta.x;
                    tab_bar->ScrollingTarget -= io.MouseDelta.x;
                    scrolling_tab_bar = true;
                }
            }
        }
        
        if (!scrolling_tab_bar && !ImGui::IsAnyItemActive()) {
            io.MouseWheelH -= io.MouseDelta.x * 0.05f;
            io.MouseWheel -= io.MouseDelta.y * 0.05f;
        }
    }
#endif
    if (RunDebugWindow::Instance) {
        RunDebugWindow::Instance->Update();
    }


#if defined(__ANDROID__) || defined(MACOS) || defined(IOS)
    ThemeManager::Instance().UpdateUIScale();
#endif

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;
    float barHeight = ImGui::GetFrameHeight() + 4.0f;
    
    bool is_portrait = (workSize.x < workSize.y) || (workSize.x < 900.0f);
    static float s_activityBarWidth = 48.0f;
    float activityBarWidth = is_portrait ? 0.0f : s_activityBarWidth;
    
    if (!is_portrait) {
        ImGui::SetNextWindowPos(workPos);
        ImGui::SetNextWindowSize(ImVec2(activityBarWidth, workSize.y - barHeight), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(48.0f, 0.0f), ImVec2(workSize.x / 3.0f, FLT_MAX));
        ImGui::SetNextWindowViewport(viewport->ID);
        
        ImGuiWindowFlags activity_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | 
                                          ImGuiWindowFlags_NoMove | 
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
        ImGui::Begin("ActivityBar", nullptr, activity_flags);
        s_activityBarWidth = ImGui::GetWindowWidth();
        activityBarWidth = s_activityBarWidth;
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        
        ImVec2 btnSize(40.0f, 40.0f);
        if (ImGui::Button(" W ", btnSize)) {
            ImGui::SetWindowFocus("Workspace Explorer");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Workspace Explorer");
        
        ImGui::Spacing();
        if (ImGui::Button(" B ", btnSize)) {
            ImGui::SetWindowFocus("Debugger Menu");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Debugger Windows");
        
        ImGui::Spacing();
        if (ImGui::Button(" F ", btnSize)) {
            if (search_window) {
                search_window->open = true;
                ImGui::SetWindowFocus("Search");
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Find");

        ImGui::Spacing();
        if (ImGui::Button(" P ", btnSize)) {
            search_window->open = true;
            ImGui::SetWindowFocus("Plugin Manager");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Plugin Manager");

        ImGui::Spacing();
        if (ImGui::Button(" S ", btnSize)) {
            if (setting_window) {
                setting_window->open = true;
                ImGui::SetWindowFocus("Settings");
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");
        
        ImGui::End();
    }

    float headerHeight = 0.0f;
#if defined(__ANDROID__) || defined(IOS) || defined(__IOS__)
    // On iOS, the notch/dynamic island covers the top of the screen.
    // If SDL doesn't provide a large enough WorkPos.y, we force it.
    float safeAreaFallback = is_portrait ? 35.0f : 15.0f;
    float safeAreaTop = viewport->WorkPos.y > 0.1f ? viewport->WorkPos.y : safeAreaFallback;
    float headerY = safeAreaTop;
    headerHeight = (headerY - viewport->WorkPos.y) + ImGui::GetFrameHeight() + 8.0f;
#endif

    ImVec2 dockPos = ImVec2(workPos.x + activityBarWidth, workPos.y + headerHeight);
    ImVec2 dockSize = ImVec2(workSize.x - activityBarWidth, workSize.y - barHeight - headerHeight);
    
    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | 
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | 
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("MainDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    
#if defined(__ANDROID__) || defined(IOS) || defined(__IOS__)
    const bool is_phone = true;
#else
    const bool is_phone = false;
#endif

    static bool first_time = true;
    static bool last_is_portrait = false;
    static bool last_swap_portrait = false;
    static bool last_swap_landscape = false;
    if (first_time || is_portrait != last_is_portrait || swap_portrait_layout != last_swap_portrait || swap_landscape_layout != last_swap_landscape) {
        first_time = false;
        last_is_portrait = is_portrait;
        last_swap_portrait = swap_portrait_layout;
        last_swap_landscape = swap_landscape_layout;
        
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dockSize);

        ImGuiID dock_main_id = dockspace_id;
        g_dock_main_id = dockspace_id;

        if (is_phone) {
            ImGui::DockBuilderDockWindow("Workspace Explorer", dock_main_id);
            ImGui::DockBuilderDockWindow("Terminal", dock_main_id);
            ImGui::DockBuilderDockWindow("Compiler Output", dock_main_id);
            ImGui::DockBuilderDockWindow("Debugger Menu", dock_main_id);
            ImGui::DockBuilderDockWindow("Calculator", dock_main_id);
            ImGui::DockBuilderDockWindow("Plugin Manager", dock_main_id);
            ImGui::DockBuilderDockWindow("Watch", dock_main_id);
            ImGui::DockBuilderDockWindow("Breakpoints", dock_main_id);
            ImGui::DockBuilderDockWindow("Funcs", dock_main_id);
            ImGui::DockBuilderDockWindow("Run & Debug", dock_main_id);
            ImGui::DockBuilderDockWindow("Code", dock_main_id);
            ImGui::DockBuilderDockWindow("Ram", dock_main_id);
            ImGui::DockBuilderDockWindow("Rom", dock_main_id);
            ImGui::DockBuilderDockWindow("PRam", dock_main_id);
            ImGui::DockBuilderDockWindow("Hardware", dock_main_id);
            ImGui::DockBuilderDockWindow("Addrs", dock_main_id);
            ImGui::DockBuilderDockWindow("Files", dock_main_id);
            ImGui::DockBuilderDockWindow("Labels", dock_main_id);
            ImGui::DockBuilderDockWindow("Theme", dock_main_id);
            ImGui::DockBuilderDockWindow("Variables", dock_main_id);
            ImGui::DockBuilderDockWindow("Snapshot", dock_main_id);
            ImGui::DockBuilderDockWindow("All", dock_main_id);
            ImGui::DockBuilderDockWindow("Bitmap", dock_main_id);
        } else if (is_portrait) {
            // Portrait Layout (vertical split, e.g. mobile portrait mode)
            // Split 55% for Calculator
            ImGuiDir calc_dir = swap_portrait_layout ? ImGuiDir_Up : ImGuiDir_Down;
            ImGuiID dock_id_top_tabs = dock_main_id;
            
            ImGui::DockBuilderDockWindow("Calculator", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Workspace Explorer", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Run & Debug", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Debugger Menu", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Terminal", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Compiler Output", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Plugin Manager", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Watch", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Breakpoints", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Funcs", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Rop", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Code", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Ram", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Rom", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("PRam", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Hardware", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Addrs", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Files", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Labels", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Settings", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Search", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Variables", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Snapshot", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("All", dock_id_top_tabs);
            ImGui::DockBuilderDockWindow("Bitmap", dock_id_top_tabs);
            g_dock_main_id = dock_id_top_tabs;
        } else {
            // Landscape Layout (Desktop or Landscape mobile)
            // Calculate ratios based on reasonable pixel sizes
            float left_ratio = 240.0f / dockSize.x;
            if (left_ratio < 0.15f) left_ratio = 0.15f;
            if (left_ratio > 0.25f) left_ratio = 0.25f;

            float right_ratio = 320.0f / (dockSize.x * (1.0f - left_ratio));
            if (right_ratio < 0.20f) right_ratio = 0.20f;
            if (right_ratio > 0.35f) right_ratio = 0.35f;

            float bottom_ratio = 250.0f / dockSize.y;
            if (bottom_ratio < 0.20f) bottom_ratio = 0.20f;
            if (bottom_ratio > 0.35f) bottom_ratio = 0.35f;

            ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, left_ratio, nullptr, &dock_main_id);
            ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, right_ratio, nullptr, &dock_main_id);
            ImGuiDir bottom_dir = swap_landscape_layout ? ImGuiDir_Up : ImGuiDir_Down;
            ImGuiID dock_id_bottom = ImGui::DockBuilderSplitNode(dock_main_id, bottom_dir, bottom_ratio, nullptr, &dock_main_id);

            // Left Sidebar: Workspace Explorer, Calculator, and Debugger Menu
            ImGui::DockBuilderDockWindow("Workspace Explorer", dock_id_left);
            ImGui::DockBuilderDockWindow("Calculator", dock_id_right);
            ImGui::DockBuilderDockWindow("Debugger Menu", dock_id_left);
            ImGui::DockBuilderDockWindow("Search", dock_id_left);
            ImGui::DockBuilderDockWindow("Plugin Manager", dock_id_left);
            
            // Bottom: Terminal and associated debugger tabs
            ImGui::DockBuilderDockWindow("Terminal", dock_id_bottom);
            ImGui::DockBuilderDockWindow("Compiler Output", dock_id_bottom);
            ImGui::DockBuilderDockWindow("Addrs", dock_id_bottom);
            ImGui::DockBuilderDockWindow("Breakpoints", dock_id_bottom);
            ImGui::DockBuilderDockWindow("Funcs", dock_id_bottom);
            ImGui::DockBuilderDockWindow("Watch", dock_id_bottom);
            ImGui::DockBuilderDockWindow("Run & Debug", dock_id_bottom);
            // Middle Top: Code Editor and other tabs
            ImGui::DockBuilderDockWindow("Settings", dock_main_id);
            ImGui::DockBuilderDockWindow("Rop", dock_main_id);
            ImGui::DockBuilderDockWindow("Code", dock_main_id);
            ImGui::DockBuilderDockWindow("Ram", dock_main_id);
            ImGui::DockBuilderDockWindow("Rom", dock_main_id);
            ImGui::DockBuilderDockWindow("PRam", dock_main_id);
            ImGui::DockBuilderDockWindow("Hardware", dock_main_id);
            ImGui::DockBuilderDockWindow("Files", dock_main_id);
            ImGui::DockBuilderDockWindow("Labels", dock_main_id);
            ImGui::DockBuilderDockWindow("Variables", dock_main_id);
            ImGui::DockBuilderDockWindow("Snapshot", dock_main_id);
            ImGui::DockBuilderDockWindow("All", dock_main_id);
            ImGui::DockBuilderDockWindow("Bitmap", dock_main_id);

            // Left and bottom nodes are left unrestricted so the user can reorganize freely.
        }
        
        ImGui::DockBuilderFinish(dockspace_id);
    }
    
    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    
    ImGui::End();
    
    RenderDebuggerToolbar();

    ImGui::Begin("Debugger Menu");
    for (auto* w : windows) {
        if (w && ImGui::Checkbox(w->name, &w->open)) {
            SaveUIState();
        }
    }
    ImGui::End();
    for (auto win : windows) {
        if (!win) continue;
        win->Render();
    }

    // Clean up closed windows
    for (auto it = windows.begin(); it != windows.end(); ) {
        if (*it && !(*it)->open && dynamic_cast<CodeEditorWindow*>(*it)) {
            delete *it;
            it = windows.erase(it);
        } else {
            ++it;
        }
    }

    //    ImGui::Begin("Testing");
    //    if (ImGui::Button("Crash"_lc)) {
    //        throw 0;
    //    }
    //    // --- 新增：手动反馈选项 ---
    // #ifdef ENABLE_SENTRY
    //    ImGui::SameLine(); // 放在 Crash 按钮旁边
    //    if (ImGui::Button("Send Feedback"_lc)) {
    //        // 重置之前的输入内容
    //        memset(sentry_user_comments, 0, sizeof(sentry_user_comments));
    //        show_sentry_feedback = true;
    //    }
    // #endif
    //    ImGui::End();
    //    // --- Sentry 反馈对话框逻辑 ---
    // #ifdef ENABLE_SENTRY
    //    if (show_sentry_feedback) {
    //        // 确保每一帧都调用 OpenPopup，直到它真正打开
    //        ImGui::OpenPopup("User Feedback");
    //    }
    //
    //    // 使用 Modal 窗口确保反馈过程不被打断
    //    if (ImGui::BeginPopupModal("User Feedback", &show_sentry_feedback, ImGuiWindowFlags_AlwaysAutoResize)) {
    //        ImGui::Text("Help us improve CasioEmuMsvc!");
    //        ImGui::Separator();
    //
    //        ImGui::Text("Email (Optional):");
    //        ImGui::InputText("##email", sentry_user_email, IM_ARRAYSIZE(sentry_user_email));
    //
    //        ImGui::Text("What happened?");
    //        ImGui::InputTextMultiline("##comments", sentry_user_comments, IM_ARRAYSIZE(sentry_user_comments),
    //            ImVec2(350, 120), ImGuiInputTextFlags_AllowTabInput);
    //
    //        if (ImGui::Button("Submit", ImVec2(120, 0))) {
    //            auto uuid = Binary::LoadOrInit("uuid.bin", util::Random::getRandomObject<sentry_uuid_t>());
    //            char buf[37]{};
    //            sentry_uuid_as_string(&uuid, buf);
    //            sentry_value_t feedback = sentry_value_new_feedback(sentry_user_comments, sentry_user_email, buf, 0);
    //            sentry_capture_feedback(feedback);
    //
    //            show_sentry_feedback = false;
    //            ImGui::CloseCurrentPopup();
    //        }
    //
    //        ImGui::SameLine();
    //        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
    //            show_sentry_feedback = false;
    //            ImGui::CloseCurrentPopup();
    //        }
    //        ImGui::EndPopup();
    //    }
    // #endif
    top_bar_size = ImGui::GetCursorPosY();
#if !defined(__ANDROID__) && !defined(IOS)
	RenderStatusBar();
#endif

	//	ImGui::Begin("Testing");
	//	if (ImGui::Button("Crash"_lc)) {
	//		throw 0;
	//	}
	//	// --- 新增：手动反馈选项 ---
	// #ifdef ENABLE_SENTRY
	//	ImGui::SameLine(); // 放在 Crash 按钮旁边
	//	if (ImGui::Button("Send Feedback"_lc)) {
	//		// 重置之前的输入内容
	//		memset(sentry_user_comments, 0, sizeof(sentry_user_comments));
	//		show_sentry_feedback = true;
	//	}
	// #endif
	//	ImGui::End();
	//	// --- Sentry 反馈对话框逻辑 ---
	// #ifdef ENABLE_SENTRY
	//	if (show_sentry_feedback) {
	//		// 确保每一帧都调用 OpenPopup，直到它真正打开
	//		ImGui::OpenPopup("User Feedback");
	//	}
	//
	//	// 使用 Modal 窗口确保反馈过程不被打断
	//	if (ImGui::BeginPopupModal("User Feedback", &show_sentry_feedback, ImGuiWindowFlags_AlwaysAutoResize)) {
	//		ImGui::Text("Help us improve CasioEmuMsvc!");
	//		ImGui::Separator();
	//
	//		ImGui::Text("Email (Optional):");
	//		ImGui::InputText("##email", sentry_user_email, IM_ARRAYSIZE(sentry_user_email));
	//
	//		ImGui::Text("What happened?");
	//		ImGui::InputTextMultiline("##comments", sentry_user_comments, IM_ARRAYSIZE(sentry_user_comments),
	//			ImVec2(350, 120), ImGuiInputTextFlags_AllowTabInput);
	//
	//		if (ImGui::Button("Submit", ImVec2(120, 0))) {
	//			auto uuid = Binary::LoadOrInit("uuid.bin", util::Random::getRandomObject<sentry_uuid_t>());
	//			char buf[37]{};
	//			sentry_uuid_as_string(&uuid, buf);
	//			sentry_value_t feedback = sentry_value_new_feedback(sentry_user_comments, sentry_user_email, buf, 0);
	//			sentry_capture_feedback(feedback);
	//
	//			show_sentry_feedback = false;
	//			ImGui::CloseCurrentPopup();
	//		}
	//
	//		ImGui::SameLine();
	//		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
	//			show_sentry_feedback = false;
	//			ImGui::CloseCurrentPopup();
	//		}
	//		ImGui::EndPopup();
	//	}
	// #endif

    ImGui::Render();
    //SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
    
    #ifndef SINGLE_WINDOW
    SDL_RenderPresent(renderer);
    #endif
}

CodeViewer* test_gui(bool* guiCreated, SDL_Window* wnd, SDL_Renderer* rnd) {
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    
    if (window || renderer) {
        gui_cleanup();
        window = nullptr;
        renderer = nullptr;
    }

#ifdef SINGLE_WINDOW
    window = wnd;
    renderer = rnd;
#else
#if defined(__ANDROID__) || defined(IOS)
    window = SDL_CreateWindow("CasioEmuMsvc Debugger",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        (int)ThemeManager::Instance().windowWidth,
        (int)ThemeManager::Instance().windowHeight,
        SDL_WINDOW_RESIZABLE);
#else
    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    
    window = SDL_CreateWindow("CasioEmuMsvc Debugger",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        dm.w * 0.8,
        dm.h * 0.8,
        SDL_WINDOW_RESIZABLE);
#endif
#ifdef _WIN32
    EnableDarkTitleBar(GetSDLWindowHandle(window));
#endif
    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
#endif

    if (!renderer) {
        SDL_Log("Error creating SDL_Renderer!");
        return nullptr;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

#if defined(__ANDROID__) || defined(IOS)
    ThemeManager::Instance().LoadSettings();
    ThemeManager::Instance().UpdateUIScale();
    m_emu->calculator_as_tab.store(true);
#endif

    RebuildFont();
    // SetupDefaultTheme();

    io.IniFilename = "imgui.ini";
    io.WantCaptureKeyboard = true;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#if defined(__ANDROID__) || defined(IOS)
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
#endif

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    if (guiCreated)
        *guiCreated = true;
    for (int i = 0; i < 5000 && !me_mmu; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    if (!me_mmu) {
        SDL_Log("MMU not ready!");
        return nullptr;
    }
    auto label_file = m_emu->GetModelFilePath("labels.txt");
    if (std::filesystem::exists(label_file))
        g_labels = parseFile(label_file);
    else
        std::cout << "[Warning] labels.txt doesn't exist. You can consider create one for better debugging experiences. Format: address(0x1234),func name(can be quoted)\n";

    if (m_emu->hardware_id == casioemu::HW_FX_5800P) {
        windows.push_back(CreateFx5800FileSystem());
    }

    if (!RunDebugWindow::Instance) {
        new RunDebugWindow();
    }

    for (auto item : std::initializer_list<UIWindow*>{
              new WorkspaceExplorer(),
              new CompilerOutputWindow(),
              new TerminalWindow(),
              new CalculatorWindow(),
              new VariableWindow(),
             new LabelViewer(),
             new WatchWindow(),
             CreateCallAnalysisWindow(),
             code_viewer = new CodeViewer(),
             injector = new Injector(),
             membp = new Breakpoints(),
             CreateAddressWindow(),
             // MakeAssemblerUI(),

             new PluginLogWindow(),
             CreateSnapshotWindow(),
             setting_window = MakeSettingWindow(),
             search_window = new SearchWindow(),
             CreateBitmapViewer(), })
        windows.push_back(item);
    for (auto item : GetEditors())
        windows.push_back(item);
    if (!std::filesystem::exists(ui_state_fn)) {
        for (auto* w : windows) {
            if (w) {
                w->open = true;
                w->bring_to_front_requested = false;
            }
        }
    }
    LoadUIState();
    ui_ready = true;
    /*for (auto* w : windows) {
        if (!w) continue;
    
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%s ptr=%p open=%d",
            w->name,
            (void*)w,
            w->open
        );
    
        DebugLog(buf);
    }*/
    
    return nullptr;
}

namespace UIHelpers {

	void JumpToMemory(uint32_t addr) {
		// Try Ram first
		for (auto* win : windows) {
			if (win->name && strcmp(win->name, "Ram") == 0) {
				if (win->GotoMemoryAddress(addr)) return;
			}
		}
		// Try PRam next
		for (auto* win : windows) {
			if (win->name && strcmp(win->name, "PRam") == 0) {
				if (win->GotoMemoryAddress(addr)) return;
			}
		}
		// Try any remaining
		for (auto* win : windows) {
			if (win->GotoMemoryAddress(addr)) return;
		}
	}

	void ClickableAddress(uint32_t addr, JumpTarget defaultTarget) {
		// Render the colored address text
		ImGui::PushStyleColor(ImGuiCol_Text, kColorInfo);
		char addrLabel[16];
		snprintf(addrLabel, sizeof(addrLabel), "%05X", addr);
		ImGui::TextUnformatted(addrLabel);
		ImGui::PopStyleColor();

		if (ImGui::IsItemHovered()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::BeginTooltip();
			if (defaultTarget == JumpTarget::Code) {
				ImGui::Text("ClickableAddress.CodeJumpTooltip"_lc, addr);
				ImGui::TextDisabled("%s", "ClickableAddress.RightClickHint"_lc);
			} else if (defaultTarget == JumpTarget::Memory) {
				ImGui::Text("ClickableAddress.MemJumpTooltip"_lc, addr);
				ImGui::TextDisabled("%s", "ClickableAddress.RightClickHint"_lc);
			} else {
				ImGui::Text("ClickableAddress.BothTooltip"_lc, addr);
			}
			ImGui::EndTooltip();
		}

		// Left-click: default action
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			if (defaultTarget == JumpTarget::Code || defaultTarget == JumpTarget::Both) {
				if (code_viewer) {
					code_viewer->JumpTo(addr);
					code_viewer->BringToFront();
				}
			} else {
				JumpToMemory(addr);
			}
		}

		// Right-click: context menu with both options
		char popupId[32];
		snprintf(popupId, sizeof(popupId), "##ca_popup_%05X", addr);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			ImGui::OpenPopup(popupId);
		}
		if (ImGui::BeginPopup(popupId)) {
			ImGui::TextDisabled("0x%05X", addr);
			ImGui::Separator();
			if (ImGui::MenuItem("ClickableAddress.CodeJump"_lc)) {
				if (code_viewer) {
					code_viewer->JumpTo(addr);
					code_viewer->BringToFront();
				}
			}
			if (ImGui::MenuItem("ClickableAddress.MemJump"_lc)) {
				JumpToMemory(addr);
			}
			ImGui::EndPopup();
		}
	}
}


void gui_cleanup() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SaveUIState();
    windows.clear();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}