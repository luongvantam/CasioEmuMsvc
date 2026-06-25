#pragma once
#include "Ui.hpp"
#include <string>
#include <vector>

class SearchWindow : public UIWindow {
private:
    char search_query[256];
    int search_scope = 0; // 0: Files, 1: Memory, 2: Code

    struct SearchResult {
        std::string label;
        std::string context;
        uint32_t address;
        std::string filepath;
        int line;
        size_t match_pos;
        size_t match_len;
    };
    std::vector<SearchResult> results;
    bool is_searching;

    void PerformSearch();
    void SearchFiles();
    void SearchMemory();
    void SearchCode();

public:
    SearchWindow();
    virtual void RenderCore() override;
};
