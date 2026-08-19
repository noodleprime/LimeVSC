#pragma once

#include "limecore.h"

namespace lime {

struct ImportOptions {
    std::string moduleName;
    float       columnWidth = 300.0f;
    float       rowHeight = 110.0f;
};

bool importLua(std::string_view luaSource, const NodeRegistry* nodes,
               const ImportOptions& opts, Graph& out, Diagnostics& diag);

bool importLuaFile(const std::string& path, const NodeRegistry* nodes,
                   const ImportOptions& opts, Graph& out, Diagnostics& diag);

}
