#include "project/project.h"
#include "asset/asset_db.h"
#include "scene/component_provider.h"
#include "scene/scene_compile.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>

#include <toml.hpp>
#include <sstream>

namespace fs = std::filesystem;

namespace lime {
namespace {

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path findUp(const std::string& hint, const fs::path& rel) {
    std::error_code ec;
    fs::path p = fs::absolute(hint, ec);
    for (int up = 0; up < 7 && !p.empty(); ++up, p = p.parent_path())
        if (fs::exists(p / rel, ec)) return p / rel;
    return {};
}

}

const char* projectModeName(ProjectMode m) {
    return m == ProjectMode::Engine ? "engine" : "framework";
}

std::string ProjectContext::projectFile() const {
    return (fs::path(root) / "project.limeproj").string();
}

void ProjectContext::loadSettings(Diagnostics& diag) {
    mode = ProjectMode::Framework;
    startScene.clear();

    std::error_code ec;
    const std::string path = projectFile();
    if (root.empty() || !fs::exists(path, ec)) return;

    toml::parse_result res = toml::parse_file(path);
    if (!res) {
        diag.error("failed to parse " + path + ": "
                   + std::string(res.error().description()));
        return;
    }
    const toml::table& t = res.table();

    const std::string m = t["project"]["mode"].value_or(std::string{"framework"});
    if (m == "engine") mode = ProjectMode::Engine;
    else if (m != "framework")
        diag.warn("unknown project mode '" + m + "', treating as framework");

    startScene = t["engine"]["startScene"].value_or(std::string{});
}

bool ProjectContext::saveSettings(Diagnostics& diag) const {
    if (root.empty()) return false;

    std::ostringstream out;
    out << "# LimeVSC project settings.\n"
        << "# Deleting this file returns the project to Framework mode - the\n"
        << "# absence of it IS framework, so nothing else has to change.\n\n"
        << "[project]\n"
        << "schema = 1\n"
        << "mode   = \"" << projectModeName(mode) << "\"\n";

    if (mode == ProjectMode::Engine) {
        out << "\n[engine]\n"
            << "startScene = \""
            << (startScene.empty() ? std::string("content/main.limescene")
                                   : startScene)
            << "\"\n";
    }

    const std::string text = out.str();
    std::ofstream f(projectFile(), std::ios::binary);
    if (!f) {
        diag.error("cannot write " + projectFile());
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

bool ProjectContext::valid() const {
    std::error_code ec;
    return !root.empty() && fs::exists(fs::path(root) / "content", ec);
}

std::string ProjectContext::contentDir() const {
    return (fs::path(root) / "content").string();
}
std::string ProjectContext::appExe() const {
    return (fs::path(root) / "app.exe").string();
}
std::string ProjectContext::outputLog() const {
    return (fs::path(root) / "output.log").string();
}

void ProjectContext::scan() {
    limeFiles.clear();
    luaFiles.clear();
    sceneFiles.clear();

    Diagnostics ignored;
    loadSettings(ignored);
    std::error_code ec;
    const fs::path content = fs::path(root) / "content";
    if (!fs::exists(content, ec)) return;

    for (auto it = fs::recursive_directory_iterator(content, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext == ".lime")           limeFiles.push_back(it->path().string());
        else if (ext == ".lua")       luaFiles.push_back(it->path().string());
        else if (ext == ".limescene") sceneFiles.push_back(it->path().string());
    }
    std::sort(limeFiles.begin(), limeFiles.end());
    std::sort(luaFiles.begin(), luaFiles.end());
    std::sort(sceneFiles.begin(), sceneFiles.end());
}

namespace {
std::string findToolchain(const std::string& hint, const fs::path& rel) {
    if (!hint.empty()) {
        const fs::path p = findUp(hint, rel);
        if (!p.empty()) return p.string();
    }
    const fs::path p = findUp(LIMEVSC_DATA_DIR, rel);
    return p.empty() ? std::string{} : p.string();
}
}

std::string ProjectContext::findLimeBuilder(const std::string& hint) {
    return findToolchain(hint,
                         fs::path("LimeX") / "LimeVS" / "cmd" / "LimeBuilder.exe");
}

std::string ProjectContext::findTemplate(const std::string& hint) {
    return findToolchain(hint, fs::path("LimeX") / "LimeVS" / "template");
}

bool compileProject(ProjectContext& proj, const NodeRegistry& nodes,
                    const TypeRegistry& types, const EmitterRegistry& emitters,
                    std::vector<LoadedMap>& mapsOut, Diagnostics& diag) {
    proj.scan();
    mapsOut.clear();
    bool ok = true;

    for (const std::string& limePath : proj.limeFiles) {
        Diagnostics rd;
        Graph g;
        if (!readLime(limePath, g, rd)) {
            for (const Diagnostic& d : rd.all()) diag.add(d);
            ok = false;
            continue;
        }

        const fs::path out = generatedLuaPath(limePath);
        std::error_code oec;
        fs::create_directories(out.parent_path(), oec);
        Diagnostics cd;
        const CompileResult r =
            compileGraph(g, nodes, types, emitters,
                         fs::path(limePath).filename().string(), cd);
        for (const Diagnostic& d : cd.all()) diag.add(d);
        if (!r.ok) { ok = false; continue; }

        std::ofstream f(out, std::ios::binary);
        if (!f) {
            diag.error("cannot write " + out.string());
            ok = false;
            continue;
        }
        f.write(r.lua.data(), static_cast<std::streamsize>(r.lua.size()));
        f.close();

        mapsOut.push_back({out.string(), limePath, r.map});
    }

    if (proj.isEngine() && !proj.sceneFiles.empty())
        ok = cookScenes(proj, diag) && ok;

    return ok;
}

namespace {

const char* const kGenDir = "content/Scripts/Generated/";

std::string slashed(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}

}

std::string generatedLuaPath(const std::string& limePath) {
    const std::string p = slashed(limePath);
    const std::size_t at = p.find("content/");
    if (at == std::string::npos)
        return fs::path(limePath).replace_extension(".lua").string();
    return p.substr(0, at) + kGenDir + fs::path(p).stem().string() + ".lua";
}

std::string graphModuleName(const std::string& limePath) {
    const std::string p = slashed(limePath);
    const std::string stem = fs::path(p).stem().string();
    if (p.find("content/") == std::string::npos) return stem;
    std::string rel = std::string(kGenDir) + stem;
    for (char& c : rel) if (c == '/') c = '.';
    return rel;
}

bool isGeneratedLuaPath(const std::string& luaPath) {
    return slashed(luaPath).find(kGenDir) != std::string::npos;
}

std::string sceneModuleName(const ProjectContext& proj) {
    std::string rel = proj.startScene;
    if (rel.empty() && !proj.sceneFiles.empty()) {
        std::error_code ec;
        rel = fs::relative(proj.sceneFiles.front(), proj.root, ec).generic_string();
        if (ec) return {};
    }
    if (rel.empty()) return {};

    for (char& c : rel)
        if (c == '\\') c = '/';
    const std::string kExt = ".limescene";
    if (rel.size() > kExt.size()
        && rel.compare(rel.size() - kExt.size(), kExt.size(), kExt) == 0)
        rel.resize(rel.size() - kExt.size());
    rel += "_scene";
    for (char& c : rel)
        if (c == '/') c = '.';
    return rel;
}

bool cookScenes(ProjectContext& proj, Diagnostics& diag) {
    TypeRegistry types;
    ComponentRegistry comps;
    const std::string dataDir = LIMEVSC_DATA_DIR;
    comps.addProvider(std::make_unique<ComponentFileProvider>(
        dataDir + "/core.limecomponents", "core", kComponentPriorityCore));
    const std::string projComps = proj.root + "/components.limecomponents";
    std::error_code ec;
    if (fs::exists(projComps, ec))
        comps.addProvider(std::make_unique<ComponentFileProvider>(
            projComps, "project", kComponentPriorityProject));
    comps.rebuild(types, diag);

    AssetTypeRegistry assetTypes;
    assetTypes.loadFile(dataDir + "/core.limeassets", diag);
    const std::string projAssets = proj.root + "/assets.limeassets";
    if (fs::exists(projAssets, ec)) assetTypes.loadFile(projAssets, diag);
    AssetDatabase assets;
    assets.scan(proj.root, assetTypes, diag);

    bool ok = true;
    for (const std::string& scenePath : proj.sceneFiles) {
        Scene sc;
        Diagnostics rd;
        if (!readScene(scenePath, sc, rd)) {
            for (const Diagnostic& d : rd.all()) diag.add(d);
            ok = false;
            continue;
        }
        for (const Diagnostic& d : rd.all()) diag.add(d);

        Diagnostics cd;
        for (const BrokenRef& b : findBrokenRefs(sc, comps, assets))
            cd.error("missing asset: entity '" + b.entity + "' " + b.component
                     + "." + b.prop
                     + (b.lastKnownPath.empty()
                            ? std::string(" [unknown guid ") + b.guid + "]"
                            : " was " + b.lastKnownPath));

        const SceneCompileResult r = compileScene(
            sc, comps, fs::path(scenePath).filename().string(), cd, &assets);
        for (const Diagnostic& d : cd.all()) diag.add(d);
        if (!r.ok) { ok = false; continue; }

        const fs::path out = fs::path(scenePath).parent_path()
                             / (fs::path(scenePath).stem().string() + "_scene.lua");
        std::ofstream f(out, std::ios::binary);
        if (!f) {
            diag.error("cannot write " + out.string());
            ok = false;
            continue;
        }
        f.write(r.lua.data(), static_cast<std::streamsize>(r.lua.size()));
    }

    const fs::path runtime = fs::path(proj.root) / "content" / "lime_scene.lua";
    if (!fs::exists(runtime, ec)) {
        const std::string text = sceneRuntimeLua();
        std::ofstream f(runtime, std::ios::binary);
        if (f) f.write(text.data(), static_cast<std::streamsize>(text.size()));
        else { diag.error("cannot write " + runtime.string()); ok = false; }
    }

    const std::string startModule = sceneModuleName(proj);
    if (!startModule.empty()) {
        const std::string text = sceneBootLua(startModule);
        const fs::path boot = fs::path(proj.root) / "content" / "lime_boot.lua";
        std::ofstream f(boot, std::ios::binary);
        if (f) f.write(text.data(), static_cast<std::streamsize>(text.size()));
        else { diag.error("cannot write " + boot.string()); ok = false; }
    } else {
        diag.warn("no start scene set, so nothing will load at launch - "
                  "right-click a scene and choose Set as Start Scene");
    }
    return ok;
}

bool packProject(const ProjectContext& proj, std::string& outputText,
                 Diagnostics& diag) {
    if (proj.limeBuilder.empty()) {
        diag.error("LimeBuilder.exe not found - is a LimeX checkout nearby?");
        return false;
    }

    std::string cmd = "\"\"" + proj.limeBuilder + "\" \"" + proj.root + "\"\"";
    outputText.clear();

    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        diag.error("failed to launch LimeBuilder");
        return false;
    }
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) outputText += buf;
    const int rc = _pclose(pipe);
    if (rc != 0) {
        diag.error("LimeBuilder failed (exit " + std::to_string(rc) + ")");
        return false;
    }
    return true;
}

bool packageProject(const ProjectContext& proj, Diagnostics& diag) {
    std::error_code ec;
    const fs::path root(proj.root);
    const fs::path bin = root / "bin";

    std::vector<std::string> ignored = {".limepkg", ".ico", ".exp", ".lib",
                                        ".pdb", ".log", ".lime", ".map",
                                        ".limescene", ".limeasset", ".limeproj",
                                        ".limecomponents", ".limeassets",
                                        ".limenodes", ".limetypes"};
    if (fs::exists(root / ".ignore", ec)) {
        std::istringstream ss(readAll(root / ".ignore"));
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty()) ignored.push_back(line);
        }
    }

    fs::remove_all(bin, ec);
    fs::create_directories(bin, ec);

    int copied = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path rel = fs::relative(it->path(), root, ec);
        if (rel.empty()) continue;
        if (rel.begin() != rel.end() && *rel.begin() == "bin") continue;
        if (!it->is_regular_file(ec)) continue;

        const std::string ext = it->path().extension().string();
        if (std::find(ignored.begin(), ignored.end(), ext) != ignored.end())
            continue;

        const fs::path dst = bin / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++copied;
    }

    diag.info("packaged " + std::to_string(copied) + " files to " + bin.string());
    return copied > 0;
}

