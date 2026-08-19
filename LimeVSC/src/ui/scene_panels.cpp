#include "ui/panels.h"

#include "api/graphfn_provider.h"

#include <filesystem>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lime {
namespace {

bool parseNumber(const std::string& s, float& out) {
    try {
        std::size_t used = 0;
        const float v = std::stof(s, &used);
        while (used < s.size() && std::isspace(static_cast<unsigned char>(s[used])))
            ++used;
        if (used != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseVec(const std::string& s, int n, float* out) {
    char head[16];
    std::snprintf(head, sizeof(head), "Vec%d.new(", n);
    const std::size_t hl = std::strlen(head);
    if (s.size() <= hl || s.compare(0, hl, head) != 0) return false;
    if (s.back() != ')') return false;

    const std::string args = s.substr(hl, s.size() - hl - 1);
    std::size_t at = 0;
    for (int i = 0; i < n; ++i) {
        const std::size_t comma = args.find(',', at);
        const std::string tok =
            args.substr(at, comma == std::string::npos ? std::string::npos
                                                       : comma - at);
        std::string trimmed;
        for (char c : tok)
            if (!std::isspace(static_cast<unsigned char>(c))) trimmed += c;
        if (!parseNumber(trimmed, out[i])) return false;
        if (comma == std::string::npos) return i == n - 1;
        at = comma + 1;
    }
    return false;
}

std::string fmtNumber(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

std::string fmtVec(int n, const float* v) {
    std::string s = "Vec" + std::to_string(n) + ".new(";
    for (int i = 0; i < n; ++i) {
        if (i) s += ", ";
        s += fmtNumber(v[i]);
    }
    return s + ")";
}

bool parseLuaString(const std::string& s, std::string& out) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    out.clear();
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else if (s[i] == '"') {
            return false;
        } else {
            out += s[i];
        }
    }
    return true;
}

std::string fmtLuaString(const std::string& text) {
    std::string s = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') s += '\\';
        if (c == '\n') { s += "\\n"; continue; }
        s += c;
    }
    return s + "\"";
}

bool drawBehaviourPicker(EditorContext& e, const char* label,
                         const std::string& current, std::string& out) {
    namespace fs = std::filesystem;
    std::string shown;
    parseLuaString(current, shown);

    const fs::path content(e.project.contentDir());
    const auto rel = [&](const std::string& p) {
        std::error_code ec;
        const fs::path r = fs::path(p).lexically_relative(content);
        return r.empty() ? p : r.generic_string();
    };

    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##behaviour", shown.empty() ? "(none)" : shown.c_str())) {
        if (ImGui::Selectable("(none)", shown.empty())) {
            out = fmtLuaString("");
            changed = true;
        }
        for (const std::vector<std::string>* list :
             {&e.project.limeFiles, &e.project.luaFiles})
            for (const std::string& f : *list) {
                if (list == &e.project.luaFiles && e.isGeneratedLua(f)) continue;
                const std::string r = rel(f);
                if (ImGui::Selectable(r.c_str(), r == shown)) {
                    out = fmtLuaString(r);
                    changed = true;
                }
            }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

std::string_view assetKind(const std::string& typeName) {
    constexpr std::string_view kPrefix = "Asset:";
    if (typeName.size() > kPrefix.size() &&
        typeName.compare(0, kPrefix.size(), kPrefix) == 0)
        return std::string_view(typeName).substr(kPrefix.size());
    return {};
}

void helpMarker(const std::string& doc) {
    if (doc.empty()) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(doc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

class HierarchyPanel final : public IPanel {
public:
    std::string_view id() const override { return "hierarchy"; }
    std::string_view title() const override { return "Hierarchy"; }
    bool availableIn(bool engineMode) const override { return engineMode; }

    void draw(EditorContext& e) override {
        if (e.scenePath.empty()) {
            ImGui::TextDisabled("No scene open.");
            ImGui::TextWrapped("Open a .limescene from the Project panel, or use "
                               "File > New Scene.");
            return;
        }

        const float addW = ImGui::CalcTextSize("Add Entity").x
                           + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float lineW = ImGui::GetContentRegionAvail().x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(e.scene.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s | %zu entities", e.sceneDirty ? "*" : "",
                            e.scene.size());
        ImGui::SameLine(lineW - addW);
        if (ImGui::Button("Add Entity")) e.addEntity("Entity", {});
        ImGui::Separator();

        for (EntityId r : e.scene.childrenOf({})) drawEntity(e, r);

        ImGui::Dummy(ImVec2(-1, 24));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("LIME_ENTITY"))
                e.reparentEntity(*static_cast<const EntityId*>(p->Data), EntityId{});
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextWindow("hctx",
                                           ImGuiPopupFlags_MouseButtonRight
                                               | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("Add Entity")) e.addEntity("Entity", {});
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !e.clipEntities.empty()))
                e.pasteEntity(EntityId{});
            ImGui::EndPopup();
        }

        if (renameTarget.valid()) drawRenameModal(e);
    }

private:
    EntityId renameTarget{};
    char     renameBuf[128] = {};
    bool     openRename = false;

    void drawEntity(EditorContext& e, EntityId id) {
        const Entity* ent = e.scene.entity(id);
        if (!ent) return;

        const std::vector<EntityId> kids = e.scene.childrenOf(id);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (e.selectedEntity == id) flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::PushID(static_cast<int>(id.v));
        const bool open = ImGui::TreeNodeEx("##e", flags, "%s",
                                            ent->name.empty() ? "(unnamed)"
                                                              : ent->name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            e.selectedEntity = id;
            e.inspecting = EditorContext::Inspecting::Entity;

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            ImGui::SetDragDropPayload("LIME_ENTITY", &id, sizeof(EntityId));
            ImGui::TextUnformatted(ent->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("LIME_ENTITY"))
                e.reparentEntity(*static_cast<const EntityId*>(p->Data), id);
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem("ctx")) {
            e.selectedEntity = id;
            e.inspecting = EditorContext::Inspecting::Entity;
            if (ImGui::MenuItem("Add Child")) e.addEntity("Entity", id);
            ImGui::Separator();
            if (ImGui::MenuItem("Unparent", nullptr, false, ent->parent.valid()))
                e.reparentEntity(id, EntityId{});
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C")) e.copyEntity(id);
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !e.clipEntities.empty()))
                e.pasteEntity(id);
            if (ImGui::MenuItem("Paste as Sibling", nullptr, false,
                                !e.clipEntities.empty()))
                e.pasteEntity(ent->parent);
            if (ImGui::MenuItem("Paste Component", nullptr, false,
                                e.hasClipComponent))
                e.pasteComponent(id);
            if (ImGui::MenuItem("Rename")) {
                renameTarget = id;
                std::snprintf(renameBuf, sizeof(renameBuf), "%s", ent->name.c_str());
                openRename = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) e.deleteEntity(id);
            ImGui::EndPopup();
        }

