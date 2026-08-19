#include "ui/panels.h"

#include <imgui.h>

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

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
        drawDeleteModal(e);
    }

private:
    std::string renameTarget;
    char        renameBuf[128] = "";
    bool        openRename = false;
    char        filter[64] = {};
    std::string deleteTarget;
    bool        keepContents = false;
    std::vector<std::string> selected;
    std::vector<std::string> rowOrder;
    std::vector<ImVec2>      rowMin, rowMax;
    std::string anchorRow;
    std::vector<std::string> bandBase;
    ImVec2 bandStart{};
    bool   banding = false;
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
        ImGui::TextDisabled("%s", shortProjectPath(e.project.root).c_str());

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

    bool isSelected(const std::string& p) const {
        return std::find(selected.begin(), selected.end(), p) != selected.end();
    }

    void clickRow(const std::string& path) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && !anchorRow.empty()) {
            const auto a = std::find(rowOrder.begin(), rowOrder.end(), anchorRow);
            const auto b = std::find(rowOrder.begin(), rowOrder.end(), path);
            if (a != rowOrder.end() && b != rowOrder.end()) {
                auto lo = a, hi = b;
                if (lo > hi) std::swap(lo, hi);
                if (!io.KeyCtrl) selected.clear();
                for (auto it = lo; it <= hi; ++it)
                    if (!isSelected(*it)) selected.push_back(*it);
                return;
            }
        }
        if (io.KeyCtrl) {
            const auto at = std::find(selected.begin(), selected.end(), path);
            if (at != selected.end()) selected.erase(at);
            else selected.push_back(path);
            anchorRow = path;
            return;
        }
        selected.assign(1, path);
        anchorRow = path;
    }

    void dragSelect() {
        const ImGuiIO& io = ImGui::GetIO();
        const bool inside = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        if (!banding && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered()) {
            banding = true;
            bandStart = ImGui::GetMousePos();
            bandBase = io.KeyCtrl ? selected : std::vector<std::string>{};
            selected = bandBase;
            anchorRow.clear();
        }
        if (!banding) return;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            banding = false;
            return;
        }

        const ImVec2 now = ImGui::GetMousePos();
        const ImVec2 lo((std::min)(bandStart.x, now.x),
                        (std::min)(bandStart.y, now.y));
        const ImVec2 hi((std::max)(bandStart.x, now.x),
                        (std::max)(bandStart.y, now.y));

        selected = bandBase;
        for (std::size_t i = 0; i < rowOrder.size(); ++i) {
            if (rowMax[i].y < lo.y || rowMin[i].y > hi.y) continue;
            if (rowMax[i].x < lo.x || rowMin[i].x > hi.x) continue;
            if (!isSelected(rowOrder[i])) selected.push_back(rowOrder[i]);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(lo, hi, IM_COL32(140, 209, 115, 40));
        dl->AddRect(lo, hi, IM_COL32(140, 209, 115, 160));
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
        rowOrder.clear();
        rowMin.clear();
        rowMax.clear();
        drawDir(e, root);
        dragSelect();
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
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                                       | ImGuiTreeNodeFlags_OpenOnArrow
                                       | ImGuiTreeNodeFlags_OpenOnDoubleClick;
            if (isSelected(d)) flags |= ImGuiTreeNodeFlags_Selected;

            const ImVec2 rowTop = ImGui::GetCursorScreenPos();
            const bool open = ImGui::TreeNodeEx(fileName(d).c_str(), flags);
            rowOrder.push_back(d);
            rowMin.push_back(rowTop);
            rowMax.push_back(ImGui::GetItemRectMax());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                clickRow(d);

            if (ImGui::BeginPopupContextItem("dctx")) {
                drawCreateMenu(e, d);
                ImGui::Separator();
                const bool keep = holdsNeeded(e, d);
                const bool pickedDir = isSelected(d);
                const int manyDirs =
                    pickedDir ? static_cast<int>(selected.size()) : 1;
                char dlabel[48];
                if (manyDirs > 1)
                    std::snprintf(dlabel, sizeof(dlabel), "Delete %d items",
                                  manyDirs);
                else
                    std::snprintf(dlabel, sizeof(dlabel), "Delete Folder");
                if (ImGui::MenuItem(dlabel, nullptr, false, !keep)) {
                    if (!pickedDir) selected.assign(1, d);
                    deleteTarget = d;
                }
                if (keep
                    && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Holds a file the project needs.");
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
        const bool isPrefabFile = ext == ".limeprefab";
        const bool isScene = ext == ".limescene" || isPrefabFile;
        const bool isGraph = ext == ".lime";
        const bool isLua = ext == ".lua";
        const bool generated = isLua && e.isGeneratedLua(path);

        bool active = false;
        if (isScene) active = (path == e.scenePath);
        else if (isGraph || isLua) active = (path == e.filePath());

        ImGui::PushID(path.c_str());
        if (generated) ImGui::PushStyleColor(ImGuiCol_Text,
                                             ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        rowOrder.push_back(path);
        const bool picked = isSelected(path);
        const ImVec2 rowTop = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(fileName(path).c_str(), active || picked,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            clickRow(path);
            const ImGuiIO& io = ImGui::GetIO();
            if (!io.KeyCtrl && !io.KeyShift) openFile(e, path);
        }
        if (generated) ImGui::PopStyleColor();
        rowMin.push_back(rowTop);
        rowMax.push_back(ImGui::GetItemRectMax());

        if (isPrefabFile
            && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            ImGui::SetDragDropPayload("LIME_PREFAB", path.c_str(),
                                      path.size() + 1);
            ImGui::TextUnformatted(fileName(path).c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginPopupContextItem("fctx")) {
            if (isScene || isGraph || EditorContext::isTextPath(path))
                if (ImGui::MenuItem("Open")) openFile(e, path);
            if (isPrefabFile)
                if (ImGui::MenuItem("Add to Scene", nullptr, false,
                                    !e.scenePath.empty()))
                    e.instantiatePrefab(path, EntityId{});
            if (isScene && !isPrefabFile
                && ImGui::MenuItem("Set as Start Scene")) {
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
            const bool locked = needed(e, path);
            const int many = picked ? static_cast<int>(selected.size()) : 1;
            char label[48];
            if (many > 1) std::snprintf(label, sizeof(label), "Delete %d items", many);
            else std::snprintf(label, sizeof(label), "Delete");
            if (ImGui::MenuItem(label, nullptr, false, !generated && !locked)) {
                if (!picked) selected.assign(1, path);
                deleteTarget = path;
            }
            if ((generated || locked)
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    generated
                        ? "Compiler output. Delete the graph it came from instead."
                        : "The project will not build without this.");
            ImGui::Separator();
            drawCreateMenu(e, fs::path(path).parent_path().string());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    static void openFile(EditorContext& e, const std::string& path) {
        const std::string ext = std::filesystem::path(path).extension().string();
        if (ext == ".limescene" || ext == ".limeprefab") {
            if (path == e.scenePath) return;
            if (e.sceneDirty) e.saveScene();
            e.openScene(path);
            return;
        }
        if (ext == ".lime" || EditorContext::isTextPath(path)) e.openDoc(path);
    }

    static void importInto(EditorContext& e, const std::string& dir) {
        char buf[8192] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = sizeof(buf);
        ofn.lpstrTitle = "Import into this folder";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT
                    | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&ofn)) return;

        namespace fs = std::filesystem;
        std::vector<fs::path> picked;
        const std::string head(buf);
        const char* p = buf + head.size() + 1;
        if (*p == 0) {
            picked.push_back(fs::path(head));
        } else {
            while (*p) {
                picked.push_back(fs::path(head) / p);
                p += std::strlen(p) + 1;
            }
        }

        int copied = 0;
        for (const fs::path& src : picked) {
            std::error_code ec;
            const fs::path dst = fs::path(dir) / src.filename();
            if (fs::exists(dst, ec)) {
                e.note(EditorContext::NoteKind::Warning,
                       src.filename().string() + " is already here");
                continue;
            }
            fs::copy_file(src, dst, ec);
            if (ec)
                e.note(EditorContext::NoteKind::Error,
                       "could not import " + src.filename().string());
            else
                ++copied;
        }
        if (copied > 0) {
            e.note(EditorContext::NoteKind::Action,
                   "Imported " + std::to_string(copied)
                       + (copied == 1 ? " file" : " files"));
            e.rescanAssets();
            e.project.scan();
        }
    }

    void drawCreateMenu(EditorContext& e, const std::string& dir) {
        if (ImGui::MenuItem("New Folder...")) beginCreate(dir, 0, "New Folder");
        ImGui::Separator();
        if (ImGui::MenuItem("New Graph...")) beginCreate(dir, 1, "graph");
        if (e.project.isEngine())
            if (ImGui::MenuItem("New Scene...")) beginCreate(dir, 2, "scene");
        if (ImGui::MenuItem("New Script...")) beginCreate(dir, 3, "script");
        if (e.project.isEngine())
            if (ImGui::MenuItem("New Prefab...")) beginCreate(dir, 4, "Prefab");
        ImGui::Separator();
        if (ImGui::MenuItem("Import Asset...")) importInto(e, dir);
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

    static bool underPath(const std::string& root, const std::string& other) {
        namespace fs = std::filesystem;
        if (other.empty()) return false;
        const std::string r = fs::path(root).lexically_normal().generic_string();
        const std::string o = fs::path(other).lexically_normal().generic_string();
        return o == r
               || (o.size() > r.size() && o.compare(0, r.size(), r) == 0
                   && o[r.size()] == '/');
    }

    static bool anyDirty(const EditorContext& e, const std::string& root) {
        for (const auto& d : e.docs)
            if (d->dirty && underPath(root, d->filePath)) return true;
        return false;
    }

    std::vector<std::string> targets() const {
        if (selected.size() > 1
            && std::find(selected.begin(), selected.end(), deleteTarget)
                   != selected.end())
            return selected;
        return {deleteTarget};
    }

    static void rehomeContents(EditorContext& e, const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path up = fs::path(dir).parent_path();
        if (up.empty()) return;

        std::vector<fs::path> items;
        for (const fs::directory_entry& en : fs::directory_iterator(dir, ec))
            items.push_back(en.path());

        int moved = 0;
        for (const fs::path& src : items) {
            fs::path dst = up / src.filename();
            for (int n = 2; fs::exists(dst, ec) && n < 1000; ++n)
                dst = up
                      / (src.stem().string() + " (" + std::to_string(n) + ")"
                         + src.extension().string());
            std::error_code mec;
            fs::rename(src, dst, mec);
            if (mec) {
                e.note(EditorContext::NoteKind::Error,
                       "could not move " + src.filename().string());
                continue;
            }
            if (src != dst) e.forgetDeleted(src.string());
            ++moved;
        }
        if (moved > 0)
            e.note(EditorContext::NoteKind::Action,
                   "Moved " + std::to_string(moved) + " up to "
                       + up.filename().string());
    }

    void drawDeleteModal(EditorContext& e) {
        namespace fs = std::filesystem;
        if (!deleteTarget.empty() && !ImGui::IsPopupOpen("Delete"))
            ImGui::OpenPopup("Delete");
        ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0),
                                            ImVec2(420, FLT_MAX));
        if (!ImGui::BeginPopupModal("Delete", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        std::error_code ec;
        const bool isDir = fs::is_directory(deleteTarget, ec);
        const std::vector<std::string> batch = targets();
        if (batch.size() > 1) {
            ImGui::TextWrapped("Delete these %zu items?", batch.size());
            for (const std::string& t : batch)
                ImGui::TextDisabled("%s", fileName(t).c_str());
        } else {
            ImGui::TextWrapped("Delete %s?", fileName(deleteTarget).c_str());
            ImGui::TextDisabled("%s", deleteTarget.c_str());
        }

        if (isDir) {
            int files = 0, dirs = 0;
            for (auto it = fs::recursive_directory_iterator(deleteTarget, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_directory(ec)) ++dirs;
                else ++files;
            }
            ImGui::Spacing();
            if (files + dirs > 0) {
                ImGui::Checkbox("Move what is inside up one folder",
                                &keepContents);
                if (!keepContents)
                    ImGui::TextWrapped(
                        "%d file%s and %d folder%s inside it go too.", files,
                        files == 1 ? "" : "s", dirs, dirs == 1 ? "" : "s");
            }
        }

        const bool openScene = !e.scenePath.empty()
                               && underPath(deleteTarget, e.scenePath);
        int openDocs = 0;
        for (const auto& d : e.docs)
            if (underPath(deleteTarget, d->filePath)) ++openDocs;
        const bool unsaved = (openScene && e.sceneDirty) || anyDirty(e, deleteTarget);

        if (openScene || openDocs > 0) {
            ImGui::Spacing();
            ImGui::TextWrapped("Item to delete is currently open");
        }
        if (unsaved)
            ImGui::TextColored(ImVec4(0.89f, 0.66f, 0.29f, 1.0f),
                               "There are unsaved changes. They will be lost.");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.89f, 0.36f, 0.33f, 1.0f),
                           "Deletion is permanent");

        ImGui::Separator();
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            std::vector<std::string> batch = targets();
            int done = 0;
            for (const std::string& t : batch) {
                std::error_code dec;
                if (fs::is_directory(t, dec)) {
                    if (keepContents) rehomeContents(e, t);
                    fs::remove_all(t, dec);
                } else {
                    fs::remove(t, dec);
                    if (endsWithLime(t)) {
                        std::error_code gec;
                        const std::string gen = generatedLuaPath(t);
                        fs::remove(gen, gec);
                        fs::remove(gen + ".map", gec);
                    }
                }
                if (dec)
                    e.note(EditorContext::NoteKind::Error,
                           "could not delete " + fileName(t));
                else {
                    e.forgetDeleted(t);
                    ++done;
                }
            }
            if (done > 0)
                e.note(EditorContext::NoteKind::Action,
                       done == 1 ? "Deleted " + fileName(batch.front())
                                 : "Deleted " + std::to_string(done) + " items");
            e.project.scan();
            e.rescanAssets();
            selected.clear();
            deleteTarget.clear();
            keepContents = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            deleteTarget.clear();
            keepContents = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    static bool needed(const EditorContext& e, const std::string& path) {
        namespace fs = std::filesystem;
        const std::string name = fs::path(path).filename().string();
        if (name == "main.lime" || name == "main.lua") return true;
        if (e.project.startScene.empty()) return false;
        const fs::path start =
            (fs::path(e.project.root) / e.project.startScene).lexically_normal();
        return fs::path(path).lexically_normal() == start;
    }

    static bool holdsNeeded(const EditorContext& e, const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && needed(e, it->path().string()))
                return true;
        }
        return false;
    }

    static bool endsWithLime(const std::string& p) {
        return p.size() > 5 && p.compare(p.size() - 5, 5, ".lime") == 0;
    }

    void drawCreateModal(EditorContext& e) {
        static const char* kTitles[] = {"New Folder", "New Graph", "New Scene",
                                        "New Script", "New Prefab"};
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
        case 4:
            e.newPrefab((base / (name + ".limeprefab")).string(), name);
            break;
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

std::string shortProjectPath(const std::string& root) {
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

}
