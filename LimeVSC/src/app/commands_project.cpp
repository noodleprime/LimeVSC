#include "app/editor.h"
#include "project/project.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace lime {
namespace {

void report(EditorContext& ed, const Diagnostics& d) { ed.report(d); }

std::string readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool hasProject(EditorContext& ed) { return ed.project.valid(); }

int CALLBACK browseInit(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data)
        SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, data);
    return 0;
}

}

std::string pickFolder(const char* title, const std::string& startAt) {
    BROWSEINFOA bi{};
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!startAt.empty()) {
        bi.lpfn = browseInit;
        bi.lParam = reinterpret_cast<LPARAM>(startAt.c_str());
    }
    LPITEMIDLIST id = SHBrowseForFolderA(&bi);
    if (!id) return {};
    char buf[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListA(id, buf) != FALSE;
    CoTaskMemFree(id);
    return ok ? std::string(buf) : std::string{};
}

namespace {

std::string saveFileDialog(const char* title, const std::string& startDir,
                           const char* defaultName, bool sceneFilter = false,
                           bool luaFilter = false) {
    char buf[MAX_PATH]{};
    std::snprintf(buf, sizeof(buf), "%s", defaultName);

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = luaFilter   ? "Lua script (*.lua)\0*.lua\0All files\0*.*\0"
                    : sceneFilter ? "LimeVSC scene (*.limescene)\0*.limescene\0All files\0*.*\0"
                                  : "LimeVSC graph (*.lime)\0*.lime\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = luaFilter ? "lua" : (sceneFilter ? "limescene" : "lime");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!startDir.empty()) ofn.lpstrInitialDir = startDir.c_str();

    if (!GetSaveFileNameA(&ofn)) return {};
    return std::string(buf);
}

bool buildProject(EditorContext& ed) {
    Diagnostics d;
    ed.saveAndCompile();

    if (!compileProject(ed.project, ed.nodes, ed.types, ed.emitters, ed.maps, d)) {
        report(ed, d);
        ed.log("build failed: compilation errors");
        return false;
    }
    report(ed, d);

    std::string out;
    if (!packProject(ed.project, out, d)) {
        report(ed, d);
        return false;
    }
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) ed.log(line);
    }
    return true;
}

void runProject(EditorContext& ed) {
    const std::string exe = ed.project.appExe();
    if (!fs::exists(exe)) {
        ed.log("app.exe not found - build first");
        return;
    }
    std::error_code ec;
    fs::remove(ed.project.outputLog(), ec);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = exe;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        ed.project.root.c_str(), &si, &pi)) {
        ed.log("failed to launch " + exe);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ed.log("launched " + exe);
}

void collectRuntimeErrors(EditorContext& ed) {
    const std::string log = ed.project.outputLog();
    if (!fs::exists(log)) return;

    const std::string text = readAll(log);
    const std::vector<Diagnostic> errs = mapRuntimeErrors(text, ed.maps);
    if (errs.empty()) return;

    ed.previewErrors().clear();
    for (const Diagnostic& d : errs) {
        const std::string where =
            d.node.valid() ? (" [node " + encodeId(d.node.v) + "]") : std::string();
        ed.log("runtime error: " + d.message + where);
        ed.previewErrors().push_back({d.node, d.message});
    }
    if (errs.front().node.valid()) {
        ed.selection() = {errs.front().node};
        ed.inspected() = errs.front().node;
    }
}

}

