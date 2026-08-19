#include "lime/lua_check.h"

#include <cctype>

namespace lime {
namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

int longBracketLevel(std::string_view s, std::size_t at) {
    if (at >= s.size() || s[at] != '[') return -1;
    std::size_t i = at + 1;
    while (i < s.size() && s[i] == '=') ++i;
    if (i < s.size() && s[i] == '[') return static_cast<int>(i - at - 1);
    return -1;
}

struct Block {
    std::string keyword;
    int line = 0;
};

}

std::vector<LuaDiagnostic> checkLua(std::string_view src) {
    std::vector<LuaDiagnostic> out;
    std::vector<Block> blocks;
    std::vector<Block> brackets;

    int line = 1;
    std::size_t i = 0;

    auto report = [&](int at, std::string msg) {
        if (!out.empty() && out.back().line == at) return;
        out.push_back({at, std::move(msg)});
    };

    while (i < src.size()) {
        const char c = src[i];

        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
            const int lvl = longBracketLevel(src, i + 2);
            if (lvl >= 0) {
                const std::string close = "]" + std::string(static_cast<std::size_t>(lvl), '=') + "]";
                const std::size_t start = line;
                const std::size_t at = src.find(close, i + 2);
                if (at == std::string_view::npos) {
                    report(static_cast<int>(start), "unterminated long comment");
                    return out;
                }
                for (std::size_t k = i; k < at; ++k)
                    if (src[k] == '\n') ++line;
                i = at + close.size();
                continue;
            }
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }

        if (const int lvl = longBracketLevel(src, i); lvl >= 0) {
            const std::string close = "]" + std::string(static_cast<std::size_t>(lvl), '=') + "]";
            const int start = line;
            const std::size_t at = src.find(close, i + 1);
            if (at == std::string_view::npos) {
                report(start, "unterminated long string");
                return out;
            }
            for (std::size_t k = i; k < at; ++k)
                if (src[k] == '\n') ++line;
            i = at + close.size();
            continue;
        }

        if (c == '"' || c == '\'') {
            const int start = line;
            ++i;
            bool closed = false;
            while (i < src.size()) {
                if (src[i] == '\\') {
                    if (i + 1 < src.size() && src[i + 1] == '\n') ++line;
                    i += 2;
                    continue;
                }
                if (src[i] == '\n') break;
                if (src[i] == c) { ++i; closed = true; break; }
                ++i;
            }
            if (!closed) report(start, "unterminated string");
            continue;
        }

        if (c == '(' || c == '[' || c == '{') {
            brackets.push_back({std::string(1, c), line});
            ++i;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            const char want = c == ')' ? '(' : (c == ']' ? '[' : '{');
            if (brackets.empty()) {
                report(line, std::string("stray '") + c + "'");
            } else if (brackets.back().keyword[0] != want) {
                report(line, std::string("'") + c + "' closes a '"
                                 + brackets.back().keyword + "' opened on line "
                                 + std::to_string(brackets.back().line));
                brackets.pop_back();
            } else {
                brackets.pop_back();
            }
            ++i;
            continue;
        }

        if (isIdentStart(c)) {
            const std::size_t start = i;
            while (i < src.size() && isIdentChar(src[i])) ++i;
            const std::string_view w = src.substr(start, i - start);

            if (w == "function" || w == "if" || w == "for" || w == "while") {
                blocks.push_back({std::string(w), line});
            } else if (w == "do") {
                if (!blocks.empty()
                    && (blocks.back().keyword == "for"
                        || blocks.back().keyword == "while")) {
                } else {
                    blocks.push_back({"do", line});
                }
            } else if (w == "repeat") {
                blocks.push_back({"repeat", line});
            } else if (w == "then" || w == "elseif" || w == "else") {
            } else if (w == "end") {
                if (blocks.empty()) {
                    report(line, "'end' with nothing to close");
                } else if (blocks.back().keyword == "repeat") {
                    report(line, "'repeat' on line "
                                     + std::to_string(blocks.back().line)
                                     + " needs 'until', not 'end'");
                    blocks.pop_back();
                } else {
                    blocks.pop_back();
                }
            } else if (w == "until") {
                if (blocks.empty() || blocks.back().keyword != "repeat")
                    report(line, "'until' without 'repeat'");
                else
                    blocks.pop_back();
            }
            continue;
        }
        ++i;
    }

    for (const Block& b : brackets)
        report(b.line, "'" + b.keyword + "' is never closed");
    for (const Block& b : blocks)
        report(b.line, "'" + b.keyword + "' is never closed"
                           + (b.keyword == "repeat" ? " (needs 'until')"
                                                    : " (needs 'end')"));
    return out;
}

}
