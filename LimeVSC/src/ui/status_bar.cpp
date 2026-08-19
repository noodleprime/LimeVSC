#include "ui/panels.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace lime {

extern const int kLogoSize;
extern const std::uint8_t kLogoRGBA[];

namespace {

std::string g_area;
ImTextureID g_logo = 0;
bool g_itemHover = false;


struct Hint {
    const char* key;
    const char* what;
};

const std::vector<Hint> kGraphNode = {
    {"Drag pin", "Connect"},
    {"2xLMB wire", "Waypoint"},
    {"Ctrl+LMB pin", "Move wire"},
};

const std::vector<Hint>& hintsFor(const std::string& area, bool textDoc) {
    static const std::vector<Hint> kGraph = {
        {"LMB", "Select"},        {"RMB", "Add node"},
        {"MMB", "Pan"},           {"Wheel", "Zoom"},
        {"Del", "Delete"},        {"C", "Comment"},
        {"Ctrl+D", "Duplicate"},
    };
    static const std::vector<Hint> kText = {
        {"Ctrl+S", "Save"},     {"Ctrl+Z", "Undo"},
        {"Tab", "Indent"},      {"Ctrl+A", "Select all"},
    };
    static const std::vector<Hint> kViewport = {
        {"LMB", "Select"},      {"RMB", "Look"},
        {"RMB+WASD", "Fly"},    {"MMB", "Pan"},
        {"Alt+LMB", "Orbit"},   {"Wheel", "Dolly"},
        {"W/E/R", "Move/Rotate/Scale"}, {"F", "Focus"},
    };
    static const std::vector<Hint> kHierarchy = {
        {"LMB", "Select"},      {"Drag", "Reparent"},
        {"RMB", "Menu"},        {"Ctrl+Z", "Undo"},
    };
    static const std::vector<Hint> kContent = {
        {"LMB", "Open"},        {"Drag", "Assign asset"},
        {"RMB", "Menu"},
    };
    static const std::vector<Hint> kPalette = {
        {"Drag", "Place node"}, {"Double-click", "Add node"},
    };
    static const std::vector<Hint> kInspector = {
        {"Drag", "Change value"}, {"Ctrl+drag", "Snap"},
        {"Double-click", "Type a value"},
    };
    static const std::vector<Hint> kNone = {
        {"F1", "Nothing here yet"},
    };

    if (area == "Viewport")  return kViewport;
    if (area == "Hierarchy") return kHierarchy;
    if (area == "Content")   return kContent;
    if (area == "Palette")   return kPalette;
    if (area == "Inspector") return kInspector;
    if (area == "Graph")     return textDoc ? kText : kGraph;
    return kNone;
}

}

void statusSetArea(const char* area) { g_area = area ? area : ""; }

void statusSetItemHover() { g_itemHover = true; }

ImTextureID statusLogo() { return g_logo; }

void statusInitLogo(void* d3d11Device) {
    if (g_logo || !d3d11Device) return;
    g_logo = reinterpret_cast<ImTextureID>(createLogoTexture(d3d11Device));
}

