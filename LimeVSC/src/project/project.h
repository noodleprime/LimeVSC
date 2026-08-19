#pragma once

#include "limecore.h"

#include <cstdint>

#include <string>
#include <vector>

namespace lime {

enum class ProjectMode : std::uint8_t { Framework, Engine };

const char* projectModeName(ProjectMode m);

class ProjectContext {
public:
    std::string root;
    std::string limeBuilder;
    std::vector<std::string> limeFiles;
    std::vector<std::string> luaFiles;
    std::vector<std::string> sceneFiles;

    ProjectMode mode = ProjectMode::Framework;
    std::string startScene;

    bool isEngine() const { return mode == ProjectMode::Engine; }
    std::string projectFile() const;

    void loadSettings(Diagnostics& diag);
    bool saveSettings(Diagnostics& diag) const;

    bool valid() const;
    void scan();

    std::string contentDir() const;
    std::string appExe() const;
    std::string outputLog() const;

    static std::string findLimeBuilder(const std::string& hint);
    static std::string findTemplate(const std::string& hint);
};

struct LoadedMap {
    std::string luaPath;
    std::string limePath;
    SourceMap   map;
};

bool cookScenes(ProjectContext& proj, Diagnostics& diag);
std::string sceneModuleName(const ProjectContext& proj);

bool compileProject(ProjectContext& proj, const NodeRegistry& nodes,
                    const TypeRegistry& types, const EmitterRegistry& emitters,
                    std::vector<LoadedMap>& mapsOut, Diagnostics& diag);

bool packProject(const ProjectContext& proj, std::string& outputText,
                 Diagnostics& diag);

bool packageProject(const ProjectContext& proj, Diagnostics& diag);

bool createProject(const std::string& templateDir, const std::string& dest,
                   Diagnostics& diag);

std::vector<Diagnostic> mapRuntimeErrors(const std::string& logText,
                                         const std::vector<LoadedMap>& maps);

}
