#include "ui/panels.h"

#include <imgui.h>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lime {
namespace {

std::string folderOf(const std::string& rel) {
    const std::size_t slash = rel.find_last_of('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::string fileOf(const std::string& rel) {
    const std::size_t slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

constexpr const char* kEmpty = "No content present";

bool matches(const AssetRecord& r, const char* filter) {
    if (!filter || !filter[0]) return true;
    return r.relPath.find(filter) != std::string::npos;
}

class ContentPanel final : public IPanel {
public:
    std::string_view id() const override { return "content"; }
    std::string_view title() const override { return "Content"; }

    void draw(EditorContext& e) override {
        if (!e.project.valid()) {
            drawEmptyState(e);
            return;
        }
        drawToolbar(e);
        drawTree(e);
        drawRenameModal(e);
        drawCreateModal(e);
    }

private:
    std::string renameTarget;
    char        renameBuf[128] = "";
    bool        openRename = false;
    char        filter[64] = {};
    std::string createDir;
    int         createKind = 0;
    char        createBuf[128] = "";
    bool        openCreate = false;
    bool        createFocus = false;

    static std::string fileName(const std::string& path) {
        return path.substr(path.find_last_of("/\\") + 1);
    }

    void drawEmptyState(EditorContext& e) {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ImGui::Button("New Project...", ImVec2(half, 0)))
            e.commands.invoke("project.new", e);
        ImGui::SameLine();
        if (ImGui::Button("Open Project...", ImVec2(half, 0)))
            e.commands.invoke("project.open", e);
        ImGui::Spacing();

        if (e.settings.recentProjects.empty()) {
            ImGui::TextDisabled("%s", kEmpty);
            return;
        }

        const float clearW = ImGui::CalcTextSize("Clear").x
                             + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float lineW = ImGui::GetContentRegionAvail().x;
        ImGui::SeparatorText("Recent");
        ImGui::SameLine(lineW - clearW);
        if (ImGui::SmallButton("Clear")) {
            e.settings.recentProjects.clear();
            Diagnostics cd;
            e.settings.save(cd);
            return;
        }

        const std::vector<std::string> recent = e.settings.recentProjects;
        std::string pick;
        for (const std::string& r : recent) {
            std::error_code ec;
            const bool there = std::filesystem::exists(r, ec);

            const std::string name = fileName(r);
            ImGui::PushID(r.c_str());
            if (!there)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(name.c_str()) && there) pick = r;
            if (!there) ImGui::PopStyleColor();

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s%s", r.c_str(),
                    there ? "" : "\n\nThis folder is gone.");
            ImGui::PopID();
        }

        if (!pick.empty()) e.queueOpenProject(pick);
    }

    static std::string shownRoot(const std::string& root) {
        namespace fs = std::filesystem;
        if (root.empty()) return root;

        const fs::path def(AppSettings::defaultProjectsDir());
        if (def.empty()) return root;

        const fs::path under = fs::path(root).lexically_relative(def);
        if (under.empty() || under.native().rfind(L"..", 0) == 0) return root;

        const fs::path base = def.parent_path().parent_path();
        if (base.empty()) return root;
        const fs::path shown = fs::path(root).lexically_relative(base);
        if (shown.empty() || shown.native().rfind(L"..", 0) == 0) return root;
        return shown.string();
    }

    void drawToolbar(EditorContext& e) {
        struct BuildAction {
            const char* label;
            const char* command;
            const char* shortcut;
        };
        static const BuildAction kActions[] = {
            {"Build+Run", "project.buildRun", "Ctrl+F5"},
            {"Build", "project.build", "F7"},
            {"Run", "project.run", "F5"},
        };
        static int chosen = 0;

        const ImGuiStyle& st = ImGui::GetStyle();
        const char*  label = kActions[chosen].label;
        const float  arrowW = ImGui::GetFrameHeight();
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float  w = ts.x + st.FramePadding.x * 2.0f + arrowW;
        const ImVec2 at = ImGui::GetCursorScreenPos();

        const bool pressed = ImGui::Button("##buildsplit", ImVec2(w, 0));
        const float h = ImGui::GetItemRectSize().y;
        const float sepX = at.x + w - arrowW;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
        dl->AddText(ImVec2(at.x + st.FramePadding.x, at.y + st.FramePadding.y),
                    fg, label);
        dl->AddLine(ImVec2(sepX, at.y + 3.0f), ImVec2(sepX, at.y + h - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator));
        const float cx = sepX + arrowW * 0.5f;
        const float cy = at.y + h * 0.5f;
        dl->AddTriangleFilled(ImVec2(cx - 3.5f, cy - 1.5f),
                              ImVec2(cx + 3.5f, cy - 1.5f),
                              ImVec2(cx, cy + 2.5f), fg);

        if (pressed) {
            if (ImGui::GetIO().MouseClickedPos[0].x >= sepX)
                ImGui::OpenPopup("buildmore");
            else
                e.commands.invoke(kActions[chosen].command, e);
        }

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", shownRoot(e.project.root).c_str());

        if (ImGui::BeginPopup("buildmore")) {
            for (int i = 0; i < IM_ARRAYSIZE(kActions); ++i)
                if (ImGui::MenuItem(kActions[i].label, kActions[i].shortcut,
                                    i == chosen)) {
                    chosen = i;
                    e.commands.invoke(kActions[i].command, e);
                }
            ImGui::EndPopup();
        }
        ImGui::Separator();
    }