void drawStatusBar(EditorContext& e) {
    const float height = ImGui::GetFrameHeight();
    if (!ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(),
                                     ImGuiDir_Down, height,
                                     ImGuiWindowFlags_NoScrollbar
                                         | ImGuiWindowFlags_NoScrollWithMouse
                                         | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    const ImVec2 origin = ImGui::GetWindowPos();
    const float  barW = ImGui::GetWindowWidth();
    const float  barH = ImGui::GetWindowHeight();
    const float  pad = ImGui::GetStyle().ItemSpacing.x;
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    const float textH = ImGui::GetTextLineHeight();
    const float textY = origin.y + (barH - textH) * 0.5f;
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 bright = ImGui::GetColorU32(ImGuiCol_Text);

    float x = origin.x + pad;
    auto put = [&](const char* s, ImU32 col) {
        dl->AddText(ImVec2(x, textY), col, s);
        x += ImGui::CalcTextSize(s).x + pad;
    };

    put(g_area.empty() ? "-" : g_area.c_str(), dim);
    put("|", dim);
    const bool onItem = g_itemHover && g_area == "Graph" && !e.doc().isText();
    for (const Hint& h : onItem ? kGraphNode
                                : hintsFor(g_area, e.doc().isText())) {
        put(h.key, bright);
        put(h.what, dim);
    }
    g_itemHover = false;

    const float logoSide = barH - 6.0f;
    const std::string version = std::string("v") + LIMEVSC_VERSION;
    const float versionW = ImGui::CalcTextSize(version.c_str()).x;

    float rx = origin.x + barW - pad;
    if (g_logo) {
        rx -= logoSide;
        const float logoY = origin.y + (barH - logoSide) * 0.5f;
        dl->AddImage(g_logo, ImVec2(rx, logoY),
                     ImVec2(rx + logoSide, logoY + logoSide));

        if (ImGui::IsWindowHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            if (m.x >= rx && m.x <= rx + logoSide && m.y >= logoY
                && m.y <= logoY + logoSide)
                ImGui::SetTooltip("LimeVSC %s\nVisual scripting for LimeX",
                                  LIMEVSC_VERSION);
        }
        rx -= pad;
    }
    rx -= versionW;
    dl->AddText(ImVec2(rx, textY), dim, version.c_str());

    ImGui::End();
}

namespace {

double holdFor(EditorContext::NoteKind k) {
    switch (k) {
    case EditorContext::NoteKind::Error:   return 7.0;
    case EditorContext::NoteKind::Warning: return 5.0;
    default:                               return 2.2;
    }
}

void accentFor(EditorContext::NoteKind k, int& r, int& g, int& b) {
    switch (k) {
    case EditorContext::NoteKind::Error:   r = 226; g = 92;  b = 84;  break;
    case EditorContext::NoteKind::Warning: r = 226; g = 170; b = 74;  break;
    default:                               r = 140; g = 209; b = 115; break;
    }
}

struct Live {
    std::uint64_t serial = 0;
    double        born = 0.0;
};

}

void drawNotes(EditorContext& e) {
    static std::vector<Live>  live;
    static std::uint64_t      adoptedTo = 0;

    const double now = ImGui::GetTime();
    for (const EditorContext::Note& n : e.notes)
        if (n.serial > adoptedTo) live.push_back({n.serial, now});
    if (!e.notes.empty()) adoptedTo = e.notes.back().serial;

    const auto find = [&](std::uint64_t serial) -> const EditorContext::Note* {
        for (const EditorContext::Note& n : e.notes)
            if (n.serial == serial) return &n;
        return nullptr;
    };

    live.erase(std::remove_if(live.begin(), live.end(),
                              [&](const Live& l) {
                                  const EditorContext::Note* n = find(l.serial);
                                  return !n || now - l.born >= holdFor(n->kind);
                              }),
               live.end());
    if (live.empty()) return;

    constexpr std::size_t kMaxShown = 5;
    constexpr float kFade = 0.45f;
    constexpr float kGap = 6.0f;
    const ImVec2 pad(14.0f, 9.0f);
    const float  accent = 3.0f;
    const float  margin = 14.0f;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float bottom = vp->Pos.y + vp->Size.y - ImGui::GetFrameHeight() - margin;
    const float right = vp->Pos.x + vp->Size.x - margin;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    std::size_t drawn = 0;
    for (auto it = live.rbegin(); it != live.rend() && drawn < kMaxShown;
         ++it, ++drawn) {
        const EditorContext::Note* n = find(it->serial);
        if (!n) continue;

        const double left = holdFor(n->kind) - (now - it->born);
        const float  a = static_cast<float>(
            left < kFade ? std::max(0.0, left) / kFade : 1.0);
        const auto fade = [a](int r, int g, int b, int alpha) {
            return IM_COL32(r, g, b,
                            static_cast<int>(static_cast<float>(alpha) * a));
        };

        const char*  text = n->text.c_str();
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 br(right, bottom);
        const ImVec2 tl(br.x - ts.x - pad.x * 2.0f - accent,
                        br.y - ts.y - pad.y * 2.0f);

        dl->AddRectFilled(tl, br, fade(22, 24, 27, 242));
        dl->AddRect(tl, br, fade(58, 64, 70, 255));
        int ar = 0, ag = 0, ab = 0;
        accentFor(n->kind, ar, ag, ab);
        dl->AddRectFilled(tl, ImVec2(tl.x + accent, br.y), fade(ar, ag, ab, 255));
        dl->AddText(ImVec2(tl.x + accent + pad.x, tl.y + pad.y),
                    fade(226, 232, 238, 255), text);

        bottom = tl.y - kGap;
    }
}

}
