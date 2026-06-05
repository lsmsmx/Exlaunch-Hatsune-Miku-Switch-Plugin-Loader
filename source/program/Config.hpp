#pragma once
#include <string>
#include <vector>

class Config {
public:
    static bool enableDebug;
    static bool enableLog;
    static std::string modsDirectoryPath;
    static std::vector<std::string> priorityPaths;

    static bool init();
};