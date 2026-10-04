#pragma once

#include <cstdio>
#include <string>

inline std::string jsonEscape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}
