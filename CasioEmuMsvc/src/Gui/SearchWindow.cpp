#include "SearchWindow.hpp"
#include "CodeViewer.hpp"
#include "IDEWindows.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <Localization.h>
#include <regex>

#include "Models.h"
#include "Chipset/Chipset.hpp"
#include "Chipset/MMU.hpp"
#include "Ui.hpp"

extern CodeViewer* code_viewer;
extern std::vector<class UIWindow*> windows;

SearchWindow::SearchWindow() : UIWindow("Search"), is_searching(false) {
    memset(search_query, 0, sizeof(search_query));
}

void SearchWindow::PerformSearch() {
    results.clear();
    std::string query = search_query;
    if (query.empty()) return;

    if (search_scope == 0) {
        SearchFiles();
    } else if (search_scope == 1) {
        SearchMemory();
    } else if (search_scope == 2) {
        SearchCode();
    }
}

void SearchWindow::SearchFiles() {
    if (!WorkspaceExplorer::Instance) return;
    std::string root = WorkspaceExplorer::Instance->GetWorkspaceRoot();
    if (root.empty() || !std::filesystem::exists(root)) return;

    std::string query = search_query;
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::ifstream file(entry.path());
            std::string line;
            int line_number = 1;
            while (std::getline(file, line)) {
                std::string lower_line = line;
                std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);
                
                size_t pos = lower_line.find(lower_query);
                if (pos != std::string::npos) {
                    SearchResult res;
                    res.label = entry.path().filename().string() + ":" + std::to_string(line_number);
                    res.context = line;
                    res.filepath = entry.path().string();
                    res.line = line_number;
                    res.address = 0;
                    res.match_pos = pos;
                    res.match_len = query.length();
                    results.push_back(res);
                }
                line_number++;
            }
        }
    }
}

void SearchWindow::SearchMemory() {
    if (!m_emu) return;
    std::string query = search_query;
    std::vector<uint8_t> bytes;
    std::string hex_str;
    for (char c : query) {
        if (!std::isspace(c)) {
            hex_str += c;
        }
    }
    
    for (size_t i = 0; i < hex_str.length(); i += 2) {
        std::string byte_str = hex_str.substr(i, 2);
        if (byte_str.length() == 1) byte_str = "0" + byte_str;
        try {
            bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
        } catch (...) {
            return; // Invalid hex
        }
    }

    if (bytes.empty()) return;

    uint32_t start_addr = casioemu::GetRamBaseAddr(m_emu->hardware_id);
    uint32_t size = casioemu::GetRamSize(m_emu->hardware_id);

    // Naive search
    for (uint32_t i = 0; i <= size - bytes.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (m_emu->chipset.mmu.ReadData(start_addr + i + j) != bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            SearchResult res;
            char hex[32];
            snprintf(hex, sizeof(hex), "%X", start_addr + i);
            res.label = "Address: 0x" + std::string(hex);
            res.context = "Hex: " + query;
            res.address = start_addr + i;
            res.filepath = "";
            res.line = 0;
            res.match_pos = 5; 
            res.match_len = query.length();
            results.push_back(res);
            
            // Limit results to avoid memory overload
            if (results.size() >= 100) break;
        }
    }
}

std::string RegexSafeSpace(const std::string& query) {
    std::string rx;
    bool in_space = false;
    for (char c : query) {
        if (std::isspace(c)) {
            if (!in_space) {
                rx += "\\s+";
                in_space = true;
            }
        } else {
            if (strchr(".^$|()[]{}+*?\\", c)) rx += "\\";
            rx += c;
            in_space = false;
        }
    }
    return rx;
}

void SearchWindow::SearchCode() {
    if (!code_viewer) return;
    std::string query = search_query;
    
    std::string rx_str = RegexSafeSpace(query);
    if (rx_str.empty()) return;
    std::regex search_rx;
    try {
        search_rx = std::regex(rx_str, std::regex_constants::icase);
    } catch (...) { return; }

    for (const auto& elem : code_viewer->codes) {
        std::string src = elem.srcbuf;
        std::smatch match;
        if (std::regex_search(src, match, search_rx)) {
            SearchResult res;
            char hex[32];
            snprintf(hex, sizeof(hex), "%X", elem.offset);
            res.label = "Code: 0x" + std::string(hex);
            res.context = src;
            res.address = elem.offset;
            res.filepath = "";
            res.line = 0;
            res.match_pos = match.position();
            res.match_len = match.length();
            results.push_back(res);
        }
    }
}

void SearchWindow::RenderCore() {
    ImGui::InputText("Query", search_query, sizeof(search_query));
    
    ImGui::RadioButton("Files", &search_scope, 0); ImGui::SameLine();
    ImGui::RadioButton("Memory", &search_scope, 1); ImGui::SameLine();
    ImGui::RadioButton("Code", &search_scope, 2);
    
    if (ImGui::Button("Find")) {
        PerformSearch();
    }
    
    ImGui::Separator();
    
    ImGui::BeginChild("SearchResults", ImVec2(0, 0), true);
    for (const auto& res : results) {
        ImGui::PushID(&res);
        ImVec2 start_pos = ImGui::GetCursorPos();
        
        ImGui::BeginGroup();
        ImGui::TextUnformatted(res.label.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted("-");
        ImGui::SameLine();
        
        if (res.match_len > 0 && res.match_pos <= res.context.length()) {
            std::string pre = res.context.substr(0, res.match_pos);
            std::string match = res.context.substr(res.match_pos, res.match_len);
            std::string post = res.context.substr(res.match_pos + res.match_len);
            
            if (!pre.empty()) { ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", pre.c_str()); ImGui::SameLine(0, 0); }
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s", match.c_str()); 
            if (!post.empty()) { ImGui::SameLine(0, 0); ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", post.c_str()); }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", res.context.c_str());
        }
        ImGui::EndGroup();
        
        ImVec2 end_pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(start_pos);
            if (ImGui::Selectable("##sel", false, ImGuiSelectableFlags_AllowItemOverlap | ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, end_pos.y - start_pos.y))) {
                if (search_scope == 0) {
                    OpenFileInEditor(res.filepath);
                    if (CodeEditorWindow* editor = GetFocusedCodeEditor()) {
                        if (editor->m_filePath == res.filepath) {
                            editor->m_editor.SetSelection(TextEditor::Coordinates(res.line - 1, res.match_pos), TextEditor::Coordinates(res.line - 1, res.match_pos + res.match_len));
                            editor->m_editor.SetCursorPosition(TextEditor::Coordinates(res.line - 1, res.match_pos + res.match_len));
                        }
                    }
                } else if (search_scope == 2 && code_viewer) {
                    code_viewer->JumpTo(res.address);
                    code_viewer->SetSearchHighlight(res.address, res.match_pos, res.match_len);
                    code_viewer->open = true;
                    ImGui::SetWindowFocus("Code");
                } else if (search_scope == 1) {
                    // Focus memory viewer or watch
                    for (auto* w : windows) {
                        if (w->name && strcmp(w->name, "Ram") == 0) {
                            w->open = true;
                            int byte_len = 0;
                            for (char c : search_query) {
                                if (!std::isspace(c)) byte_len++;
                            }
                            byte_len /= 2;
                            if (byte_len == 0) byte_len = 1;
                            w->SetSearchHighlight(res.address, 0, byte_len);
                            ImGui::SetWindowFocus(w->name);
                            break;
                        }
                    }
                }
            }
        ImGui::SetCursorPos(end_pos);
        ImGui::PopID();
    }
    ImGui::EndChild();
}
