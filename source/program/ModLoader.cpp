#include "ModLoader.hpp"
#include "DatabaseLoader.hpp"
#include "Config.hpp"
#include "FileLogger.hpp"
#include "toml.hpp"
#include "lib.hpp"
#include "fs.hpp"
#include "patches.hpp"
#include "Allocator.hpp"
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>

#define ADDR_INIT_ROM_DIR    FIX(0x001F1940) 
#define ADDR_ROM_DIR_PATHS   0x00CDF7C0


/**
 * Re-implementation of libc++ std::vector for memory-safe interaction 
 * with the game's internal data structures.
 */
struct libcxx_vector {
    libcxx_string* begin_;
    libcxx_string* end_;
    libcxx_string* cap_;

    size_t size() const { return end_ - begin_; }
    size_t capacity() const { return cap_ - begin_; }

    void insert_front(const std::vector<std::string>& strings) {
        size_t count = strings.size();
        if (count == 0) return;

        size_t old_size = size();
        size_t new_size = old_size + count;
        
        if (new_size > capacity()) {
            size_t new_cap = capacity() * 2;
            if (new_cap < new_size) new_cap = new_size;
            
            libcxx_string* new_buffer = (libcxx_string*)GameOperatorNew(new_cap * sizeof(libcxx_string));
            std::memset(new_buffer, 0, new_cap * sizeof(libcxx_string));
            
            for (size_t i = 0; i < old_size; ++i) {
                std::memcpy(&new_buffer[i + count], &begin_[i], sizeof(libcxx_string));
            }
            
            begin_ = new_buffer;
            end_ = new_buffer + new_size;
            cap_ = new_buffer + new_cap;
        } else {
            std::memmove(&begin_[count], &begin_[0], old_size * sizeof(libcxx_string));
            end_ += count;
        }
        
        for (size_t i = 0; i < count; ++i) {
            begin_[i].u.s.size_flag = 0; 
            begin_[i].assign(strings[i].c_str(), strings[i].length());
        }
    }
};

std::vector<std::string> ModLoader::modDirectoryPaths;
static std::vector<std::string> s_modRomPaths;

void ModLoader::initMod(const std::string& path) {
    FileLogger::log("[initMod] %s", path.c_str());
    std::string configPath = path + "/config.toml";
    nn::fs::FileHandle h;

    // If no mod-specific config exists, treat the mod as enabled by default
    if (R_FAILED(nn::fs::OpenFile(&h, configPath.c_str(), nn::fs::OpenMode_Read))) {
        FileLogger::log("  no config.toml, enabled by default");
        modDirectoryPaths.push_back(path);
        return;
    }

    int64_t size = 0;
    nn::fs::GetFileSize(&size, h);
    std::string content(size, '\0');
    nn::fs::ReadFile(h, 0, content.data(), size);
    nn::fs::CloseFile(h);

    toml::parse_result result = toml::parse(content);
    if (!result) {
        FileLogger::log("  config.toml parse failed, skipping");
        return;
    }

    toml::table config = std::move(result).table();
    bool isEnabled = config["enabled"].value_or(true);
    FileLogger::log("  config.toml found, enabled=%s", isEnabled ? "true" : "false");
    if (!isEnabled) return;

    // Handle "include" array if present
    if (auto includeArr = config["include"].as_array()) {
        for (auto& elem : *includeArr) {
            std::string sub = elem.value_or("");
            if (!sub.empty()) {
                std::string resolved = (sub == ".") ? path : path + "/" + sub;
                FileLogger::log("  include: %s", resolved.c_str());
                modDirectoryPaths.push_back(resolved);
            }
        }
    } else {
        modDirectoryPaths.push_back(path);
    }
}

/**
 * Hook to inject our mod paths into the game's internal RomFS search list.
 */
HOOK_DEFINE_TRAMPOLINE(InitRomDirectoryPathsHook) {
    static void Callback() {
        Orig(); 
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto romDirectoryPaths = *reinterpret_cast<libcxx_vector**>(base + ADDR_ROM_DIR_PATHS);
        
        // Inject mod paths at the beginning of the list for highest priority
        if (romDirectoryPaths && !s_modRomPaths.empty()) {
            romDirectoryPaths->insert_front(s_modRomPaths);
        }
    }
};

void ModLoader::init() {
    FileLogger::log("=== ModLoader::init ===");

    if (Config::modsDirectoryPath.empty()) {
        FileLogger::log("modsDirectoryPath is empty, aborting");
        return;
    }

    FileLogger::log("processing %zu priority entries", Config::priorityPaths.size());

    // 1. Process mods based on the synchronized priority list
    for (const auto& modName : Config::priorityPaths) {
        std::string modDirectory = Config::modsDirectoryPath + "/" + modName;
        nn::fs::DirectoryHandle dh;
        if (R_SUCCEEDED(nn::fs::OpenDirectory(&dh, modDirectory.c_str(), nn::fs::OpenDirectoryMode_Directory))) {
            nn::fs::CloseDirectory(dh);
            initMod(modDirectory);
        } else {
            FileLogger::log("  [skip] dir not found: %s", modDirectory.c_str());
        }
    }

    FileLogger::log("modDirectoryPaths: %zu entries", modDirectoryPaths.size());

    // 2. Convert physical SD paths to virtual RomFS paths for optimized loading
    for (auto& modDir : modDirectoryPaths) {
        std::string checkPath = modDir + "/rom";

        nn::fs::DirectoryHandle dh;
        if (R_SUCCEEDED(nn::fs::OpenDirectory(&dh, checkPath.c_str(), nn::fs::OpenDirectoryMode_Directory))) {
            nn::fs::CloseDirectory(dh);

            // Convert "ExlSD:/atmosphere/.../romfs/mods/ModName" to "rom:/mods/ModName"
            // This triggers Atmosphere's LayeredFS RAM caching for maximum speed.
            std::string virtualPath = modDir;
            size_t romfsPos = virtualPath.find("/romfs/");
            if (romfsPos != std::string::npos) {
                virtualPath = "rom:/" + virtualPath.substr(romfsPos + 7);
            }
            FileLogger::log("  romPath: %s -> %s", modDir.c_str(), virtualPath.c_str());
            s_modRomPaths.push_back(virtualPath);
        } else {
            FileLogger::log("  [no /rom dir] %s", modDir.c_str());
        }
    }

    FileLogger::log("s_modRomPaths: %zu entries", s_modRomPaths.size());

    // Initialize mod prefixes for the Database (MdataMgr)
    if (!s_modRomPaths.empty()) {
        FileLogger::log("calling DatabaseLoader::initMdataMgr");
        DatabaseLoader::initMdataMgr(s_modRomPaths);
        InitRomDirectoryPathsHook::InstallAtOffset(ADDR_INIT_ROM_DIR);
    } else {
        FileLogger::log("no rom paths, hooks NOT installed");
    }

    FileLogger::log("=== ModLoader::init done ===");
}
