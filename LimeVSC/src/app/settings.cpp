#include "app/settings.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <toml.hpp>

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

namespace fs = std::filesystem;

namespace lime {
namespace {

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &wide))) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                      nullptr, nullptr);
    std::string out(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    CoTaskMemFree(wide);
    return out;
}

std::string quoted(const std::string& s) {
    std::string q = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q + "\"";
}

}

std::vector<AppSettings::EditorChoice> AppSettings::knownEditors() {
    return {
        {"Visual Studio Code",      "code"},
        {"VS Code Insiders",        "code-insiders"},
        {"Cursor",                  "cursor"},
        {"Zed",                     "zed"},
        {"Sublime Text",            "subl"},
        {"Notepad++",               "notepad++"},
        {"Neovim (in a terminal)",  "cmd /c start nvim"},
        {"Vim (in a terminal)",     "cmd /c start vim"},
        {"Emacs",                   "emacs"},
        {"JetBrains Rider",         "rider64"},
        {"IntelliJ IDEA",           "idea64"},
        {"CLion",                   "clion64"},
        {"PyCharm",                 "pycharm64"},
        {"WebStorm",                "webstorm64"},
        {"Visual Studio",           "devenv"},
        {"Kate",                    "kate"},
        {"Geany",                   "geany"},
        {"Atom",                    "atom"},
        {"Brackets",                "brackets"},
        {"ZeroBrane Studio",        "zbstudio"},
        {"Lite XL",                 "lite-xl"},
        {"Helix",                   "cmd /c start hx"},
        {"Notepad",                 "notepad"},
        {"WordPad",                 "write"},
    };
}

int AppSettings::matchKnownEditor() const {
    const std::vector<EditorChoice> list = knownEditors();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (externalEditor == list[i].command) return static_cast<int>(i);
    return -1;
}

std::string AppSettings::filePath() {
    const std::string appData = knownFolder(FOLDERID_RoamingAppData);
    if (appData.empty()) return "limevsc-settings.toml";
    std::error_code ec;
    const fs::path dir = fs::path(appData) / "LimeVSC";
    fs::create_directories(dir, ec);
    return (dir / "settings.toml").string();
}

std::string AppSettings::defaultProjectsDir() {
    const std::string docs = knownFolder(FOLDERID_Documents);
    if (docs.empty()) return {};
    std::error_code ec;
    const fs::path dir = fs::path(docs) / "LimeVSC Projects";
    fs::create_directories(dir, ec);
    return dir.string();
}

void AppSettings::noteProject(const std::string& root) {
    if (root.empty()) return;
    recentProjects.erase(
        std::remove(recentProjects.begin(), recentProjects.end(), root),
        recentProjects.end());
    recentProjects.insert(recentProjects.begin(), root);
    if (recentProjects.size() > kMaxRecent) recentProjects.resize(kMaxRecent);
}

void AppSettings::forgetProject(const std::string& root) {
    recentProjects.erase(
        std::remove(recentProjects.begin(), recentProjects.end(), root),
        recentProjects.end());
}

void AppSettings::load(Diagnostics& diag) {
    projectsDir = defaultProjectsDir();
    externalEditor.clear();
    useExternalEditor = false;
    recentProjects.clear();

    const std::string path = filePath();
    std::error_code ec;
    if (!fs::exists(path, ec)) return;

    toml::parse_result res = toml::parse_file(path);
    if (!res) {
        diag.warn("could not read settings: "
                  + std::string(res.error().description()));
        return;
    }
    const toml::table& t = res.table();

    if (auto s = t["paths"]["projectsDir"].value<std::string>();
        s && !s->empty())
        projectsDir = *s;
    externalEditor = t["editor"]["command"].value_or(std::string{});
    useExternalEditor = t["editor"]["external"].value_or(false);

    const std::int64_t lim = t["undo"]["limit"].value_or<std::int64_t>(
        static_cast<std::int64_t>(kDefaultUndoLimit));
    undoLimit = static_cast<std::size_t>(
        std::clamp<std::int64_t>(lim, 0, static_cast<std::int64_t>(kMaxUndoLimit)));

    if (const toml::array* arr = t["recent"]["projects"].as_array())
        for (const toml::node& n : *arr)
            if (auto s = n.value<std::string>(); s && !s->empty())
                recentProjects.push_back(*s);
    if (recentProjects.size() > kMaxRecent) recentProjects.resize(kMaxRecent);
}

bool AppSettings::save(Diagnostics& diag) const {
    std::ostringstream o;
    o << "# LimeVSC settings. Per user, not per project - which editor you\n"
      << "# like is a fact about you, not about the game.\n\n"
      << "[paths]\n"
      << "projectsDir = " << quoted(projectsDir) << "\n\n"
      << "[editor]\n"
      << "# external = false uses the built-in .lua editor.\n"
      << "external = " << (useExternalEditor ? "true" : "false") << "\n"
      << "command  = " << quoted(externalEditor) << "\n\n"
      << "[undo]\n"
      << "# How many edits Ctrl+Z walks back, across every open document.\n"
      << "# 0 keeps everything.\n"
      << "limit = " << undoLimit << "\n\n"
      << "[recent]\n"
      << "projects = [\n";
    for (const std::string& r : recentProjects) o << "  " << quoted(r) << ",\n";
    o << "]\n";

    const std::string text = o.str();
    std::ofstream f(filePath(), std::ios::binary);
    if (!f) {
        diag.error("cannot write " + filePath());
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

bool AppSettings::openExternally(const std::string& path) const {
    if (!useExternalEditor || externalEditor.empty()) return false;

    std::string cmd = externalEditor;
    const std::size_t at = cmd.find("{file}");
    if (at != std::string::npos) cmd.replace(at, 6, "\"" + path + "\"");
    else                         cmd += " \"" + path + "\"";

    std::string program = cmd;
    std::string args;
    if (const std::size_t sp = cmd.find(' '); sp != std::string::npos) {
        program = cmd.substr(0, sp);
        args = cmd.substr(sp + 1);
    }
    const HINSTANCE rc = ShellExecuteA(nullptr, "open", program.c_str(),
                                       args.empty() ? nullptr : args.c_str(),
                                       nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

}
