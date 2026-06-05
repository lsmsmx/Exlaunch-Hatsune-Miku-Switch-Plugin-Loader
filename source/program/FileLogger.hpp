#pragma once
#include <cstdarg>
#include <nn/fs.hpp>
#include "lib/nx/result.h"

namespace FileLogger {
    inline bool enabled = false;
    constexpr const char* LOG_PATH = "ExlSD:/DMLSwitchPort/debug.log";

    inline void init() {
        if (!enabled) return;
        nn::fs::DeleteFile(LOG_PATH);
        nn::fs::CreateFile(LOG_PATH, 0);
    }

    inline void log(const char* fmt, ...) {
        if (!enabled) return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(buf, (int)sizeof(buf) - 2, fmt, args);
        va_end(args);
        if (len < 0) len = 0;
        else if (len > (int)sizeof(buf) - 2) len = (int)sizeof(buf) - 2;
        buf[len++] = '\n';

        nn::fs::FileHandle h;
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, LOG_PATH, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
            int64_t sz = 0;
            nn::fs::GetFileSize(&sz, h);
            nn::fs::WriteFile(h, sz, buf, (size_t)len, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
        }
    }
}
