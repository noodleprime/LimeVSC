#include "limecore.h"

#include <deque>
#include <unordered_map>

namespace lime {

namespace {
struct Table {
    std::deque<std::string> strings;
    std::unordered_map<std::string_view, std::uint32_t> index;
};

Table& table() {
    static Table t;
    return t;
}
}

std::uint32_t Interner::id(std::string_view s) {
    Table& t = table();
    if (auto it = t.index.find(s); it != t.index.end()) return it->second;
    t.strings.emplace_back(s);
    const auto v = static_cast<std::uint32_t>(t.strings.size() - 1);
    t.index.emplace(std::string_view(t.strings.back()), v);
    return v;
}

std::string_view Interner::str(std::uint32_t v) {
    Table& t = table();
    if (v >= t.strings.size()) return {};
    return t.strings[v];
}

}
