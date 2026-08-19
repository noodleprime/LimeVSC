#include "ui/panels.h"
#include "render/renderer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace lime {
namespace {

Renderer g_renderer;
bool     g_rendererTried = false;

enum class GizmoMode { Translate, Rotate, Scale };

struct DragState {
    int      axis = -1;
    GizmoMode mode = GizmoMode::Translate;
    EntityId entity{};
    V3       startValue{};
    ImVec2   startMouse{};
    ImVec2   screenAxis{};
    float    screenScale = 1.0f;

    std::string startProp;
    std::string startLiteral;
    bool        startSet = false;
};

bool parseVec3Literal(const std::string& s, V3& out) {
    constexpr const char* kHead = "Vec3.new(";
    const std::size_t hl = 9;
    if (s.size() <= hl || s.compare(0, hl, kHead) != 0) return false;
    if (s.back() != ')') return false;

    const std::string args = s.substr(hl, s.size() - hl - 1);
    float v[3] = {0, 0, 0};
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t comma = args.find(',', at);
        const std::string tok =
            args.substr(at, comma == std::string::npos ? std::string::npos
                                                       : comma - at);
        try {
            std::size_t used = 0;
            v[i] = std::stof(tok, &used);
            while (used < tok.size()
                   && std::isspace(static_cast<unsigned char>(tok[used])))
                ++used;
            if (used != tok.size()) return false;
        } catch (...) {
            return false;
        }
        if (comma == std::string::npos) {
            if (i != 2) return false;
            break;
        }
        at = comma + 1;
    }
    out = {v[0], v[1], v[2]};
    return true;
}

std::string vec3Literal(V3 v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Vec3.new(%g, %g, %g)",
                  static_cast<double>(v.x), static_cast<double>(v.y),
                  static_cast<double>(v.z));
    return buf;
}

V3 transformProp(EditorContext& e, const Entity& ent, const char* prop, V3 fallback) {
    const Component* t = ent.component("Transform");
    if (!t) return fallback;
    const ComponentDesc* d = e.components.find("Transform");
    V3 v = fallback;
    parseVec3Literal(e.propValue(*t, d, prop), v);
    return v;
}

M4 worldMatrix(EditorContext& e, EntityId id, int depth = 0) {
    const Entity* ent = e.scene.entity(id);
    if (!ent || depth > 32) return M4::identity();
    const M4 local = M4::scale(transformProp(e, *ent, "scale", {1, 1, 1}))
                     * M4::rotationEuler(transformProp(e, *ent, "rotation", {}))
                     * M4::translation(transformProp(e, *ent, "position", {}));
    if (!ent->parent.valid()) return local;
    return local * worldMatrix(e, ent->parent, depth + 1);
}

V3 worldPosition(EditorContext& e, EntityId id) {
    const M4 m = worldMatrix(e, id);
    return {m.m[3][0], m.m[3][1], m.m[3][2]};
}

struct View {
    M4     viewProj;
    ImVec2 origin;
    ImVec2 size;
};

bool project(const View& v, V3 world, ImVec2& out) {
    float w = 0;
    const V3 clip = transformPoint(v.viewProj, world, w);
    if (w < 1e-4f) return false;
    out = ImVec2(v.origin.x + (clip.x / w * 0.5f + 0.5f) * v.size.x,
                 v.origin.y + (0.5f - clip.y / w * 0.5f) * v.size.y);
    return true;
}

float distanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab(b.x - a.x, b.y - a.y);
    const ImVec2 ap(p.x - a.x, p.y - a.y);
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    if (len2 < 1e-6f) return std::sqrt(ap.x * ap.x + ap.y * ap.y);
    float t = (ap.x * ab.x + ap.y * ab.y) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec2 c(a.x + ab.x * t, a.y + ab.y * t);
    return std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y));
}

