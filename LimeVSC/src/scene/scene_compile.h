#pragma once

#include "asset/asset_db.h"
#include "scene/scene.h"

namespace lime {

struct SceneCompileResult {
    bool        ok = false;
    std::string lua;
};

SceneCompileResult compileScene(const Scene& s, const ComponentRegistry& comps,
                                const std::string& sourceName, Diagnostics& diag,
                                const AssetDatabase* assets = nullptr);

struct BrokenRef {
    std::string entity;
    std::string component;
    std::string prop;
    std::string guid;
    std::string lastKnownPath;
};

std::vector<BrokenRef> findBrokenRefs(const Scene& s,
                                      const ComponentRegistry& comps,
                                      const AssetDatabase& assets);

std::string sceneRuntimeLua();

std::string sceneBootLua(const std::string& startModule);

}
