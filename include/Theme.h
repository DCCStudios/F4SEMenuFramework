#pragma once


class Theme {
public:
    static std::vector<std::string> GetJsonFiles();
    static void LoadJsonStyle(const std::string& path);
};