#pragma once

#include "limecore.h"

#include <string>
#include <vector>

namespace lime {

struct AppSettings {
    std::string projectsDir;

    std::string externalEditor;
    bool        useExternalEditor = false;

    static constexpr std::size_t kDefaultUndoLimit = 128;
    static constexpr std::size_t kMaxUndoLimit = 100000;
    std::size_t undoLimit = kDefaultUndoLimit;

    std::vector<std::string> recentProjects;
    static constexpr std::size_t kMaxRecent = 10;

    void noteProject(const std::string& root);
    void forgetProject(const std::string& root);

    static std::string filePath();
    static std::string defaultProjectsDir();

    struct EditorChoice {
        const char* name;
        const char* command;
    };
    static std::vector<EditorChoice> knownEditors();
    int  matchKnownEditor() const;

    void load(Diagnostics& diag);
    bool save(Diagnostics& diag) const;

    bool openExternally(const std::string& path) const;
};

}
