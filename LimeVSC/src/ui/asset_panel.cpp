#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>

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
        if (e.project.isEngine()) drawScenes(e);
        drawGraphs(e);
        drawScripts(e);
        if (e.project.isEngine()) drawAssets(e);
        drawGenerated(e);
        drawRenameModal(e);
    }

private:
    std::string renameTarget;
    char        renameBuf[128] = "";
    bool        openRename = false;
    char        filter[64] = {};

    static std::string fileName(const std::string& path) {
        return path.substr(path.find_last_of("/\\") + 1);
    }

    void drawEmptyState(EditorContext& e) {
        if (ImGui::Button("New Project...", ImVec2(-1, 0)))
            e.commands.invoke("project.new", e);
        if (ImGui::Button("Open Project...", ImVec2(-1, 0)))
            e.commands.invoke("project.open", e);
        ImGui::Spacing();

        if (e.settings.recentProjects.empty()) {
            ImGui::TextDisabled("%s", kEmpty);
            return;
        }

        ImGui::SeparatorText("Recent");
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

        ImGui::Spacing();
        if (ImGui::SmallButton("Clear")) {
            e.settings.recentProjects.clear();
            Diagnostics d;
            e.settings.save(d);
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

    void drawScenes(EditorContext& e) {
        if (!ImGui::CollapsingHeader("Scenes", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        if (e.project.sceneFiles.empty()) {
            ImGui::TextDisabled("%s", kEmpty);
            return;
        }
        for (const std::string& f : e.project.sceneFiles) {
            const bool open = (f == e.scenePath);
            ImGui::PushID(f.c_str());
            if (ImGui::Selectable(fileName(f).c_str(), open) && !open) {
                if (e.sceneDirty) e.saveScene();
                e.openScene(f);
            }
            if (ImGui::BeginPopupContextItem("sctx")) {
                if (ImGui::MenuItem("Set as Start Scene")) {
                    if (f != e.scenePath) e.openScene(f);
                    e.commands.invoke("scene.setStart", e);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    void drawGraphs(EditorContext& e) {
        if (!ImGui::CollapsingHeader("Graphs", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        if (e.project.limeFiles.empty()) {
            ImGui::TextDisabled("%s", kEmpty);
            return;
        }
        for (const std::string& f : e.project.limeFiles) {
            const bool hasTab = e.findDoc(f) < e.docs.size();
            const bool active = (f == e.filePath());

            ImGui::PushID(f.c_str());
            if (ImGui::Selectable(fileName(f).c_str(), active))
                e.openDoc(f);
            if (hasTab && !active) {
                ImGui::SameLine();
                ImGui::TextDisabled("open");
            }
            if (ImGui::BeginPopupContextItem("ctx")) {
                if (ImGui::MenuItem("Open")) e.openDoc(f);
                if (ImGui::MenuItem("Rename...")) {
                    renameTarget = f;
                    const std::string name = fileName(f);
                    const std::size_t dot = name.find_last_of('.');
                    std::snprintf(renameBuf, sizeof(renameBuf), "%s",
                                  name.substr(0, dot).c_str());
                    openRename = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
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

    void drawAssets(EditorContext& e) {
        if (!ImGui::CollapsingHeader("Assets", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        if (ImGui::SmallButton("Rescan")) e.queueAssetScan();

        int missing = 0;
        for (const AssetRecord& r : e.assets.all())
            if (r.missing) ++missing;
        if (missing) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%d missing", missing);
        }

        if (e.assets.size() == 0) {
            ImGui::TextDisabled("%s", kEmpty);
            return;
        }
        if (e.assets.size() > 12) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##filter", "Filter...", filter,
                                     sizeof(filter));
        } else {
            filter[0] = '\0';
        }

        std::string lastFolder = "\x01";
        for (const AssetRecord& r : e.assets.all()) {
            if (!matches(r, filter)) continue;
            const std::string folder = folderOf(r.relPath);
            if (folder != lastFolder) {
                lastFolder = folder;
                ImGui::SeparatorText(folder.empty() ? "content/" : folder.c_str());
            }
            drawAssetRow(r);
        }
    }

    static void drawAssetRow(const AssetRecord& r) {
        ImGui::PushID(r.guid.str().c_str());

        if (r.missing)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.3f, 1));
        ImGui::Selectable(fileOf(r.relPath).c_str());
        if (r.missing) ImGui::PopStyleColor();

        if (ImGui::BeginDragDropSource()) {
            const AssetGuid g = r.guid;
            ImGui::SetDragDropPayload("LIME_ASSET", &g, sizeof(AssetGuid));
            ImGui::TextUnformatted(fileOf(r.relPath).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(r.relPath.c_str());
            ImGui::TextDisabled("%s | %llu bytes", r.type.c_str(),
                                static_cast<unsigned long long>(r.size));
            if (r.missing)
                ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1),
                                   "File is gone. Restore it, or clear the\n"
                                   "references that still point at it.");
            else
                ImGui::TextDisabled("id %s", r.guid.str().c_str());
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", r.type.c_str());
        ImGui::PopID();
    }

    void drawScripts(EditorContext& e) {
        if (!ImGui::CollapsingHeader("Scripts", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        bool any = false;
        for (const std::string& f : e.project.luaFiles) {
            if (e.isGeneratedLua(f)) continue;
            any = true;
            const bool active = (f == e.filePath());
            ImGui::PushID(f.c_str());
            if (ImGui::Selectable(fileName(f).c_str(), active)) e.openDoc(f);
            if (!active && e.findDoc(f) < e.docs.size()) {
                ImGui::SameLine();
                ImGui::TextDisabled("open");
            }
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("%s", kEmpty);
    }

    void drawGenerated(EditorContext& e) {
        if (!ImGui::CollapsingHeader("Generated")) return;
        bool any = false;
        for (const std::string& f : e.project.luaFiles) {
            if (!e.isGeneratedLua(f)) continue;
            any = true;
            ImGui::TextDisabled("%s", fileName(f).c_str());
        }
        if (!any) ImGui::TextDisabled("%s", kEmpty);
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