bool rayHitsBox(V3 origin, V3 dir, V3 lo, V3 hi, float& tOut) {
    float tMin = 0.0f, tMax = 1e9f;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {dir.x, dir.y, dir.z};
    const float l[3] = {lo.x, lo.y, lo.z};
    const float h[3] = {hi.x, hi.y, hi.z};

    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-6f) {
            if (o[i] < l[i] || o[i] > h[i]) return false;
            continue;
        }
        float t1 = (l[i] - o[i]) / d[i];
        float t2 = (h[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return false;
    }
    tOut = tMin;
    return true;
}

constexpr float kHandlePixels = 70.0f;
constexpr float kGrabPixels = 8.0f;

const ImU32 kAxisColor[3] = {IM_COL32(232, 80, 80, 255),
                             IM_COL32(120, 220, 110, 255),
                             IM_COL32(90, 150, 240, 255)};
constexpr ImU32 kHotColor = IM_COL32(255, 220, 90, 255);

class ViewportPanel final : public IPanel {
public:
    std::string_view id() const override { return "viewport"; }
    std::string_view title() const override { return "Viewport"; }
    bool availableIn(bool engineMode) const override { return engineMode; }

    void draw(EditorContext& e) override {
        if (!g_renderer.ready()) {
            ImGui::TextDisabled("The viewport could not start.");
            ImGui::TextWrapped("Shader compilation or device creation failed. "
                               "Everything else in the editor still works.");
            return;
        }
        if (e.scenePath.empty()) {
            ImGui::TextDisabled("No scene open.");
            return;
        }

        drawToolbar(e);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const int w = static_cast<int>(std::max(16.0f, avail.x));
        const int h = static_cast<int>(std::max(16.0f, avail.y));

        renderScene(e, w, h);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(g_renderer.texture()),
                     ImVec2(static_cast<float>(w), static_cast<float>(h)));
        const bool hovered = ImGui::IsItemHovered();

        View view;
        view.viewProj = cam_.view() * cam_.proj(static_cast<float>(w)
                                                / static_cast<float>(h));
        view.origin = origin;
        view.size = ImVec2(static_cast<float>(w), static_cast<float>(h));

        drawGizmo(e, view, hovered);
        handleCamera(hovered);
        handlePick(e, view, hovered);
        drawOverlay(e, view);
    }