bool createProject(const std::string& templateDir, const std::string& dest,
                   Diagnostics& diag) {
    std::error_code ec;
    if (templateDir.empty() || !fs::exists(templateDir, ec)) {
        diag.error("project template not found");
        return false;
    }
    fs::create_directories(dest, ec);
    fs::copy(templateDir, dest,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        diag.error("failed to copy template: " + ec.message());
        return false;
    }
    return true;
}

std::vector<Diagnostic> mapRuntimeErrors(const std::string& logText,
                                         const std::vector<LoadedMap>& maps) {
    std::vector<Diagnostic> out;

    static const std::regex kModule(R"(\[string \"([^\"]+)\"\]:(\d+):\s*(.*))");
    static const std::regex kFile(R"(([\w./\\-]+\.lua):(\d+):\s*(.*))");

    std::istringstream ss(logText);
    std::string line;
    while (std::getline(ss, line)) {
        std::smatch m;
        std::string unit;
        int lineNo = 0;
        std::string msg;

        if (std::regex_search(line, m, kModule)) {
            unit = m[1].str();
            std::replace(unit.begin(), unit.end(), '.', '/');
            unit += ".lua";
            lineNo = std::atoi(m[2].str().c_str());
            msg = m[3].str();
        } else if (std::regex_search(line, m, kFile)) {
            unit = m[1].str();
            std::replace(unit.begin(), unit.end(), '\\', '/');
            lineNo = std::atoi(m[2].str().c_str());
            msg = m[3].str();
        } else {
            continue;
        }

        Diagnostic d;
        d.severity = Severity::Error;
        d.message = msg;
        d.line = lineNo;

        for (const LoadedMap& lm : maps) {
            std::string norm = lm.luaPath;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            if (norm.size() < unit.size()) continue;
            if (norm.compare(norm.size() - unit.size(), unit.size(), unit) != 0)
                continue;
            d.file = lm.limePath;
            d.node = lm.map.nodeForLine(lineNo);
            break;
        }
        out.push_back(std::move(d));
    }
    return out;
}

}
