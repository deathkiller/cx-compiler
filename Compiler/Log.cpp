#include "Log.h"

#include <stdint.h>
#include <iostream>

// Windows-specific includes
#include "targetver.h"
#include <windows.h>

#include <io.h>
#include <fcntl.h>

namespace Log {
    static const int32_t max_lines = 3;

    static HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    static bool is_output_redirected;
    static bool supports_unicode;

    static int8_t indent;
    static std::string last_lines[max_lines];
    static int8_t last_line_index;

    static bool EndsWith(std::string const &a, std::string const &b) {
        auto len = b.length();
        auto pos = a.length() - len;
        if (pos < 0) {
            return false;
        }
        auto pos_a = &a[pos];
        auto pos_b = &b[0];
        while (*pos_a) {
            if (*pos_a++ != *pos_b++) {
                return false;
            }
        }
        return true;
    }

    static uint32_t GetEqualBeginChars(std::string const &a, std::string const &b)
    {
        uint32_t min_length = (uint32_t)min(a.size(), b.size());
        uint32_t last_break_count = 0;
        uint32_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            // Skip whitespace / indentation
            if (a[i] == ' ') {
                ++i;
                continue;
            }
            if (b[j] == ' ') {
                ++j;
                last_break_count = j;
                continue;
            }

            if (a[i] != b[j]) {
                return last_break_count;
            }
            if (!isalnum(b[j])) {
                last_break_count = j + 1;
            }

            ++i; ++j;
        }
        return min_length;
    }

    static uint32_t GetEqualEndChars(std::string const &a, std::string const &b)
    {
        uint32_t min_length = (uint32_t)min(a.size(), b.size());
        uint32_t last_break_count = 0;
        for (uint32_t i = 0; i < min_length; i++) {
            if (a[a.size() - 1 - i] != b[b.size() - 1 - i]) {
                return last_break_count;
            }
            if (!isalnum(a[a.size() - 1 - i])) {
                last_break_count = i + 1;
            }
        }
        return min_length;
    }

    static void SetDarkConsoleColor(LogType type, WORD default_attrib)
    {
        WORD foreground;
        switch (type) {
            default:
            case LogType::Info: foreground = FOREGROUND_INTENSITY; break;
            case LogType::Warning: foreground = FOREGROUND_RED | FOREGROUND_GREEN; break;
            case LogType::Error: foreground = FOREGROUND_RED; break;
            case LogType::Verbose: foreground = FOREGROUND_INTENSITY; break;
        }

        SetConsoleTextAttribute(console_handle, (default_attrib & 0xFFF0) | foreground);
    }

    static void SetBrightConsoleColor(LogType type, bool highlight, WORD default_attrib)
    {
        WORD foreground;
        switch (type) {
            default:
            case LogType::Info: foreground = (highlight ? FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY : FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); break;
            case LogType::Warning: foreground = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case LogType::Error: foreground = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
            case LogType::Verbose: foreground = FOREGROUND_INTENSITY; break;
        }

        SetConsoleTextAttribute(console_handle, (default_attrib & 0xFFF0) | foreground);
    }

    void PushIndent()
    {
        indent++;
    }

    void PopIndent()
    {
        if (indent > 0) {
            indent--;
        }
    }

    void Write(LogType type, std::string line)
    {
        if (line.empty()) {
            std::cout << "\r\n";
            return;
        }

        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(console_handle, &info);

        WORD default_attrib = info.wAttributes;

        bool highlight = (indent == 0 && EndsWith(line, "..."));

        uint32_t begin_grey_length = 0;
        uint32_t end_grey_length = 0;
        if (!highlight) {
            for (int8_t i = 0; i < max_lines; i++) {
                std::string& last_line = last_lines[i];
                begin_grey_length = max(begin_grey_length, GetEqualBeginChars(last_line, line));
                end_grey_length = max(end_grey_length, GetEqualEndChars(last_line, line));
            }
            if (begin_grey_length == line.length()) {
                end_grey_length = 0;
            }
            if (begin_grey_length + end_grey_length >= line.length()) {
                end_grey_length = 0;
            }
        }

        // Indent
        SetBrightConsoleColor(type, highlight, default_attrib);

        for (int8_t i = 0; i < indent; i++) {
            std::cout << "  ";
        }

        // Dark beginning
        if (begin_grey_length != 0) {
            SetDarkConsoleColor(type, default_attrib);
            std::cout << line.substr(0, begin_grey_length);
        }

        // Bright main part
        SetBrightConsoleColor(type, highlight, default_attrib);
        std::cout << line.substr(begin_grey_length, line.length() - begin_grey_length - end_grey_length);

        // Dark ending
        if (end_grey_length != 0) {
            SetDarkConsoleColor(type, default_attrib);
            std::cout << line.substr(line.length() - end_grey_length, end_grey_length);
        }

        // End the current line
        std::cout << "\r\n";

        last_lines[last_line_index] = line;
        last_line_index = (last_line_index + 1) % max_lines;

        SetConsoleTextAttribute(console_handle, default_attrib);
    }

    void WriteSeparator()
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(console_handle, &info);

        SetConsoleTextAttribute(console_handle, FOREGROUND_INTENSITY);

        for (uint16_t i = 0; i < info.dwSize.X; i += 1) {
            std::cout << "_";
        }
        std::cout << "\r\n";

        SetConsoleTextAttribute(console_handle, info.wAttributes);
    }

    void SetHighlight(bool highlight)
    {
        WORD attrib;
        if (highlight) {
            attrib = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        } else {
            attrib = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        }

        SetConsoleTextAttribute(console_handle, attrib);
    }
}