bool EditorContext::createProjectAt(const std::string& dest, ProjectMode mode,
                                    bool mainIsScript) {
    const bool engine = mode == ProjectMode::Engine;
    closeProject();

    Diagnostics d;
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        note(NoteKind::Error, "cannot create " + dest);
        return false;
    }

    const std::string tpl = ProjectContext::findTemplate(dest);
    if (!createProject(tpl, dest, d)) {
        report(d);
        return false;
    }

    fs::remove(fs::path(dest) / "content" / "main.lua", ec);

    for (const char* sub : {"Scenes", "Graphs", "Scripts", "Assets"})
        fs::create_directories(fs::path(dest) / "content" / sub, ec);

    project.root = dest;
    project.limeBuilder = ProjectContext::findLimeBuilder(dest);
    project.mode = mode;

    if (engine) {
        project.startScene = "content/Scenes/main.limescene";
        project.saveSettings(d);
        newScene(
            (fs::path(dest) / "content" / "Scenes" / "main.limescene").string(),
            "Main");
    }

    if (mainIsScript) {
        const fs::path p = fs::path(dest) / "content" / "Scripts" / "main.lua";
        std::string body = R"LUA(Lime.onInit:hook(function()
    Lime.setInitConfig(Lime.Enum.DriverType.Direct3D9, Vec2.new(640, 480))
end)

)LUA";
        if (engine) body += R"LUA(require("content.lime_boot")

)LUA";
        body += R"LUA(Lime.onStart:hook(function()
    Lime.log("Hello, World!")
end)

Lime.onUpdate:hook(function(dt)
end)

Lime.onClose:hook(function()
end)
)LUA";
        std::ofstream f(p, std::ios::binary);
        if (f) f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.close();
        openDoc(p.string());
    } else {
        newGraph((fs::path(dest) / "content" / "Graphs" / "main.lime").string(),
                 true);
        if (engine) seedStartScene();
    }
    if (!mainIsScript) saveAndCompile();
    project.scan();
    rebuildGraphFunctions();
    report(d);
    settings.noteProject(dest);
    Diagnostics sd;
    settings.save(sd);
    log(std::string("created ") + projectModeName(project.mode) + " project at "
        + dest);
    note(NoteKind::Action, "Created " + std::string(projectModeName(project.mode))
                               + " project");
    return true;
}

