#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lime {

struct LuaDiagnostic {
    int         line = 0;
    std::string message;
};

std::vector<LuaDiagnostic> checkLua(std::string_view src);

}