private:
    CameraState cam_;
    GizmoMode   mode_ = GizmoMode::Translate;
    DragState   drag_;
    bool        showGrid_ = true;
    bool        capture_ = false;

    void drawToolbar(EditorContext& e) {
        const struct { GizmoMode m; const char* label; const char* tip; } kModes[] = {
            {GizmoMode::Translate, "Move", "Drag an arrow to move the selection (W)"},
            {GizmoMode::Rotate, "Rotate", "Drag a ring to turn the selection (E)"},
            {GizmoMode::Scale, "Scale", "Drag a handle to resize the selection (R)"},
        };
        for (const auto& k : kModes) {
            const bool on = mode_ == k.m;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(k.label)) mode_ = k.m;
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.tip);
            ImGui::SameLine();
        }
        ImGui::Checkbox("Grid", &showGrid_);
        ImGui::SameLine();
        if (ImGui::Button("Frame") && e.selectedEntity.valid()) frame(e);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Point the camera at the selection (F)");
        ImGui::SameLine();
        ImGui::TextDisabled("look: RMB | fly: RMB+WASD | pan: MMB | orbit: Alt+LMB");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Right-drag        look around");
            ImGui::TextUnformatted("Right + WASD      fly (Q/E down/up, Shift faster)");
            ImGui::TextUnformatted("Right + wheel     change fly speed");
            ImGui::TextUnformatted("Left-drag         walk and turn");
            ImGui::TextUnformatted("Middle-drag       pan");
            ImGui::TextUnformatted("Alt + Left-drag   orbit");
            ImGui::TextUnformatted("Alt + Right-drag  dolly");
            ImGui::TextUnformatted("Wheel             dolly");
            ImGui::TextUnformatted("F                 focus the selection");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("| %.1f m/s", static_cast<double>(cam_.flySpeed));

        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) mode_ = GizmoMode::Translate;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) mode_ = GizmoMode::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) mode_ = GizmoMode::Scale;
            if (ImGui::IsKeyPressed(ImGuiKey_F) && e.selectedEntity.valid())
                frame(e);
        }
    }

    void frame(EditorContext& e) {
        const V3 target = worldPosition(e, e.selectedEntity);
        float radius = 1.0f;
        if (const Entity* ent = e.scene.entity(e.selectedEntity))
            radius = std::max(0.5f, length(transformProp(e, *ent, "scale",
                                                         {1, 1, 1})));
        cam_.focusOn(target, std::max(2.5f, radius * 3.5f));
    }

    void handleCamera(bool hovered) {
        if (drag_.axis >= 0) {
            capture_ = false;
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        const bool alt = io.KeyAlt;
        const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);

        const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        const bool lmb = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool mmb = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

        if (hovered && (rmb || mmb || (lmb && alt) || lmb)) capture_ = true;
        if (!rmb && !lmb && !mmb) capture_ = false;
        if (!capture_ && !hovered) return;

        const ImVec2 d = io.MouseDelta;
        constexpr float kLook = 0.005f;
        constexpr float kMaxPitch = 1.5533f;

        if (alt && lmb && !rmb) {
            const V3 pivot = cam_.pivot();
            cam_.yaw += d.x * kLook;
            cam_.pitch = std::clamp(cam_.pitch - d.y * kLook, -kMaxPitch, kMaxPitch);
            cam_.position = pivot - cam_.forward() * cam_.pivotDistance;
        } else if (alt && rmb) {
            dolly(-d.y * cam_.pivotDistance * 0.005f);
        } else if (rmb) {
            cam_.yaw += d.x * kLook;
            cam_.pitch = std::clamp(cam_.pitch - d.y * kLook, -kMaxPitch, kMaxPitch);
            flyKeys(dt);
        } else if (mmb) {
            const float k = std::max(0.05f, cam_.pivotDistance) * 0.0022f;
            cam_.position = cam_.position - cam_.right() * (d.x * k)
                            + cam_.up() * (d.y * k);
        } else if (lmb) {
            cam_.yaw += d.x * kLook;
            const V3 flat = normalize(V3{cam_.forward().x, 0, cam_.forward().z});
            cam_.position = cam_.position + flat * (-d.y * cam_.flySpeed * 0.01f);
        }

        if (!hovered) return;
        if (io.MouseWheel != 0.0f) {
            if (rmb) {
                cam_.flySpeed = std::clamp(
                    cam_.flySpeed * std::pow(1.25f, io.MouseWheel), 0.05f, 5000.0f);
            } else {
                dolly(io.MouseWheel * std::max(0.4f, cam_.pivotDistance * 0.15f));
            }
        }
    }

    void dolly(float amount) {
        const V3 pivot = cam_.pivot();
        cam_.position = cam_.position + cam_.forward() * amount;
        const float d = dot(pivot - cam_.position, cam_.forward());
        cam_.pivotDistance = std::clamp(d, 0.25f, 5000.0f);
    }

    void flyKeys(float dt) {
        if (ImGui::GetIO().WantTextInput) return;
        V3 move{0, 0, 0};
        if (ImGui::IsKeyDown(ImGuiKey_W)) move = move + cam_.forward();
        if (ImGui::IsKeyDown(ImGuiKey_S)) move = move - cam_.forward();
        if (ImGui::IsKeyDown(ImGuiKey_D)) move = move + cam_.right();
        if (ImGui::IsKeyDown(ImGuiKey_A)) move = move - cam_.right();
        if (ImGui::IsKeyDown(ImGuiKey_E)) move = move + V3{0, 1, 0};
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move = move - V3{0, 1, 0};
        if (length(move) < 1e-4f) return;

        float speed = cam_.flySpeed;
        if (ImGui::GetIO().KeyShift) speed *= 3.0f;
        cam_.position = cam_.position + normalize(move) * (speed * dt);
    }

    void renderScene(EditorContext& e, int w, int h) {
        if (!g_renderer.begin(w, h, cam_)) return;

        if (showGrid_) {
            const MeshData* grid = g_renderer.findMesh("#grid");
            if (!grid) grid = g_renderer.cacheMesh("#grid", makeGrid(20, 1.0f));
            if (grid) {
                DrawItem d;
                d.mesh = grid;
                d.lines = true;
                d.color = {0.30f, 0.32f, 0.36f, 1.0f};
                g_renderer.draw(d);
            }
        }

        for (const Entity& ent : e.scene.entities()) {
            DrawItem d;
            d.transform = worldMatrix(e, ent.id);
            d.mesh = meshFor(e, ent, d.color);
            if (!d.mesh) continue;
            g_renderer.draw(d);

            if (ent.id == e.selectedEntity) {
                DrawItem sel = d;
                sel.wireframe = true;
                sel.unlit = true;
                sel.color = {1.0f, 0.72f, 0.20f, 1.0f};
                g_renderer.draw(sel);
            }
        }
        g_renderer.end();
    }

    const MeshData* meshFor(EditorContext& e, const Entity& ent,
                            std::array<float, 4>& color) {
        if (const Component* mr = ent.component("MeshRenderer")) {
            const ComponentDesc* d = e.components.find("MeshRenderer");
            const std::string ref = e.propValue(*mr, d, "mesh");
            const AssetGuid g = AssetDatabase::parseRef(ref);
            const AssetRecord* rec = g.valid() ? e.assets.find(g) : nullptr;
            if (rec && !rec->missing) {
                const std::string key = "asset:" + rec->guid.str();
                if (const MeshData* cached = g_renderer.findMesh(key))
                    return cached;
                const std::string full =
                    e.project.contentDir() + "/" + rec->relPath;
                MeshData md;
                Diagnostics md_diag;
                if (rec->relPath.size() > 4
                    && readObj(full, md, md_diag))
                    if (const MeshData* up = g_renderer.cacheMesh(key, std::move(md)))
                        return up;
                return g_renderer.cacheMesh(key, makeBox(1, 1, 1));
            }
            color = {0.85f, 0.35f, 0.25f, 1.0f};
            return placeholder("#box", makeBox(1, 1, 1));
        }
        if (ent.component("Camera")) {
            color = {0.78f, 0.42f, 0.78f, 1.0f};
            return placeholder("#cam", makeBox(0.7f, 0.5f, 1.1f));
        }
        if (ent.component("Light")) {
            color = {0.95f, 0.85f, 0.35f, 1.0f};
            return placeholder("#light", makeSphere(0.35f, 12));
        }
        if (ent.component("Collider")) {
            color = {0.35f, 0.75f, 0.55f, 1.0f};
            return placeholder("#col", makeBox(1, 1, 1));
        }
        color = {0.55f, 0.57f, 0.62f, 1.0f};
        return placeholder("#empty", makeBox(0.25f, 0.25f, 0.25f));
    }

    static const MeshData* placeholder(const char* key, MeshData make) {
        if (const MeshData* m = g_renderer.findMesh(key)) return m;
        return g_renderer.cacheMesh(key, std::move(make));
    }

    void handlePick(EditorContext& e, const View& v, bool hovered) {
        if (!hovered) return;
        if (drag_.axis >= 0 || capture_) return;
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        if (ImGui::GetIO().KeyAlt) return;

        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        if (std::abs(drag.x) + std::abs(drag.y) > 4.0f) return;

        const ImVec2 m = ImGui::GetIO().MousePos;
        const V3 dir = rayThrough(v, m);

        EntityId best{};
        float bestT = 1e9f;
        for (const Entity& ent : e.scene.entities()) {
            V3 lo, hi;
            if (!worldBounds(e, ent, lo, hi)) continue;
            float t = 0;
            if (!rayHitsBox(cam_.eye(), dir, lo, hi, t)) continue;
            if (t >= bestT) continue;
            bestT = t;
            best = ent.id;
        }

        e.selectedEntity = best;
        e.inspecting = EditorContext::Inspecting::Entity;
    }

    V3 rayThrough(const View& v, ImVec2 px) const {
        const float ndcX = (px.x - v.origin.x) / v.size.x * 2.0f - 1.0f;
        const float ndcY = 1.0f - (px.y - v.origin.y) / v.size.y * 2.0f;
        const float tanHalf = std::tan(cam_.fov * 3.14159265f / 360.0f);
        const float aspect = v.size.x / std::max(1.0f, v.size.y);
        return normalize(cam_.forward()
                         + cam_.right() * (ndcX * tanHalf * aspect)
                         + cam_.up() * (ndcY * tanHalf));
    }

    bool worldBounds(EditorContext& e, const Entity& ent, V3& lo, V3& hi) {
        std::array<float, 4> ignored{};
        const MeshData* mesh = meshFor(e, ent, ignored);
        if (!mesh || mesh->vertices.empty()) return false;

        const M4 m = worldMatrix(e, ent.id);
        const std::array<float, 3>& a = mesh->boundsMin;
        const std::array<float, 3>& b = mesh->boundsMax;
        bool first = true;
        for (int c = 0; c < 8; ++c) {
            const V3 corner{(c & 1) ? b[0] : a[0], (c & 2) ? b[1] : a[1],
                            (c & 4) ? b[2] : a[2]};
            float w = 0;
            const V3 p = transformPoint(m, corner, w);
            if (first) { lo = hi = p; first = false; continue; }
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const float pad = 0.02f;
        lo = lo - V3{pad, pad, pad};
        hi = hi + V3{pad, pad, pad};
        return true;
    }

    void drawGizmo(EditorContext& e, const View& v, bool hovered) {
        const Entity* ent = e.scene.entity(e.selectedEntity);
        if (!ent || !ent->component("Transform")) {
            drag_.axis = -1;
            return;
        }

        const V3 origin = worldPosition(e, e.selectedEntity);
        ImVec2 originPx;
        if (!project(v, origin, originPx)) return;

        const V3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        ImVec2 tip[3];
        bool   visible[3] = {false, false, false};
        float  worldPerPixel[3] = {1, 1, 1};

        for (int i = 0; i < 3; ++i) {
            ImVec2 probe;
            const float eyeDist = std::max(0.05f, length(origin - cam_.eye()));
            const float unit = eyeDist * 0.15f;
            if (!project(v, origin + axes[i] * unit, probe)) continue;
            const float dx = probe.x - originPx.x, dy = probe.y - originPx.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-3f) continue;
            const float k = kHandlePixels / len;
            tip[i] = ImVec2(originPx.x + dx * k, originPx.y + dy * k);
            worldPerPixel[i] = unit * k / kHandlePixels;
            visible[i] = true;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        int hot = -1;
        if (drag_.axis >= 0) {
            hot = drag_.axis;
        } else if (hovered && !capture_
                   && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            float best = kGrabPixels;
            for (int i = 0; i < 3; ++i) {
                if (!visible[i]) continue;
                const float d = mode_ == GizmoMode::Rotate
                                    ? std::abs(distanceToCircle(mouse, originPx,
                                                                kHandlePixels))
                                    : distanceToSegment(mouse, originPx, tip[i]);
                if (d < best) { best = d; hot = i; }
            }
            if (mode_ == GizmoMode::Rotate && hot >= 0)
                hot = nearestRingAxis(mouse, originPx, tip, visible);
        }

        for (int i = 0; i < 3; ++i) {
            if (!visible[i]) continue;
            const ImU32 col = (i == hot) ? kHotColor : kAxisColor[i];
            if (mode_ == GizmoMode::Rotate) {
                drawRing(dl, originPx, tip, i, col);
                continue;
            }
            dl->AddLine(originPx, tip[i], col, 2.5f);
            if (mode_ == GizmoMode::Translate) {
                const float dx = tip[i].x - originPx.x, dy = tip[i].y - originPx.y;
                const float len = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
                const ImVec2 u(dx / len, dy / len), n(-u.y, u.x);
                dl->AddTriangleFilled(
                    ImVec2(tip[i].x + u.x * 9, tip[i].y + u.y * 9),
                    ImVec2(tip[i].x + n.x * 4.5f, tip[i].y + n.y * 4.5f),
                    ImVec2(tip[i].x - n.x * 4.5f, tip[i].y - n.y * 4.5f), col);
            } else {
                dl->AddRectFilled(ImVec2(tip[i].x - 4, tip[i].y - 4),
                                  ImVec2(tip[i].x + 4, tip[i].y + 4), col);
            }
        }
        dl->AddCircleFilled(originPx, 3.0f, IM_COL32(230, 230, 230, 220));

        if (drag_.axis < 0 && hot >= 0 && hovered && !capture_
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag_.axis = hot;
            drag_.mode = mode_;
            drag_.entity = e.selectedEntity;
            drag_.startMouse = mouse;
            drag_.startValue = currentValue(e, *ent);
            drag_.startProp = propNameFor(mode_);
            drag_.startSet = false;
            drag_.startLiteral.clear();
            if (const Component* t = ent->component("Transform"))
                if (const std::string* v = t->value(drag_.startProp)) {
                    drag_.startSet = true;
                    drag_.startLiteral = *v;
                }
            drag_.screenAxis = ImVec2(tip[hot].x - originPx.x,
                                      tip[hot].y - originPx.y);
            drag_.screenScale = worldPerPixel[hot];
        }

        if (drag_.axis >= 0) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                finishDrag(e);
            } else if (drag_.entity == e.selectedEntity) {
                applyDrag(e, mouse);
            } else {
                finishDrag(e);
            }
        }
    }

    static float distanceToCircle(ImVec2 p, ImVec2 c, float r) {
        const float dx = p.x - c.x, dy = p.y - c.y;
        return std::sqrt(dx * dx + dy * dy) - r;
    }

    static int nearestRingAxis(ImVec2 mouse, ImVec2 origin, const ImVec2* tip,
                               const bool* visible) {
        const ImVec2 dir(mouse.x - origin.x, mouse.y - origin.y);
        const float len = std::max(1e-3f,
                                   std::sqrt(dir.x * dir.x + dir.y * dir.y));
        int best = -1;
        float bestDot = 2.0f;
        for (int i = 0; i < 3; ++i) {
            if (!visible[i]) continue;
            const ImVec2 a(tip[i].x - origin.x, tip[i].y - origin.y);
            const float al = std::max(1e-3f, std::sqrt(a.x * a.x + a.y * a.y));
            const float d = std::abs((dir.x * a.x + dir.y * a.y) / (len * al));
            if (d < bestDot) { bestDot = d; best = i; }
        }
        return best;
    }

    void drawRing(ImDrawList* dl, ImVec2 origin, const ImVec2* tip, int axis,
                  ImU32 col) {
        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
        const ImVec2 u(tip[a].x - origin.x, tip[a].y - origin.y);
        const ImVec2 v(tip[b].x - origin.x, tip[b].y - origin.y);
        constexpr int kSteps = 48;
        for (int s = 0; s < kSteps; ++s) {
            const float t0 = 6.2831853f * static_cast<float>(s) / kSteps;
            const float t1 = 6.2831853f * static_cast<float>(s + 1) / kSteps;
            const ImVec2 p0(origin.x + u.x * std::cos(t0) + v.x * std::sin(t0),
                            origin.y + u.y * std::cos(t0) + v.y * std::sin(t0));
            const ImVec2 p1(origin.x + u.x * std::cos(t1) + v.x * std::sin(t1),
                            origin.y + u.y * std::cos(t1) + v.y * std::sin(t1));
            dl->AddLine(p0, p1, col, 2.0f);
        }
    }

    V3 currentValue(EditorContext& e, const Entity& ent) const {
        switch (mode_) {
        case GizmoMode::Translate: return transformProp(e, ent, "position", {});
        case GizmoMode::Rotate:    return transformProp(e, ent, "rotation", {});
        case GizmoMode::Scale:     return transformProp(e, ent, "scale", {1, 1, 1});
        }
        return {};
    }

    static const char* propNameFor(GizmoMode m) {
        switch (m) {
        case GizmoMode::Rotate: return "rotation";
        case GizmoMode::Scale:  return "scale";
        default:                return "position";
        }
    }

    void finishDrag(EditorContext& e) {
        if (drag_.axis < 0) return;
        if (!drag_.startProp.empty())
            e.recordProp(drag_.entity, "Transform", drag_.startProp,
                         drag_.startLiteral, drag_.startSet);
        drag_.axis = -1;
        drag_.startProp.clear();
    }

    void applyDrag(EditorContext& e, ImVec2 mouse) {
        const ImVec2 delta(mouse.x - drag_.startMouse.x,
                           mouse.y - drag_.startMouse.y);
        const float axisLen2 = drag_.screenAxis.x * drag_.screenAxis.x
                               + drag_.screenAxis.y * drag_.screenAxis.y;
        if (axisLen2 < 1e-6f) return;
        const float along = (delta.x * drag_.screenAxis.x
                             + delta.y * drag_.screenAxis.y)
                            / std::sqrt(axisLen2);

        V3 next = drag_.startValue;
        float* comp = drag_.axis == 0 ? &next.x
                                      : (drag_.axis == 1 ? &next.y : &next.z);
        const char* prop = "position";

        switch (drag_.mode) {
        case GizmoMode::Translate:
            *comp += along * drag_.screenScale;
            prop = "position";
            break;
        case GizmoMode::Rotate:
            *comp += along * 1.5f;
            prop = "rotation";
            break;
        case GizmoMode::Scale:
            *comp = std::max(0.001f, *comp + along * drag_.screenScale);
            prop = "scale";
            break;
        }

        if (ImGui::GetIO().KeyCtrl) {
            const float step = drag_.mode == GizmoMode::Rotate ? 5.0f : 0.1f;
            *comp = std::round(*comp / step) * step;
        }

        setTransform(e, prop, next);
    }

    void setTransform(EditorContext& e, const char* prop, V3 value) {
        const Entity* ent = e.scene.entity(drag_.entity);
        if (!ent) return;
        if (!ent->component("Transform")) return;
        e.putProp(drag_.entity, "Transform", prop, vec3Literal(value));
    }

    void drawOverlay(EditorContext& e, const View& v) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(v.origin,
                         ImVec2(v.origin.x + v.size.x, v.origin.y + v.size.y),
                         true);
        for (const Entity& ent : e.scene.entities()) {
            ImVec2 p;
            if (!project(v, worldPosition(e, ent.id), p)) continue;
            const bool sel = ent.id == e.selectedEntity;
            dl->AddText(ImVec2(p.x + 8, p.y - 7),
                        sel ? IM_COL32(255, 200, 80, 255)
                            : IM_COL32(200, 205, 215, 190),
                        ent.name.c_str());
        }
        dl->PopClipRect();
    }
};

}

void viewportInit(void* device, void* context) {
    if (g_rendererTried) return;
    g_rendererTried = true;
    g_renderer.init(static_cast<ID3D11Device*>(device),
                    static_cast<ID3D11DeviceContext*>(context));
}

void viewportShutdown() {
    g_renderer.shutdown();
    g_rendererTried = false;
}

void viewportInvalidateMeshes() { g_renderer.clearMeshCache(); }

std::vector<std::unique_ptr<IPanel>> makeViewportPanels() {
    std::vector<std::unique_ptr<IPanel>> v;
    v.push_back(std::make_unique<ViewportPanel>());
    return v;
}

}