void registerProjectCommands(CommandRegistry& reg) {
    reg.add({"project.new", "New Project...", "Project", "",
             [](EditorContext& ed) { ed.showNewProject = true; }, nullptr});

    reg.add({"project.convert", "Convert to Engine...", "Project", "",
             [](EditorContext& ed) {
                 if (MessageBoxA(
                         nullptr,
                         "Convert this project to ENGINE mode?\n\n"
                         "This ADDS scenes, entities and components. Nothing is "
                         "removed or rewritten - every existing graph keeps "
                         "working exactly as it does now.\n\n"
                         "To go back, delete project.limeproj.",
                         "Convert to Engine", MB_OKCANCEL | MB_ICONINFORMATION)
                     != IDOK)
                     return;

                 Diagnostics d;
                 ed.project.mode = ProjectMode::Engine;
                 ed.project.startScene = "content/main.limescene";
                 if (!ed.project.saveSettings(d)) { report(ed, d); return; }

                 const fs::path scene =
                     fs::path(ed.project.root) / "content" / "main.limescene";
                 std::error_code ec;
                 if (!fs::exists(scene, ec)) ed.newScene(scene.string(), "Main");
                 else ed.openScene(scene.string());

                 ed.project.scan();
                 report(ed, d);
                 ed.log("converted to Engine mode - existing graphs untouched");
             },
             [](EditorContext& ed) {
                 return ed.project.valid() && !ed.project.isEngine();
             }});

    reg.add({"project.open", "Open Project...", "Project", "Ctrl+Shift+O",
             [](EditorContext& ed) {
                 const std::string start = ed.settings.projectsDir.empty()
                                               ? std::string()
                                               : ed.settings.projectsDir;
                 const std::string root =
                     pickFolder("Choose a project folder", start);
                 if (!root.empty()) ed.queueOpenProject(root);
             },
             nullptr});

    reg.add({"file.new", "New Graph...", "File", "Ctrl+N",
             [](EditorContext& ed) {
                 const std::string start =
                     ed.project.valid() ? ed.project.contentDir() : std::string();
                 const std::string path =
                     saveFileDialog("New graph", start, "graph.lime");
                 if (path.empty()) return;

                 ed.newGraph(path,  false);
                 ed.saveAndCompile();
                 if (ed.project.valid()) {
                     ed.project.scan();
                     ed.rebuildGraphFunctions();
                 }
             },
             nullptr});

    reg.add({"file.newLua", "New Lua Script...", "File", "",
             [](EditorContext& ed) {
                 const std::string start =
                     ed.project.valid() ? ed.project.contentDir() : std::string();
                 const std::string path =
                     saveFileDialog("New Lua script", start, "script.lua",
                                     false,  true);
                 if (path.empty()) return;
                 ed.addDoc();
                 ed.newTextFile(path);
                 if (ed.project.valid()) ed.project.scan();
             },
             nullptr});

    reg.add({"scene.new", "New Scene...", "File", "",
             [](EditorContext& ed) {
                 const std::string start =
                     ed.project.valid() ? ed.project.contentDir() : std::string();
                 const std::string path =
                     saveFileDialog("New scene", start, "level.limescene",
                                     true);
                 if (path.empty()) return;
                 ed.newScene(path, fs::path(path).stem().string());
                 if (ed.project.valid()) ed.project.scan();
             },
             [](EditorContext& ed) { return ed.project.isEngine(); }});

    reg.add({"scene.save", "Save Scene", "File", "",
             [](EditorContext& ed) { ed.saveScene(); },
             [](EditorContext& ed) { return !ed.scenePath.empty(); }});

    reg.add({"scene.setStart", "Set as Start Scene", "Project", "",
             [](EditorContext& ed) {
                 std::error_code ec;
                 const fs::path rel =
                     fs::relative(ed.scenePath, ed.project.root, ec);
                 const std::string s = ec ? ed.scenePath : rel.generic_string();
                 ed.project.startScene = s;
                 Diagnostics d;
                 ed.project.saveSettings(d);
                 report(ed, d);
                 ed.log("start scene is now " + s);
             },
             [](EditorContext& ed) {
                 return ed.project.isEngine() && !ed.scenePath.empty();
             }});

    reg.add({"file.saveAs", "Save As...", "File", "",
             [](EditorContext& ed) {
                 const std::string start =
                     ed.project.valid() ? ed.project.contentDir() : std::string();
                 const std::string path =
                     saveFileDialog("Save graph", start, "graph.lime");
                 if (path.empty()) return;
                 ed.filePath() = path;
                 ed.saveAndCompile();
                 if (ed.project.valid()) {
                     ed.project.scan();
                     ed.rebuildGraphFunctions();
                 }
             },
             nullptr});

    reg.add({"project.compile", "Compile Graphs", "Project", "Ctrl+B",
             [](EditorContext& ed) {
                 Diagnostics d;
                 ed.saveAndCompile();
                 if (compileProject(ed.project, ed.nodes, ed.types, ed.emitters,
                                    ed.maps, d))
                     ed.log("compiled "
                            + std::to_string(ed.project.limeFiles.size())
                            + " graph(s)");
                 report(ed, d);
             },
             hasProject});

    reg.add({"project.build", "Build", "Project", "F7",
             [](EditorContext& ed) { buildProject(ed); },
             hasProject});

    reg.add({"project.run", "Run", "Project", "F5",
             [](EditorContext& ed) { runProject(ed); },
             hasProject});

    reg.add({"project.buildRun", "Build and Run", "Project", "Ctrl+F5",
             [](EditorContext& ed) { if (buildProject(ed)) runProject(ed); },
             hasProject});

    reg.add({"project.errors", "Read Run Log", "Project", "",
             [](EditorContext& ed) { collectRuntimeErrors(ed); },
             hasProject});

    reg.add({"project.package", "Package", "Project", "",
             [](EditorContext& ed) {
                 Diagnostics d;
                 if (!buildProject(ed)) return;
                 packageProject(ed.project, d);
                 report(ed, d);
             },
             hasProject});
}

}