        if (open) {
            for (EntityId k : kids) drawEntity(e, k);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void drawRenameModal(EditorContext& e) {
        if (openRename) {
            ImGui::OpenPopup("Rename Entity");
            openRename = false;
        }
        if (!ImGui::BeginPopupModal("Rename Entity", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::SetNextItemWidth(280);
        const bool submit = ImGui::InputText("##name", renameBuf, sizeof(renameBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        if (submit || ImGui::Button("Rename")) {
            e.renameEntity(renameTarget, renameBuf);
            renameTarget = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            renameTarget = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
};

class EntityInspector {
public:
    void draw(EditorContext& e) {
        Entity* ent = e.scene.entity(e.selectedEntity);
        if (!ent) {
            ImGui::TextDisabled("No entity selected.");
            return;
        }

        char name[128];
        std::snprintf(name, sizeof(name), "%s", ent->name.c_str());
        ImGui::TextDisabled("Name");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", name, sizeof(name)))
            e.renameEntity(ent->id, name);

        if (const Entity* p = e.scene.entity(ent->parent))
            ImGui::TextDisabled("Child of %s", p->name.c_str());
        ImGui::Separator();

        std::vector<std::string> types;
        types.reserve(ent->components.size());
        for (const Component& c : ent->components) types.push_back(c.type);

        for (const std::string& type : types) drawComponent(e, ent->id, type);

        ImGui::Separator();
        if (ImGui::Button("Add Component", ImVec2(-1, 0)))
            ImGui::OpenPopup("addcomp");
        drawAddPopup(e, ent->id);
    }

private:
    char filter[64] = {};

    void drawComponent(EditorContext& e, EntityId id, const std::string& type) {
        Entity* ent = e.scene.entity(id);
        if (!ent) return;
        Component* c = ent->component(type);
        if (!c) return;

        const ComponentDesc* d = e.components.find(type);

        ImGui::PushID(type.c_str());
        const bool open = ImGui::CollapsingHeader(
            d ? std::string(d->label()).c_str() : type.c_str(),
            ImGuiTreeNodeFlags_DefaultOpen);

        if (d && !d->doc.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextUnformatted(d->doc.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        if (ImGui::BeginPopupContextItem("compctx")) {
            if (ImGui::MenuItem("Copy Component")) e.copyComponent(id, type);
            if (ImGui::MenuItem("Paste Component", nullptr, false,
                                e.hasClipComponent))
                e.pasteComponent(id);
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                ImGui::EndPopup();
                ImGui::PopID();
                e.removeComponent(id, type);
                return;
            }
            ImGui::EndPopup();
        }

        if (open) {
            if (!d) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                                   "Unknown component - no provider defines it.");
                for (const auto& [k, v] : c->values)
                    ImGui::TextDisabled("%s = %s", k.c_str(), v.c_str());
            } else {
                for (const PropDesc& p : d->props) drawProp(e, id, type, *d, p);
                if (type == "Behaviour") drawExposedProps(e, id, *c);
                if (ImGui::SmallButton("Remove")) {
                    ImGui::PopID();
                    e.removeComponent(id, type);
                    return;
                }
            }
        }
        ImGui::PopID();
    }

    void drawProp(EditorContext& e, EntityId id, const std::string& type,
                  const ComponentDesc& d, const PropDesc& p) {
        Entity* ent = e.scene.entity(id);
        Component* c = ent ? ent->component(type) : nullptr;
        if (!c) return;

        const std::string cur = e.propValue(*c, &d, p.name);
        const bool overridden = c->value(p.name) != nullptr;

        ImGui::PushID(p.name.c_str());
        if (!overridden)
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(p.name.c_str());
        if (!overridden) ImGui::PopStyleColor();
        helpMarker(p.doc);

        if (overridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("reset")) {
                ImGui::PopID();
                e.setProp(id, type, p.name, "");
                return;
            }
        }

        std::string next = cur;
        bool changed = false;
        ImGui::SetNextItemWidth(-1);

        float v3[3], v2[2];
        float num = 0;
        std::string str;

        if (p.typeName == "boolean" && (cur == "true" || cur == "false")) {
            bool b = cur == "true";
            if (ImGui::Checkbox("##v", &b)) {
                next = b ? "true" : "false";
                changed = true;
            }
        } else if (p.typeName == "Vec3" && parseVec(cur, 3, v3)) {
            if (ImGui::DragFloat3("##v", v3, 0.05f)) {
                next = fmtVec(3, v3);
                changed = true;
            }
        } else if (p.typeName == "Vec2" && parseVec(cur, 2, v2)) {
            if (ImGui::DragFloat2("##v", v2, 0.05f)) {
                next = fmtVec(2, v2);
                changed = true;
            }
        } else if ((p.typeName == "number" || p.typeName == "integer")
                   && parseNumber(cur, num)) {
            if (ImGui::DragFloat("##v", &num, 0.1f)) {
                next = fmtNumber(num);
                changed = true;
            }
        } else if (const std::string_view kind = assetKind(p.typeName);
                   kind == "Graph" || kind == "Script") {
            changed = drawBehaviourPicker(e, p.name.c_str(), cur, next);
        } else if (!kind.empty()) {
            changed = drawAssetPicker(e, p.name.c_str(), kind, cur, next);
        } else if (p.typeName == "string" && parseLuaString(cur, str)) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", str.c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf))) {
                next = fmtLuaString(buf);
                changed = true;
            }
        } else {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                next = buf;
                changed = true;
            }
        }

        ImGui::PopID();
        if (changed && next != cur) e.setProp(id, type, p.name, next);
    }

    void drawExposedProps(EditorContext& e, EntityId id, Component& c) {
        const std::string* ref = c.value("graph");
        std::string path = ref ? *ref : std::string();
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        if (path.empty()) {
            ImGui::TextDisabled("Pick a graph to see its properties.");
            return;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path full = fs::path(e.project.contentDir()) / path;
        if (!fs::exists(full, ec)) full = fs::path(e.project.root) / path;

        std::string module;
        std::vector<FnDecl> fns;
        std::vector<PropDecl> props;
        if (!fs::exists(full, ec)
            || !GraphFnProvider::readSignatures(full.string(), module, fns, props)) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Graph not found.");
            return;
        }
        if (props.empty()) {
            ImGui::TextDisabled("This graph exposes no properties.");
            ImGui::TextDisabled("Add !prop declarations to expose one.");
            return;
        }

        ImGui::SeparatorText("Exposed");
        for (const PropDecl& pd : props) {
            PropDesc p;
            p.name = pd.name;
            p.typeName = pd.type;
            p.defaultValue = pd.defaultValue;
            p.doc = "Exposed by " + module + ".";
            ComponentDesc synthetic;
            synthetic.id = "Behaviour";
            synthetic.props.push_back(p);
            drawProp(e, id, "Behaviour", synthetic, synthetic.props.front());
        }
    }

    void drawAddPopup(EditorContext& e, EntityId id) {
        if (!ImGui::BeginPopup("addcomp")) return;

        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) {
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##filter", "Search components...", filter,
                                 sizeof(filter));
        ImGui::Separator();

        const Entity* ent = e.scene.entity(id);
        std::string lastCat;
        bool any = false;

        for (const ComponentDesc* d : e.components.browseOrder()) {
            const std::string label(d->label());
            if (filter[0] && label.find(filter) == std::string::npos
                && d->id.find(filter) == std::string::npos)
                continue;

            if (d->category != lastCat) {
                if (any) ImGui::Separator();
                ImGui::TextDisabled("%s", d->category.empty() ? "Other"
                                                              : d->category.c_str());
                lastCat = d->category;
            }
            any = true;

            const bool taken = d->unique && ent && ent->component(d->id);
            ImGui::BeginDisabled(taken);
            ImGui::PushID(d->id.c_str());
            if (ImGui::Selectable(label.c_str())) {
                e.addComponent(id, *d);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered() && !d->doc.empty()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                ImGui::TextUnformatted(d->doc.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        if (!any) ImGui::TextDisabled("No components match.");
        ImGui::EndPopup();
    }
};

EntityInspector g_entityInspector;

}

void drawEntityInspector(EditorContext& ed) { g_entityInspector.draw(ed); }

std::vector<std::unique_ptr<IPanel>> makeScenePanels() {
    std::vector<std::unique_ptr<IPanel>> v;
    v.push_back(std::make_unique<HierarchyPanel>());
    return v;
}

}
