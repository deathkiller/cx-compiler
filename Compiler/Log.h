#pragma once

#include <string>

#include "TinyFormat.h"

// Extended logging is disabled
#define LogDebug(text) 
//#define LogDebug(text) { std::cout << text << "\r\n"; }

enum struct LogType {
    Verbose,
    Info,
    Warning,
    Error
};

namespace Log {
    void PushIndent();
    void PopIndent();
    void Write(LogType type, std::string line);

    //void Write(LogType type, const char* line) {
    //    std::string sline = line;
    //    Write(type, sline);
    //}

    template<typename... Args>
    void Write(LogType type, const char* fmt, const Args&... args) {
        Write(type, tinyformat::format(fmt, args...));
    }

    void WriteSeparator();

    void SetHighlight(bool highlight);
}