    void drawTree(EditorContext& e) {
        namespace fs = std::filesystem;
        const std::string root = e.project.contentDir();
        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec)) {
            ImGui::TextDisabled("%s", kEmpty);
            return;
        }

        ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_None);
        drawDir(e, root);
        if (ImGui::BeginPopupContextWindow("rootctx",
                                           ImGuiPopupFlags_MouseButtonRight
                                               | ImGuiPopupFlags_NoOpenOverItems)) {
            drawCreateMenu(e, root);
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    void drawDir(EditorContext& e, const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<std::string> dirs, files;
        for (const fs::directory_entry& en : fs::directory_iterator(dir, ec)) {
            if (en.is_directory(ec)) dirs.push_back(en.path().string());
            else if (en.is_regular_file(ec)) files.push_back(en.path().string());
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());

        for (const std::string& d : dirs) {
            ImGui::PushID(d.c_str());
            const bool open = ImGui::TreeNodeEx(
                fileName(d).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::BeginPopupContextItem("dctx")) {
                drawCreateMenu(e, d);
                ImGui::EndPopup();
            }
            if (open) {
                drawDir(e, d);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        for (const std::string& f : files) drawFile(e, f);
    }

    void drawFile(EditorContext& e, const std::string& path) {
        namespace fs = std::filesystem;
        const std::string ext = fs::path(path).extension().string();
        const bool isScene = ext == ".limescene";
        const bool isGraph = ext == ".lime";
        const bool isLua = ext == ".lua";
        const bool generated = isLua && e.isGeneratedLua(path);

        bool active = false;
        if (isScene) active = (path == e.scenePath);
        else if (isGraph || isLua) active = (path == e.filePath());

        ImGui::PushID(path.c_str());
        if (generated) ImGui::PushStyleColor(ImGuiCol_Text,
                                             ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Selectable(fileName(path).c_str(), active)) openFile(e, path);
        if (generated) ImGui::PopStyleColor();

        if (ImGui::BeginPopupContextItem("fctx")) {
            if (isScene || isGraph || isLua)
                if (ImGui::MenuItem("Open")) openFile(e, path);
            if (isScene && ImGui::MenuItem("Set as Start Scene")) {
                if (path != e.scenePath) e.openScene(path);
                e.commands.invoke("scene.setStart", e);
            }
            if (isGraph && ImGui::MenuItem("Rename...")) {
                renameTarget = path;
                const std::string name = fileName(path);
                const std::size_t dot = name.find_last_of('.');
                std::snprintf(renameBuf, sizeof(renameBuf), "%s",
                              name.substr(0, dot).c_str());
                openRename = true;
            }
            ImGui::Separator();
            drawCreateMenu(e, fs::path(path).parent_path().string());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    static void openFile(EditorContext& e, const std::string& path) {
        const std::string ext = std::filesystem::path(path).extension().string();
        if (ext == ".limescene") {
            if (path == e.scenePath) return;
            if (e.sceneDirty) e.saveScene();
            e.openScene(path);
            return;
        }
        if (ext == ".lime" || ext == ".lua") e.openDoc(path);
    }

    void drawCreateMenu(EditorContext& e, const std::string& dir) {
        if (ImGui::MenuItem("New Folder...")) beginCreate(dir, 0, "New Folder");
        ImGui::Separator();
        if (ImGui::MenuItem("New Graph...")) beginCreate(dir, 1, "graph");
        if (e.project.isEngine())
            if (ImGui::MenuItem("New Scene...")) beginCreate(dir, 2, "scene");
        if (ImGui::MenuItem("New Script...")) beginCreate(dir, 3, "script");
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer")) {
            const std::string arg = "\"" + dir + "\"";
            ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr,
                          SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem("Rescan")) e.rescanAssets();
    }

    void beginCreate(const std::string& dir, int kind, const char* suggested) {
        createDir = dir;
        createKind = kind;
        std::snprintf(createBuf, sizeof(createBuf), "%s", suggested);
        openCreate = true;
        createFocus = true;
    }

    void drawCreateModal(EditorContext& e) {
        static const char* kTitles[] = {"New Folder", "New Graph", "New Scene",
                                        "New Script"};
        if (openCreate) {
            ImGui::OpenPopup("Create");
            openCreate = false;
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(380, 0),
                                            ImVec2(380, FLT_MAX));
        if (!ImGui::BeginPopupModal("Create", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::SeparatorText(kTitles[createKind]);
        ImGui::SetNextItemWidth(-1);
        if (createFocus) {
            ImGui::SetKeyboardFocusHere();
            createFocus = false;
        }
        const bool entered =
            ImGui::InputText("##cname", createBuf, sizeof(createBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);

        std::string name(createBuf);
        while (!name.empty()
               && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();

        std::string problem;
        if (name.empty())
            problem = "Give it a name.";
        else if (name.find_first_of("\\/:*?\"<>|") != std::string::npos)
            problem = "A name cannot contain \\ / : * ? \" < > or |";

        ImGui::TextDisabled("%s", createDir.c_str());
        if (!problem.empty())
            ImGui::TextColored(ImVec4(0.89f, 0.36f, 0.33f, 1.0f), "%s",
                               problem.c_str());
        else
            ImGui::TextDisabled(" ");

        ImGui::Separator();
        ImGui::BeginDisabled(!problem.empty());
        const bool go = ImGui::Button("Create", ImVec2(120, 0))
                        || (entered && problem.empty());
        ImGui::EndDisabled();
        if (go) {
            doCreate(e, name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void doCreate(EditorContext& e, const std::string& name) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path base(createDir);
        switch (createKind) {
        case 0:
            fs::create_directories(base / name, ec);
            if (ec) e.note(EditorContext::NoteKind::Error,
                           "cannot create " + (base / name).string());
            break;
        case 1: {
            const std::string p = (base / (name + ".lime")).string();
            e.newGraph(p, false);
            e.saveAndCompile();
            e.rebuildGraphFunctions();
            break;
        }
        case 2: {
            const std::string p = (base / (name + ".limescene")).string();
            e.newScene(p, name);
            break;
        }
        case 3: {
            const std::string p = (base / (name + ".lua")).string();
            e.addDoc();
            e.newTextFile(p);
            break;
        }
        default: break;
        }
        e.project.scan();
    }

    void drawRenameModal(EditorContext& e) {
        if (openRename) { ImGui::OpenPopup("Rename graph"); openRename = false; }
        if (!ImGui::BeginPopupModal("Rename graph", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::TextDisabled("%s", renameTarget.c_str());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(320);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submit =
            ImGui::InputText("##name", renameBuf, sizeof(renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::TextDisabled(".lime");

        ImGui::Spacing();
        if (submit || ImGui::Button("Rename", ImVec2(120, 0))) {
            e.renameGraph(renameTarget, renameBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

};

}

bool drawAssetPicker(EditorContext& e, const char* label,
                     std::string_view assetType, const std::string& current,
                     std::string& out) {
    static char pickFilter[64] = {};

    const AssetGuid cur = AssetDatabase::parseRef(current);
    const AssetRecord* rec = cur.valid() ? e.assets.find(cur) : nullptr;

    std::string shown;
    ImVec4 tint = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (rec && !rec->missing) {
        shown = fileOf(rec->relPath);
    } else if (cur.valid()) {
        shown = rec ? ("missing: " + fileOf(rec->relPath)) : "missing asset";
        tint = ImVec4(1, 0.5f, 0.3f, 1);
    } else if (current.size() > 2) {
        shown = current;
        tint = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    } else {
        shown = "(none)";
        tint = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }

    bool changed = false;
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    const bool clicked = ImGui::Button(shown.c_str(), ImVec2(-1, 0));
    ImGui::PopStyleColor();

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("LIME_ASSET")) {
            const AssetGuid g = *static_cast<const AssetGuid*>(p->Data);
            out = "\"" + AssetDatabase::makeRef(g) + "\"";
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (clicked) ImGui::OpenPopup("pick");

    if (ImGui::BeginPopup("pick")) {
        ImGui::SetNextItemWidth(240);
        if (ImGui::IsWindowAppearing()) {
            pickFilter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##f", "Search...", pickFilter,
                                 sizeof(pickFilter));
        ImGui::Separator();

        if (cur.valid() || current.size() > 2) {
            if (ImGui::Selectable("(none)")) {
                out = "\"\"";
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }

        int shownCount = 0;
        for (const AssetRecord& r : e.assets.all()) {
            if (r.missing || (!assetType.empty() && r.type != assetType)) continue;
            if (!matches(r, pickFilter)) continue;
            ++shownCount;
            ImGui::PushID(r.guid.str().c_str());
            if (ImGui::Selectable(r.relPath.c_str(), r.guid == cur)) {
                out = "\"" + AssetDatabase::makeRef(r.guid) + "\"";
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        if (!shownCount)
            ImGui::TextDisabled("No %.*s assets in this project.",
                                static_cast<int>(assetType.size()),
                                assetType.data());
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

std::vector<std::unique_ptr<IPanel>> makeAssetPanels() {
    std::vector<std::unique_ptr<IPanel>> v;
    v.push_back(std::make_unique<ContentPanel>());
    return v;
}

}
