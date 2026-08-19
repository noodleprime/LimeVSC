#include "ui/panels.h"
#include "ui/pixel_wire.h"
#include "ui/canvas_ids.h"
#include "api/graphfn_provider.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ed = ax::NodeEditor;

namespace lime {
namespace {

void commit(EditorContext& e);
std::string uniqueVarName(const Graph& g);
std::string defaultFor(const std::string& type);

std::uintptr_t encNode(NodeId n) {
    return static_cast<std::uintptr_t>(encNodeId(n.v));
}
NodeId decNode(ed::NodeId id) {
    return NodeId{decNodeId(static_cast<std::uint64_t>(id.Get()))};
}

std::uintptr_t encPin(NodeId n, PinName p) {
    return static_cast<std::uintptr_t>(encPinId(n.v, p.v));
}
PinId decPin(ed::PinId id) {
    const auto raw = static_cast<std::uint64_t>(id.Get());
    PinId out;
    out.node = NodeId{decPinNode(raw)};
    out.pin  = PinName{decPinName(raw)};
    return out;
}

ImU32 toImCol(std::uint32_t rgba) {
    const ImU32 r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF;
    const ImU32 b = (rgba >> 8) & 0xFF,  a = rgba & 0xFF;
    return IM_COL32(r, g, b, a);
}

ImU32 pinColor(const EditorContext& ed_, const PinDesc& p) {
    if (p.kind == PinKind::Exec) return IM_COL32(228, 230, 233, 255);
    return toImCol(ed_.types.get(p.type).color);
}

void applyCanvasStyle() {
    ed::Style& s = ed::GetStyle();
    s.NodeRounding             = 0.0f;
    s.PinRounding              = 0.0f;
    s.GroupRounding            = 0.0f;
    s.NodeBorderWidth          = 1.0f;
    s.HoveredNodeBorderWidth   = 2.0f;
    s.SelectedNodeBorderWidth  = 2.0f;
    s.GroupBorderWidth         = 1.0f;
    s.NodePadding              = ImVec4(10, 6, 10, 8);
    s.LinkStrength             = 130.0f;
    s.PinArrowSize             = 0.0f;
    s.PinArrowWidth            = 0.0f;

    s.Colors[ed::StyleColor_Bg]                = ImColor(16, 17, 19, 255);
    s.Colors[ed::StyleColor_Grid]              = ImColor(255, 255, 255, 8);
    s.Colors[ed::StyleColor_NodeBg]            = ImColor(28, 30, 33, 245);
    s.Colors[ed::StyleColor_NodeBorder]        = ImColor(52, 57, 62, 255);
    s.Colors[ed::StyleColor_HovNodeBorder]     = ImColor(140, 209, 115, 255);
    s.Colors[ed::StyleColor_SelNodeBorder]     = ImColor(170, 235, 130, 255);
    s.Colors[ed::StyleColor_NodeSelRect]       = ImColor(140, 209, 115, 36);
    s.Colors[ed::StyleColor_NodeSelRectBorder] = ImColor(140, 209, 115, 120);
    s.Colors[ed::StyleColor_HovLinkBorder]     = ImColor(140, 209, 115, 255);
    s.Colors[ed::StyleColor_SelLinkBorder]     = ImColor(170, 235, 130, 255);
    s.Colors[ed::StyleColor_GroupBg]           = ImColor(255, 255, 255, 10);
    s.Colors[ed::StyleColor_GroupBorder]       = ImColor(125, 135, 145, 90);
}

namespace pinart {

constexpr int kSide = 9;

constexpr const char* kExecFilled[kSide] = {
    "..X......", "..XX.....", "..XXX....",
    "..XXXX...", "..XXXXX..", "..XXXX...",
    "..XXX....", "..XX.....", "..X......",
};
constexpr const char* kExecHollow[kSide] = {
    "..X......", "..XX.....", "..X.X....",
    "..X..X...", "..X...X..", "..X..X...",
    "..X.X....", "..XX.....", "..X......",
};
constexpr const char* kDataFilled[kSide] = {
    "..XXXXX..", ".XXXXXXX.", "XXXXXXXXX",
    "XXXXXXXXX", "XXXXXXXXX", "XXXXXXXXX",
    "XXXXXXXXX", ".XXXXXXX.", "..XXXXX..",
};
constexpr const char* kDataHollow[kSide] = {
    "..XXXXX..", ".XX...XX.", "XX.....XX",
    "X..XXX..X", "X..XXX..X", "X..XXX..X",
    "XX.....XX", ".XX...XX.", "..XXXXX..",
};

}

void drawPinIcon(ImU32 color, bool exec, bool connected) {
    const float  sz = ImGui::GetTextLineHeight();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(sz, sz));

    const char* const* art =
        exec ? (connected ? pinart::kExecFilled : pinart::kExecHollow)
             : (connected ? pinart::kDataFilled : pinart::kDataHollow);
    const ImU32 col = connected ? color : ((color & 0x00FFFFFFu) | 0xC0000000u);

    const float  cell = std::max(1.0f, std::floor(sz / 11.0f));
    const float  span = cell * pinart::kSide;
    const ImVec2 o(std::floor(p.x + (sz - span) * 0.5f),
                   std::floor(p.y + (sz - span) * 0.5f));

    for (int y = 0; y < pinart::kSide; ++y) {
        const char* row = art[y];
        for (int x = 0; x < pinart::kSide;) {
            if (row[x] != 'X') { ++x; continue; }
            int run = 1;
            while (x + run < pinart::kSide && row[x + run] == 'X') ++run;
            dl->AddRectFilled(
                ImVec2(o.x + static_cast<float>(x) * cell,
                       o.y + static_cast<float>(y) * cell),
                ImVec2(o.x + static_cast<float>(x + run) * cell,
                       o.y + static_cast<float>(y + 1) * cell),
                col);
            x += run;
        }
    }
}

std::vector<std::string> splitCategory(const std::string& cat) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= cat.size()) {
        const std::size_t slash = cat.find('/', start);
        const std::string seg = cat.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!seg.empty()) out.push_back(seg);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (out.empty()) out.push_back("Uncategorised");
    return out;
}

ImU32 headerColor(std::size_t depth) {
    switch (depth) {
    case 0:  return IM_COL32(150, 205, 125, 255);
    case 1:  return IM_COL32(120, 165, 105, 255);
    default: return IM_COL32(100, 130,  95, 255);
    }
}

ImU32 roleColor(const NodeDesc& d) {
    if (d.isEvent) return IM_COL32(226, 142, 120, 255);
    if (d.pure)    return IM_COL32(150, 214, 160, 255);
    return IM_COL32(163, 191, 226, 255);
}

bool isComment(const EditorContext& e, const Node& n) {
    const NodeDesc* d = e.nodes.find(n.type);
    return d && d->emit == "struct:comment";
}
bool isReroute(const EditorContext& e, const Node& n) {
    const NodeDesc* d = e.nodes.find(n.type);
    return d && (d->emit == "reroute" || d->emit == "reroute.exec");
}

bool fuzzyMatch(std::string_view needle, std::string_view hay) {
    if (needle.empty()) return true;
    std::size_t n = 0;
    for (char h : hay) {
        if (n < needle.size()
            && std::tolower(static_cast<unsigned char>(h))
                   == std::tolower(static_cast<unsigned char>(needle[n])))
            ++n;
    }
    return n == needle.size();
}

ImGuiID g_graphDock = 0;

GraphDoc* g_doc = nullptr;

struct PendingLink {
    bool   active = false;
    PinId  from{};
    PinKind kind = PinKind::Data;
    TypeId type{};
    PinDir fromDir = PinDir::Out;
    ImVec2 canvasPos{};
};
PendingLink g_pending;

std::uint64_t g_lastHoveredLink = 0;
bool g_hoverHeld = false;

struct DragOrigin {
    std::uint32_t id;
    float         x, y;
};
std::vector<DragOrigin> g_dragFrom;

struct DragSize {
    std::uint32_t id;
    float         w, h;
};
std::vector<DragSize> g_sizeFrom;

struct PinRect {
    PinId  pin;
    ImVec2 min, max;
    ImVec2 iconMin, iconMax;
};
std::vector<PinRect> g_pinRects;

struct CarriedWire {
    bool    active = false;
    PinId   anchor{};
    bool    anchorIsOutput = false;
    PinKind kind = PinKind::Data;

    PinDir wants() const { return anchorIsOutput ? PinDir::In : PinDir::Out; }
};
CarriedWire g_carry;

bool g_requestAddPopup = false;

const PinRect* pinAt(ImVec2 canvasPos) {
    for (const PinRect& r : g_pinRects)
        if (canvasPos.x >= r.min.x && canvasPos.x <= r.max.x
            && canvasPos.y >= r.min.y && canvasPos.y <= r.max.y)
            return &r;
    return nullptr;
}

void notePinRect(NodeId n, PinName p, ImVec2 iconMin, ImVec2 iconMax) {
    g_pinRects.push_back({PinId{n, p}, ImGui::GetItemRectMin(),
                          ImGui::GetItemRectMax(), iconMin, iconMax});
}

ImVec2 rectCenter(ImVec2 lo, ImVec2 hi) {
    return ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
}

ImVec2 wireAnchor(const PinRect& r) {
    return rectCenter(r.iconMin, r.iconMax);
}

const PinRect* pinNear(ImVec2 canvasPos, float slack) {
    const PinRect* best = nullptr;
    float bestD = slack;
    for (const PinRect& r : g_pinRects) {
        const ImVec2 c((r.min.x + r.max.x) * 0.5f, (r.min.y + r.max.y) * 0.5f);
        const float dx = canvasPos.x - c.x, dy = canvasPos.y - c.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < bestD) { bestD = d; best = &r; }
    }
    return best ? best : pinAt(canvasPos);
}

ImVec2 g_lastCanvasMouse{};

std::set<std::string> g_expanded;

const PinDesc* findPin(EditorContext& e, NodeId n, PinName p) {
    const Node* node = e.graph().node(n);
    if (!node) return nullptr;
    const NodeDesc* d = e.nodes.find(node->type);
    if (!d) return nullptr;
    return d->findPin(p.str());
}

int relevance(const NodeDesc& d, std::string_view q) {
    if (q.empty()) return 1;

    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(std::string(q));
    const std::string display = lower(d.display);
    const std::string id = lower(d.id);

    if (display == needle) return 1000;
    if (id == needle) return 950;

    const std::size_t dot = id.find_last_of('.');
    if (dot != std::string::npos && id.substr(dot + 1) == needle) return 900;

    if (display.rfind(needle, 0) == 0) return 800 - static_cast<int>(display.size());
    if (dot != std::string::npos && id.compare(dot + 1, needle.size(), needle) == 0)
        return 700 - static_cast<int>(id.size() - dot);

    for (std::size_t i = 1; i < display.size(); ++i) {
        if (display[i - 1] != ' ' && display[i - 1] != '.') continue;
        if (display.compare(i, needle.size(), needle) == 0) return 600;
    }
    if (display.find(needle) != std::string::npos) return 400;
    if (id.find(needle) != std::string::npos) return 300;

    if (fuzzyMatch(std::string(q).c_str(), d.display)
        || fuzzyMatch(std::string(q).c_str(), d.id)
        || fuzzyMatch(std::string(q).c_str(), d.category))
        return 100;
    return 0;
}

const PinDesc* landingFor(EditorContext& e, const NodeDesc& d) {
    for (const PinDesc& p : d.pins) {
        if (p.kind != g_pending.kind) continue;
        if (p.dir == g_pending.fromDir) continue;
        if (p.kind == PinKind::Data) {
            const bool srcIsPending = g_pending.fromDir == PinDir::Out;
            const TypeId from = srcIsPending ? g_pending.type : p.type;
            const TypeId to   = srcIsPending ? p.type : g_pending.type;
            if (!e.types.canConnect(from, to)) continue;
        }
        return &p;
    }
    return nullptr;
}

bool canLink(EditorContext& e, PinId a, PinId b, PinKind& kindOut) {
    if (!a.valid() || !b.valid() || a.node == b.node) return false;
    const PinDesc* pa = findPin(e, a.node, a.pin);
    const PinDesc* pb = findPin(e, b.node, b.pin);
    if (!pa || !pb) return false;
    if (pa->kind != pb->kind) return false;
    if (pa->dir == pb->dir) return false;

    kindOut = pa->kind;
    if (pa->kind == PinKind::Exec) return true;

    const PinDesc* src = (pa->dir == PinDir::Out) ? pa : pb;
    const PinDesc* dst = (pa->dir == PinDir::Out) ? pb : pa;
    return e.types.canConnect(src->type, dst->type);
}

class CanvasPanel final : public IPanel {
public:
    std::string_view id() const override { return "canvas"; }
    std::string_view title() const override { return "Graph"; }

    void draw(EditorContext& e) override {
        ed::SetCurrentEditor(static_cast<ed::EditorContext*>(g_doc->canvas));

        ImGui::PushItemFlag(ImGuiItemFlags_AllowDuplicateId, true);
        const std::string canvasId = "canvas##" + std::to_string(g_doc->id);
        ed::Begin(canvasId.c_str());

        ImDrawList* const canvasDl = ImGui::GetWindowDrawList();
        const ImDrawListFlags savedAA = canvasDl->Flags;
        canvasDl->Flags &= ~(ImDrawListFlags_AntiAliasedLines
                             | ImDrawListFlags_AntiAliasedLinesUseTex
                             | ImDrawListFlags_AntiAliasedFill);

        g_pinRects.clear();
        drawNodes(e);
        drawLinks(e);
        drawCarriedWire();
        drawPendingWire();
        handleCreate(e);
        handleDelete(e);
        syncSelection(e);
        syncPositions(e);

        const ed::NodeId hovered = ed::GetHoveredNode();

        ed::Suspend();
        drawHoverTooltip(e, hovered);
        drawContextMenus(e);
        ed::Resume();

        ed::End();
        canvasDl->Flags = savedAA;

        const ed::LinkId hoveredLink = ed::GetHoveredLink();
        if (hoveredLink)
            g_lastHoveredLink = static_cast<std::uint64_t>(hoveredLink.Get());

        const bool overItem =
            static_cast<bool>(hovered) || static_cast<bool>(hoveredLink);
        const bool holding =
            ImGui::IsMouseDown(ImGuiMouseButton_Left) || g_carry.active;
        if (!holding) g_hoverHeld = overItem;
        if (overItem || (holding && g_hoverHeld)) statusSetItemHover();

        splitLinkOnDoubleClick(e);
        updateWireCarry(e);

        ImGui::PopItemFlag();

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("LIME_NODE")) {
                const char* id = static_cast<const char*>(pl->Data);
                if (const NodeDesc* nd = e.nodes.find(id)) {
                    const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());
                    const NodeId made = e.addNode(*nd, at.x, at.y);
                    ed::SetNodePosition(encNode(made), at);
                    g_doc->placed.insert(made.v);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ed::SetCurrentEditor(nullptr);
    }

private:
    static void drawNodes(EditorContext& e) {
        for (const Node& n : e.graph().nodes())
            if (isComment(e, n)) drawComment(e, n);
        for (const Node& n : e.graph().nodes())
            if (!isComment(e, n)) drawNode(e, n);
    }

    static void place(const Node& n, bool comment) {
        if (!g_doc->placed.insert(n.id.v).second) return;
        ed::SetNodePosition(encNode(n.id), ImVec2(n.x, n.y));
        if (comment && n.w > 0 && n.h > 0)
            ed::SetGroupSize(encNode(n.id), ImVec2(n.w, n.h));
    }

    static void drawComment(EditorContext& e, const Node& n) {
        place(n, true);
        const ImVec2 size(n.w > 0 ? n.w : 340.0f, n.h > 0 ? n.h : 190.0f);

        ed::PushStyleColor(ed::StyleColor_NodeBg,     ImColor(255, 255, 255, 14));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(150, 160, 170, 110));

        ed::BeginNode(encNode(n.id));
        ImGui::PushID(static_cast<int>(n.id.v));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(190, 200, 210, 255));
        ImGui::TextUnformatted(n.comment.empty() ? "Comment" : n.comment.c_str());
        ImGui::PopStyleColor();
        ed::Group(size);
        ImGui::PopID();
        ed::EndNode();

        ed::PopStyleColor(2);

        if (ed::BeginGroupHint(encNode(n.id))) {
            const ImVec2 min = ed::GetGroupMin();
            ImGui::SetCursorScreenPos(
                ImVec2(min.x + 8, min.y - ImGui::GetTextLineHeightWithSpacing() - 4));
            ImGui::BeginGroup();
            ImGui::TextUnformatted(n.comment.empty() ? "Comment" : n.comment.c_str());
            ImGui::EndGroup();
        }
        ed::EndGroupHint();
    }

    static void drawReroute(EditorContext& e, const Node& n) {
        const NodeDesc* d = e.nodes.find(n.type);
        if (!d) return;
        const PinDesc* inPin = d->findPin("in");
        const PinDesc* outPin = d->findPin("ret");
        const char* outName = "ret";
        if (!outPin) {
            outPin = d->findPin("out");
            outName = "out";
        }
        const bool execWaypoint = outPin && outPin->kind == PinKind::Exec;

        ed::PushStyleColor(ed::StyleColor_NodeBg,     ImColor(0, 0, 0, 0));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(0, 0, 0, 0));
        ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(0, 0, 0, 0));

        ed::BeginNode(encNode(n.id));
        ImGui::PushID(static_cast<int>(n.id.v));
        ImU32 col = IM_COL32(160, 165, 170, 255);
        if (execWaypoint) {
            col = IM_COL32(228, 230, 233, 255);
        } else if (const auto src = e.graph().sourceOf(PinId::make(n.id, "in"))) {
            if (const PinDesc* sp = findPin(e, src->node, src->pin))
                col = pinColor(e, *sp);
        }

        if (inPin) {
            ImGui::PushID("in");
            ed::BeginPin(encPin(n.id, PinName::of("in")), ed::PinKind::Input);
            drawPinIcon(col, execWaypoint,
                        execWaypoint
                            ? !e.graph()
                                   .execSourcesOf(PinId::make(n.id, "in"))
                                   .empty()
                            : e.graph().sourceOf(PinId::make(n.id, "in")).has_value());
            const ImVec2 iMin = ImGui::GetItemRectMin();
            const ImVec2 iMax = ImGui::GetItemRectMax();
            const ImVec2 iMid = rectCenter(iMin, iMax);
            ed::PinPivotRect(iMid, iMid);
            ed::EndPin();
            notePinRect(n.id, PinName::of("in"), iMin, iMax);
            ImGui::PopID();
        }
        ImGui::SameLine(0, 1);
        if (outPin) {
            ImGui::PushID(outName);
            ed::BeginPin(encPin(n.id, PinName::of(outName)), ed::PinKind::Output);
            drawPinIcon(col, execWaypoint,
                        execWaypoint
                            ? e.graph()
                                  .execTargetOf(PinId::make(n.id, outName))
                                  .has_value()
                            : !e.graph().targetsOf(PinId::make(n.id, outName))
                                   .empty());
            const ImVec2 oMin = ImGui::GetItemRectMin();
            const ImVec2 oMax = ImGui::GetItemRectMax();
            const ImVec2 oMid = rectCenter(oMin, oMax);
            ed::PinPivotRect(oMid, oMid);
            ed::EndPin();
            notePinRect(n.id, PinName::of(outName), oMin, oMax);
            ImGui::PopID();
        }
        ImGui::PopID();
        ed::EndNode();

        ed::PopStyleVar();
        ed::PopStyleColor(2);
    }

    static void drawNode(EditorContext& e, const Node& n) {
        place(n, false);
        if (isReroute(e, n)) { drawReroute(e, n); return; }

        const NodeDesc* d = e.nodes.find(n.type);

        ImU32 accent = IM_COL32(62, 68, 74, 255);
        if (d) {
            if (d->isEvent)   accent = IM_COL32(122, 52, 40, 255);
            else if (d->pure) accent = IM_COL32(44, 74, 50, 255);
            else              accent = IM_COL32(44, 56, 74, 255);
        } else {
            accent = IM_COL32(96, 38, 38, 255);
        }

        ed::BeginNode(encNode(n.id));
        ImGui::PushID(static_cast<int>(n.id.v));

        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(236, 238, 240, 255));
        if (d) ImGui::TextUnformatted(d->display.c_str());
        else   ImGui::Text("? %s", n.type.c_str());
        ImGui::PopStyleColor();
        const float headerBottom = ImGui::GetItemRectMax().y;

        ImGui::Dummy(ImVec2(0, 3));

        ImGui::BeginGroup();
        if (d) for (const PinDesc& p : d->pins) {
            if (p.dir != PinDir::In) continue;
            const PinId pid = PinId::make(n.id, p.name);
            const bool linked =
                p.kind == PinKind::Exec
                    ? !e.graph().execSourcesOf(pid).empty()
                    : e.graph().sourceOf(pid).has_value();
            ImGui::PushID(p.name.c_str());
            ed::BeginPin(encPin(n.id, PinName::of(p.name)), ed::PinKind::Input);
            drawPinIcon(pinColor(e, p), p.kind == PinKind::Exec, linked);
            const ImVec2 iMin = ImGui::GetItemRectMin();
            const ImVec2 iMax = ImGui::GetItemRectMax();
            const ImVec2 iMid = rectCenter(iMin, iMax);
            ed::PinPivotRect(iMid, iMid);
            ImGui::SameLine(0, 5);
            ImGui::TextUnformatted(p.name.c_str());
            ed::EndPin();
            notePinRect(n.id, PinName::of(p.name), iMin, iMax);
            ImGui::PopID();
        }
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(18, 1));
        ImGui::SameLine();

        ImGui::BeginGroup();
        if (d) for (const PinDesc& p : d->pins) {
            if (p.dir != PinDir::Out) continue;
            const PinId pid = PinId::make(n.id, p.name);
            const bool linked = p.kind == PinKind::Exec
                                    ? e.graph().execTargetOf(pid).has_value()
                                    : !e.graph().targetsOf(pid).empty();
            ImGui::PushID(p.name.c_str());
            ed::BeginPin(encPin(n.id, PinName::of(p.name)), ed::PinKind::Output);
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::SameLine(0, 5);
            drawPinIcon(pinColor(e, p), p.kind == PinKind::Exec, linked);
            const ImVec2 oMin = ImGui::GetItemRectMin();
            const ImVec2 oMax = ImGui::GetItemRectMax();
            const ImVec2 oMid = rectCenter(oMin, oMax);
            ed::PinPivotRect(oMid, oMid);
            ed::EndPin();
            notePinRect(n.id, PinName::of(p.name), oMin, oMax);
            ImGui::PopID();
        }
        ImGui::EndGroup();

        if (!n.rawBody.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 155, 162, 255));
            const std::size_t nl = n.rawBody.find('\n');
            std::string first = n.rawBody.substr(0, nl);
            if (nl != std::string::npos) first += " ...";
            ImGui::TextUnformatted(first.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::PopID();
        ed::EndNode();

        const ImVec2 nmin = ImGui::GetItemRectMin();
        const ImVec2 nmax = ImGui::GetItemRectMax();
        if (ImDrawList* dl = ed::GetNodeBackgroundDrawList(encNode(n.id))) {
            dl->AddRectFilled(ImVec2(nmin.x + 1, nmin.y + 1),
                              ImVec2(nmax.x - 1, headerBottom + 3), accent, 0.0f);
            dl->AddLine(ImVec2(nmin.x + 1, headerBottom + 3),
                        ImVec2(nmax.x - 1, headerBottom + 3),
                        IM_COL32(0, 0, 0, 70), 1.0f);
        }
    }

    static void drawLinks(EditorContext& e) {
        int i = 0;
        for (const Link& l : e.graph().links()) {
            const PinDesc* p = findPin(e, l.from.node, l.from.pin);
            const ImU32 col = p ? pinColor(e, *p) : IM_COL32(200, 200, 200, 255);
            ed::Link(static_cast<std::uintptr_t>(encLinkId(static_cast<std::uint32_t>(i++))),
                     encPin(l.from.node, l.from.pin),
                     encPin(l.to.node, l.to.pin), ImColor(col), 1.0f);
        }
    }

    static void handleCreate(EditorContext& e) {
        if (g_carry.active) {
            if (ed::BeginCreate(ImVec4(0, 0, 0, 0), 0.0f)) {
                ed::PinId a, b;
                if (ed::QueryNewLink(&a, &b)) ed::RejectNewItem(ImVec4(0, 0, 0, 0), 0.0f);
                ed::PinId lone;
                if (ed::QueryNewNode(&lone)) ed::RejectNewItem(ImVec4(0, 0, 0, 0), 0.0f);
            }
            ed::EndCreate();
            return;
        }
        if (ed::BeginCreate(ImVec4(1, 1, 1, 1), 0.0f)) {
            ed::PinId a, b;
            if (ed::QueryNewLink(&a, &b)) {
                const PinId pa = decPin(a), pb = decPin(b);
                PinKind kind{};
                if (canLink(e, pa, pb, kind)) {
                    if (ed::AcceptNewItem(ImColor(120, 220, 120), 0.5f)) {
                        const PinDesc* da = findPin(e, pa.node, pa.pin);
                        const bool aIsOut = da && da->dir == PinDir::Out;
                        e.connect(aIsOut ? pa : pb, aIsOut ? pb : pa, kind);
                    }
                } else {
                    ed::RejectNewItem(ImColor(230, 90, 90), 0.5f);
                }
            }

            ed::PinId lone;
            if (ed::QueryNewNode(&lone)) {
                if (ed::AcceptNewItem(ImVec4(1, 1, 1, 1), 0.0f)) {
                    const PinId p = decPin(lone);
                    if (const PinDesc* pd = findPin(e, p.node, p.pin)) {
                        g_pending = {true, p, pd->kind, pd->type, pd->dir,
                                     ed::ScreenToCanvas(ImGui::GetMousePos())};
                        ed::Suspend();
                        ImGui::OpenPopup("canvas.add");
                        ed::Resume();
                    }
                }
            }
        }
        ed::EndCreate();
    }

    static void splitLinkOnDoubleClick(EditorContext& e) {
        std::uint64_t raw = static_cast<std::uint64_t>(ed::GetDoubleClickedLink().Get());
        if (!raw && g_lastHoveredLink
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            raw = g_lastHoveredLink;
        if (!raw) return;

        const int idx = static_cast<int>(decLinkIndex(raw));
        const auto links = e.graph().links();
        if (idx < 0 || idx >= static_cast<int>(links.size())) return;

        const Link l = links[static_cast<std::size_t>(idx)];

        const bool exec = l.kind == PinKind::Exec;
        const NodeDesc* rd =
            e.nodes.find(exec ? "core.reroute.exec" : "core.reroute");
        if (!rd) return;

        const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());
        const NodeId made = e.addNode(*rd, at.x, at.y);
        ed::SetNodePosition(encNode(made), at);
        g_doc->placed.insert(made.v);

        e.connect(l.from, PinId::make(made, "in"), l.kind);
        e.connect(PinId::make(made, exec ? "out" : "ret"), l.to, l.kind);
    }

    static void updateWireCarry(EditorContext& e) {
        const ImVec2 mouseCanvas = ed::ScreenToCanvas(ImGui::GetMousePos());

        if (g_carry.active) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)
                || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                g_carry = {};
                return;
            }

            if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;

            const PinRect* hit = pinNear(mouseCanvas, 14.0f);
            const PinDesc* pd =
                hit ? findPin(e, hit->pin.node, hit->pin.pin) : nullptr;
            if (pd && pd->dir == g_carry.wants()) {
                PinKind kind{};
                if (canLink(e, g_carry.anchor, hit->pin, kind)) {
                    if (g_carry.anchorIsOutput)
                        e.connect(g_carry.anchor, hit->pin, kind);
                    else
                        e.connect(hit->pin, g_carry.anchor, kind);
                }
            }
            else if (!hit) {
                const PinDesc* ad =
                    findPin(e, g_carry.anchor.node, g_carry.anchor.pin);
                g_pending = {true,
                             g_carry.anchor,
                             g_carry.kind,
                             ad ? ad->type : TypeId{},
                             g_carry.anchorIsOutput ? PinDir::Out : PinDir::In,
                             mouseCanvas};
                g_requestAddPopup = true;
            }
            g_carry = {};
            return;
        }

        if (!ImGui::GetIO().KeyCtrl) return;
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

        const PinRect* hit = pinNear(mouseCanvas, 14.0f);
        if (!hit) return;

        const PinDesc* pd = findPin(e, hit->pin.node, hit->pin.pin);
        if (!pd) return;

        if (pd->dir == PinDir::In) {
            PinId src;
            if (pd->kind == PinKind::Exec) {
                const auto s = e.graph().execSourcesOf(hit->pin);
                if (s.empty()) return;
                src = s.front();
            } else {
                const auto s = e.graph().sourceOf(hit->pin);
                if (!s) return;
                src = *s;
            }
            e.disconnectInput(hit->pin);
            g_carry = {true, src,  true, pd->kind};
            return;
        }

        std::vector<PinId> sinks;
        for (const Link& l : e.graph().links())
            if (l.from == hit->pin) sinks.push_back(l.to);
        if (sinks.empty()) return;

        e.disconnectOutput(hit->pin);
        if (sinks.size() == 1)
            g_carry = {true, sinks.front(),  false, pd->kind};
    }

    static void drawPendingWire() {
        if (!g_pending.active) return;
        const PinRect* from = nullptr;
        for (const PinRect& r : g_pinRects)
            if (r.pin == g_pending.from) { from = &r; break; }
        if (!from) return;

        const ImVec2 a = wireAnchor(*from);
        const ImVec2 b = g_pending.canvasPos;
        const float span = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
        const float dir = g_pending.fromDir == PinDir::Out ? 1.0f : -1.0f;

        ImDrawList* dl = ed::GetNodeBackgroundDrawList(
            ed::NodeId(encNodeId(g_pending.from.node.v)));
        if (!dl) dl = ImGui::GetWindowDrawList();
        buildPixelWire(a, ImVec2(a.x + span * dir, a.y),
                       ImVec2(b.x - span * dir, b.y), b, 1.0f, 1.0f,
                       [dl](ImVec2 lo, ImVec2 hi) {
                           dl->AddRectFilled(lo, hi, IM_COL32(150, 210, 255, 220));
                       });
    }

    static void drawCarriedWire() {
        if (!g_carry.active) return;

        const PinRect* anchor = nullptr;
        for (const PinRect& r : g_pinRects)
            if (r.pin == g_carry.anchor) { anchor = &r; break; }
        if (!anchor) return;

        const ImVec2 a = wireAnchor(*anchor);
        const ImVec2 b = ImGui::GetMousePos();

        const float span = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
        const float dir = g_carry.anchorIsOutput ? 1.0f : -1.0f;

        ImDrawList* dl = ed::GetNodeBackgroundDrawList(
            ed::NodeId(encNodeId(g_carry.anchor.node.v)));
        if (!dl) dl = ImGui::GetWindowDrawList();
        buildPixelWire(a, ImVec2(a.x + span * dir, a.y),
                       ImVec2(b.x - span * dir, b.y), b, 1.0f, 1.0f,
                       [dl](ImVec2 lo, ImVec2 hi) {
                           dl->AddRectFilled(lo, hi, IM_COL32(255, 210, 90, 230));
                       });
    }

    static void handleDelete(EditorContext& e) {
        if (ed::BeginDelete()) {
            ed::LinkId l;
            while (ed::QueryDeletedLink(&l)) {
                if (!ed::AcceptDeletedItem()) continue;
                const int idx = static_cast<int>(
                    decLinkIndex(static_cast<std::uint64_t>(l.Get())));
                const auto links = e.graph().links();
                if (idx >= 0 && idx < static_cast<int>(links.size()))
                    e.disconnectInput(links[idx].to);
            }
            ed::NodeId n;
            while (ed::QueryDeletedNode(&n)) {
                if (!ed::AcceptDeletedItem()) continue;
                const NodeId id = decNode(n);
                e.deleteNodes(std::span<const NodeId>(&id, 1));
            }
        }
        ed::EndDelete();
    }

    static void syncSelection(EditorContext& e) {
        const int count = ed::GetSelectedObjectCount();
        std::vector<ed::NodeId> sel(static_cast<std::size_t>(std::max(count, 0)));
        const int got = sel.empty() ? 0 : ed::GetSelectedNodes(sel.data(), count);

        e.selection().clear();
        for (int i = 0; i < got; ++i) e.selection().push_back(decNode(sel[i]));
        const NodeId was = e.inspected();
        e.inspected() = e.selection().empty() ? NodeId{} : e.selection().front();
        if (e.inspected().valid() && e.inspected() != was)
            e.inspecting = EditorContext::Inspecting::Node;
    }

    static void syncPositions(EditorContext& e) {
        const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) e.endCoalescing();

        const bool finishing = !dragging && !g_dragFrom.empty();
        const bool finishingSize = !dragging && !g_sizeFrom.empty();

        for (const Node& n : e.graph().nodes()) {
            const ImVec2 p = ed::GetNodePosition(encNode(n.id));
            if (p.x == FLT_MAX) continue;
            if (p.x == n.x && p.y == n.y) continue;

            if (dragging || finishing) {
                const bool known = std::any_of(
                    g_dragFrom.begin(), g_dragFrom.end(),
                    [&](const DragOrigin& d) { return d.id == n.id.v; });
                if (!known) g_dragFrom.push_back({n.id.v, n.x, n.y});
                e.placeNode(n.id, p.x, p.y);
            } else {
                e.moveNode(n.id, p.x, p.y);
            }
        }

        if (finishing) {
            std::vector<EditorContext::NodeMove> moves;
            moves.reserve(g_dragFrom.size());
            for (const DragOrigin& d : g_dragFrom)
                if (const Node* n = e.graph().node(NodeId{d.id}))
                    moves.push_back({NodeId{d.id}, d.x, d.y, n->x, n->y});
            e.moveNodes(moves);
            g_dragFrom.clear();
        }

        e.measured().clear();
        for (const Node& n : e.graph().nodes()) {
            const ImVec2 sz = ed::GetNodeSize(encNode(n.id));
            if (sz.x > 0 && sz.y > 0) e.measured()[n.id.v] = {sz.x, sz.y};
        }

        if (!dragging && !finishingSize) return;
        for (const Node& n : e.graph().nodes()) {
            if (!isComment(e, n)) continue;
            const ImVec2 sz = ed::GetNodeSize(encNode(n.id));
            if (sz.x <= 0 || sz.y <= 0) continue;
            if (std::abs(sz.x - n.w) < 1.0f && std::abs(sz.y - n.h) < 1.0f) continue;

            const bool known = std::any_of(
                g_sizeFrom.begin(), g_sizeFrom.end(),
                [&](const DragSize& d) { return d.id == n.id.v; });
            if (!known) g_sizeFrom.push_back({n.id.v, n.w, n.h});
            e.sizeNode(n.id, sz.x, sz.y);
        }

        if (finishingSize) {
            std::vector<EditorContext::NodeResize> sizes;
            sizes.reserve(g_sizeFrom.size());
            for (const DragSize& d : g_sizeFrom)
                if (const Node* n = e.graph().node(NodeId{d.id}))
                    sizes.push_back({NodeId{d.id}, d.w, d.h, n->w, n->h});
            e.resizeNodes(sizes);
            g_sizeFrom.clear();
        }
    }

    static void drawHoverTooltip(EditorContext& e, ed::NodeId hovered) {
        if (!hovered) return;
        if (ImGui::IsPopupOpen("canvas.add")) return;

        const Node* n = e.graph().node(decNode(hovered));
        if (!n) return;
        const NodeDesc* d = e.nodes.find(n->type);
        if (!d) {
            ImGui::SetTooltip("Unknown node type '%s'.\n"
                              "No provider defines it - the graph will not compile.",
                              n->type.c_str());
            return;
        }

        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);

        ImGui::TextUnformatted(d->display.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 146, 154, 255));
        ImGui::TextUnformatted(d->id.c_str());
        ImGui::PopStyleColor();

        if (!d->doc.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(d->doc.c_str());
        }

        bool anyIn = false, anyOut = false;
        for (const PinDesc& p : d->pins) {
            if (p.kind != PinKind::Data) continue;
            (p.dir == PinDir::In ? anyIn : anyOut) = true;
        }
        if (anyIn || anyOut) ImGui::Separator();
        for (const PinDesc& p : d->pins) {
            if (p.kind != PinKind::Data) continue;
            ImGui::PushStyleColor(ImGuiCol_Text, pinColor(e, p));
            ImGui::TextUnformatted(p.dir == PinDir::In ? "in " : "out");
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6);
            ImGui::Text("%s : %s%s", p.name.c_str(),
                        std::string(e.types.get(p.type).name).c_str(),
                        p.optional ? "  (optional)" : "");
        }
        if (d->pure) {
            ImGui::Separator();
            ImGui::TextDisabled("Pure - no execution pins; runs where its "
                                "result is used.");
        }

        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    static void makeVariableFromPending(EditorContext& e) {
        Graph& g = e.graph();

        VarDecl v;
        v.name = uniqueVarName(g);
        v.type = g_pending.type.valid()
                     ? std::string(e.types.get(g_pending.type).name)
                     : std::string();
        if (v.type.empty() || v.type == "any") v.type = "number";
        v.defaultValue = defaultFor(v.type);
        g.variables.push_back(v);
        commit(e);

        const NodeDesc* d =
            e.nodes.find(graphVarGetId(g.moduleName, v.name));
        if (!d) {
            e.note(EditorContext::NoteKind::Warning,
                   "save this graph before making variables from a wire");
            g_pending = {};
            return;
        }

        const NodeId made =
            e.addNode(*d, g_pending.canvasPos.x, g_pending.canvasPos.y);
        for (const PinDesc& p : d->pins) {
            if (p.kind != PinKind::Data) continue;
            if (p.dir == g_pending.fromDir) continue;
            const PinId lp = PinId::make(made, p.name);
            const bool pendingIsOut = g_pending.fromDir == PinDir::Out;
            e.connect(pendingIsOut ? g_pending.from : lp,
                      pendingIsOut ? lp : g_pending.from, PinKind::Data);
            break;
        }
        e.selection() = {made};
        g_pending = {};
    }

    static void drawContextMenus(EditorContext& e) {
        static bool addWasOpen = false;
        const bool addIsOpen = ImGui::IsPopupOpen("canvas.add");
        if (addWasOpen && !addIsOpen) g_pending = {};
        addWasOpen = addIsOpen;

        if (ed::ShowBackgroundContextMenu()) {
            g_pending = {};
            g_lastCanvasMouse = ed::ScreenToCanvas(ImGui::GetMousePos());
            ImGui::OpenPopup("canvas.add");
        }

        if (g_requestAddPopup) {
            g_requestAddPopup = false;
            ImGui::OpenPopup("canvas.add");
        }

        if (!ImGui::BeginPopup("canvas.add")) return;

        static char filter[128] = "";
        if (ImGui::IsWindowAppearing()) {
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##filter", "search nodes...", filter,
                                 sizeof(filter));

        if (g_pending.active)
            ImGui::TextDisabled("filtered to pins compatible with '%s'",
                                std::string(g_pending.from.pin.str()).c_str());

        if (g_pending.active && g_pending.kind == PinKind::Data) {
            if (ImGui::Button("Make Variable", ImVec2(-1, 0))) {
                makeVariableFromPending(e);
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Declare a variable of this type and wire it up");
        }

        ImGui::Separator();
        ImGui::BeginChild("list", ImVec2(360, 380));

        const bool searching = filter[0] != '\0';
        std::vector<std::string> prev;
        int shown = 0;

        auto openTo = [&](const std::vector<std::string>& segs, std::size_t depth) {
            std::string path;
            for (std::size_t i = 0; i < depth; ++i) {
                if (!path.empty()) path += '/';
                path += segs[i];
                if (!(searching || g_expanded.count(path))) return false;
            }
            return true;
        };

        const NodeDesc* best = nullptr;
        int bestScore = 0;
        for (const NodeDesc* dp : e.nodes.browseOrder()) {
            const int score = relevance(*dp, filter);
            if (score <= bestScore) continue;
            if (g_pending.active && !landingFor(e, *dp)) continue;
            bestScore = score;
            best = dp;
        }

        for (const NodeDesc* dp : e.nodes.browseOrder()) {
            const NodeDesc& d = *dp;
            if (relevance(d, filter) == 0) continue;

            const PinDesc* landing = g_pending.active ? landingFor(e, d) : nullptr;
            if (g_pending.active && !landing) continue;

            const std::vector<std::string> segs = splitCategory(d.category);

            std::size_t common = 0;
            while (common < segs.size() && common < prev.size()
                   && segs[common] == prev[common])
                ++common;

            for (std::size_t i = common; i < segs.size(); ++i) {
                if (!openTo(segs, i)) break;

                std::string path;
                for (std::size_t k = 0; k <= i; ++k) {
                    if (!path.empty()) path += '/';
                    path += segs[k];
                }
                const bool open = searching || g_expanded.count(path) != 0;

                const float ind = static_cast<float>(i) * 14.0f;
                if (ind > 0.0f) ImGui::Indent(ind);

                ImGui::PushID(path.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, headerColor(i));
                if (ImGui::Selectable(((open ? "- " : "+ ") + segs[i]).c_str(),
                                      false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                    if (g_expanded.count(path)) g_expanded.erase(path);
                    else                        g_expanded.insert(path);
                }
                ImGui::PopStyleColor();
                ImGui::PopID();

                if (ind > 0.0f) ImGui::Unindent(ind);
            }
            prev = segs;

            if (!openTo(segs, segs.size())) { ++shown; continue; }

            const float leafInd = static_cast<float>(segs.size()) * 14.0f;
            ImGui::Indent(leafInd);
            ImGui::PushID(d.id.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, roleColor(d));

            const bool isBest = (dp == best);
            if (isBest && searching) ImGui::SetScrollHereY(0.4f);

            const bool accept =
                ImGui::Selectable(d.display.c_str(), isBest)
                || (isBest && ImGui::IsKeyPressed(ImGuiKey_Enter));
            if (accept) {
                const ImVec2 at = g_pending.active ? g_pending.canvasPos
                                                   : g_lastCanvasMouse;
                const NodeId made = e.addNode(d, at.x, at.y);
                ed::SetNodePosition(encNode(made), at);

                if (landing) {
                    const PinId lp = PinId::make(made, landing->name);
                    const bool pendingIsOut = g_pending.fromDir == PinDir::Out;
                    e.connect(pendingIsOut ? g_pending.from : lp,
                              pendingIsOut ? lp : g_pending.from,
                              g_pending.kind);
                }
                g_pending = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !d.doc.empty())
                ImGui::SetTooltip("%s", d.doc.c_str());
            ImGui::PopID();
            ImGui::Unindent(leafInd);
            ++shown;
        }

        if (shown == 0) ImGui::TextDisabled("No matching nodes.");

        ImGui::EndChild();
        ImGui::EndPopup();
    }
};

class PalettePanel final : public IPanel {
public:
    std::string_view id() const override { return "palette"; }
    std::string_view title() const override { return "Palette"; }

    void draw(EditorContext& e) override {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "search...", filter_, sizeof(filter_));
        ImGui::Separator();

        std::vector<std::string> prev;
        for (const NodeDesc* dp : e.nodes.browseOrder()) {
            const NodeDesc& d = *dp;
            if (!fuzzyMatch(filter_, d.display) && !fuzzyMatch(filter_, d.id)
                && !fuzzyMatch(filter_, d.category))
                continue;

            const std::vector<std::string> segs = splitCategory(d.category);
            std::size_t common = 0;
            while (common < segs.size() && common < prev.size()
                   && segs[common] == prev[common])
                ++common;
            for (std::size_t i = common; i < segs.size(); ++i) {
                const float ind = static_cast<float>(i) * 14.0f;
                if (ind > 0.0f) ImGui::Indent(ind);
                ImGui::PushStyleColor(ImGuiCol_Text, headerColor(i));
                if (i == 0) ImGui::SeparatorText(segs[i].c_str());
                else        ImGui::TextUnformatted(segs[i].c_str());
                ImGui::PopStyleColor();
                if (ind > 0.0f) ImGui::Unindent(ind);
            }
            prev = segs;
            const float leafInd = static_cast<float>(segs.size()) * 14.0f;
            ImGui::Indent(leafInd);
            ImGui::PushStyleColor(ImGuiCol_Text, roleColor(d));
            ImGui::PushID(d.id.c_str());
            ImGui::Selectable(d.display.c_str());

            if (ImGui::BeginDragDropSource()) {
                const std::string& id = d.id;
                ImGui::SetDragDropPayload("LIME_NODE", id.c_str(),
                                          id.size() + 1);
                ImGui::TextUnformatted(d.display.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::IsItemHovered() && !d.doc.empty())
                ImGui::SetTooltip("%s", d.doc.c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                e.addNode(d, 0.0f, 0.0f);
            ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::Unindent(leafInd);
        }
    }

private:

    char filter_[128] = "";
};

class InspectorPanel final : public IPanel {
public:
    std::string_view id() const override { return "inspector"; }
    std::string_view title() const override { return "Inspector"; }

    void draw(EditorContext& e) override {
        if (e.showsEntity()) {
            drawEntityInspector(e);
            return;
        }

        const Node* n = e.graph().node(e.inspected());
        if (!n) {
            ImGui::TextDisabled("Nothing selected.");
            return;
        }

        const NodeDesc* d = e.nodes.find(n->type);
        ImGui::TextUnformatted(d ? d->display.c_str() : n->type.c_str());
        ImGui::TextDisabled("%s", n->type.c_str());

        if (d && d->emit == "struct:comment") {
            ImGui::Separator();
            char title[256];
            std::snprintf(title, sizeof(title), "%s", n->comment.c_str());
            ImGui::TextDisabled("Title");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##title", title, sizeof(title)))
                e.setComment(n->id, title);
            ImGui::TextDisabled("%.0f x %.0f", n->w, n->h);
            ImGui::TextWrapped("Drag the title to move everything inside. "
                               "Drag an edge to resize.");
            return;
        }
        if (d && !d->doc.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", d->doc.c_str());
        }
        ImGui::Separator();

        if (!d) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                               "Unknown node type - no provider defines it.");
            return;
        }

        for (const PinDesc& p : d->pins) {
            if (p.dir != PinDir::In || p.kind != PinKind::Data) continue;

            const PinId pid = PinId::make(n->id, p.name);
            if (e.graph().sourceOf(pid)) {
                ImGui::TextDisabled("%s  (wired)", p.name.c_str());
                continue;
            }

            std::string cur = p.defaultValue;
            for (const auto& [k, v] : n->values) if (k == p.name) cur = v;

            if (e.types.get(p.type).name == "Asset:Script") {
                drawScriptPicker(e, *n, p, cur);
                continue;
            }

            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText(("##" + p.name).c_str(), buf, sizeof(buf)))
                e.setValue(n->id, p.name, buf);
            ImGui::SameLine(0, 4);
            ImGui::TextDisabled("%s : %s", p.name.c_str(),
                                std::string(e.types.get(p.type).name).c_str());
        }

        if (d->emit == "raw" && !hasScript(*n)) {
            ImGui::SeparatorText("Lua body");
            char body[4096];
            std::snprintf(body, sizeof(body), "%s", n->rawBody.c_str());
            if (ImGui::InputTextMultiline("##raw", body, sizeof(body),
                                          ImVec2(-1, 160))) {
                const NodeId id = n->id;
                const std::string old = n->rawBody, next = body;
                e.apply({"Edit raw Lua",
                         [id, next](Graph& g) { if (Node* x = g.node(id)) x->rawBody = next; },
                         [id, old](Graph& g)  { if (Node* x = g.node(id)) x->rawBody = old; }});
            }
        }
    }
private:
    static bool hasScript(const Node& n) {
        for (const auto& [k, v] : n.values)
            if (k == "script" && !v.empty() && v != "\"\"") return true;
        return false;
    }

    void drawScriptPicker(EditorContext& e, const Node& n, const PinDesc& p,
                          const std::string& cur) {
        std::string shown = cur;
        if (shown.size() >= 2 && shown.front() == '"' && shown.back() == '"')
            shown = shown.substr(1, shown.size() - 2);
        if (shown.empty()) shown = "(none)";

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(("##" + p.name).c_str(), shown.c_str())) {
            if (ImGui::Selectable("(none)", cur.empty()))
                e.setValue(n.id, p.name, "");

            bool any = false;
            for (const std::string& f : e.project.luaFiles) {
                if (e.isGeneratedLua(f)) continue;
                any = true;
                std::string rel = f;
                for (char& c : rel)
                    if (c == '\\') c = '/';
                const std::size_t at = rel.find("content/");
                if (at != std::string::npos) rel = rel.substr(at + 8);

                const std::string stored = "\"" + rel + "\"";
                if (ImGui::Selectable(rel.c_str(), stored == cur))
                    e.setValue(n.id, p.name, stored);
            }
            if (!any) ImGui::TextDisabled("No scripts. File > New Lua Script...");
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("script");
    }

};

class LuaPanel final : public IPanel {
public:
    std::string_view id() const override { return "lua"; }
    std::string_view title() const override { return "Generated Lua"; }

    void draw(EditorContext& e) override {
        if (!e.previewErrors().empty()) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%zu error%s",
                               e.previewErrors().size(),
                               e.previewErrors().size() == 1 ? "" : "s");
            ImGui::Separator();
            int errIx = 0;
            for (const auto& [node, msg] : e.previewErrors()) {
                ImGui::PushID(errIx++);
                if (ImGui::Selectable(msg.c_str()) && node.valid()) {
                    e.selection() = {node};
                    e.inspected() = node;
                    e.inspecting = EditorContext::Inspecting::Node;
                }
                ImGui::PopID();
            }
            return;
        }

        if (e.previewLua().empty()) {
            ImGui::TextDisabled("Add an event node to produce code.");
            return;
        }

        if (e.previewGotos() > 0)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1),
                               "%d goto - this graph is irreducible",
                               e.previewGotos());

        std::set<int> hot;
        if (!e.selection().empty())
            for (const auto& [line, node] : e.previewMap().lines)
                if (e.isSelected(node)) hot.insert(line);

        int line = 1;
        std::size_t pos = 0;
        const std::string& src = e.previewLua();
        while (pos <= src.size()) {
            const std::size_t nl = src.find('\n', pos);
            const std::string text =
                src.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (hot.count(line))
                ImGui::TextColored(ImVec4(0.65f, 0.95f, 0.55f, 1), "%s", text.c_str());
            else
                ImGui::TextUnformatted(text.c_str());
            if (nl == std::string::npos) break;
            pos = nl + 1;
            ++line;
        }
    }
};

class ConsolePanel final : public IPanel {
public:
    std::string_view id() const override { return "console"; }
    std::string_view title() const override { return "Console"; }

    void draw(EditorContext& e) override {
        for (const std::string& l : e.console) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
};

}

namespace {

ed::EditorContext* ensureCanvas(GraphDoc& d) {
    if (d.canvas) return static_cast<ed::EditorContext*>(d.canvas);
    ed::Config cfg;
    cfg.SettingsFile = nullptr;
    ed::EditorContext* ctx = ed::CreateEditor(&cfg);
    ed::SetCurrentEditor(ctx);
    applyCanvasStyle();
    ed::SetCurrentEditor(nullptr);
    d.canvas = ctx;
    return ctx;
}

CanvasPanel g_canvas;

}

void setGraphDockId(unsigned int dockId) { g_graphDock = dockId; }
unsigned int graphDockId() { return g_graphDock; }

void releaseCanvas(GraphDoc& d) {
    if (!d.canvas) return;
    ed::DestroyEditor(static_cast<ed::EditorContext*>(d.canvas));
    d.canvas = nullptr;
    d.placed.clear();
}

namespace {

constexpr float kVarsWidth = 230.0f;

void commit(EditorContext& e);
std::string sanitise(std::string s);
std::string uniqueVarName(const Graph& g);
std::string defaultFor(const std::string& type);

int varUses(const Graph& g, const std::string& name) {
    const std::string get = graphVarGetId(g.moduleName, name);
    const std::string set = graphVarSetId(g.moduleName, name);
    int n = 0;
    for (const Node& nd : g.nodes())
        if (nd.type == get || nd.type == set) ++n;
    return n;
}

void drawVariables(EditorContext& e) {
    Graph& g = e.graph();

    const float addW = ImGui::CalcTextSize("+ Add").x
                       + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float lineW = ImGui::GetContentRegionAvail().x;
    const bool shown =
        ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen
                                                 | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::SameLine(lineW - addW);
    if (ImGui::SmallButton("+ Add")) {
        VarDecl v;
        v.name = uniqueVarName(g);
        v.type = "number";
        v.defaultValue = "0";
        g.variables.push_back(std::move(v));
        commit(e);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Declare a variable this graph owns. Get and Set "
                          "nodes for it appear in the node search.");
    if (!shown) return;

    if (g.variables.empty()) {
        ImGui::TextDisabled("No variables");
        return;
    }

    static std::string dropName;
    static int         dropIndex = -1;
    static int         dropUses = 0;
    static bool        askDrop = false;

    int removeAt = -1;
    for (std::size_t i = 0; i < g.variables.size(); ++i) {
        VarDecl& v = g.variables[i];
        ImGui::PushID(static_cast<int>(i));

        const bool open = ImGui::TreeNodeEx(
            "##row", ImGuiTreeNodeFlags_SpanAvailWidth
                         | ImGuiTreeNodeFlags_AllowOverlap,
            "%s", v.name.c_str());
        if (ImGui::BeginPopupContextItem("vctx")) {
            if (ImGui::MenuItem("Delete")) {
                const int uses = varUses(g, v.name);
                if (uses > 0) {
                    dropName = v.name;
                    dropIndex = static_cast<int>(i);
                    dropUses = uses;
                    askDrop = true;
                } else {
                    removeAt = static_cast<int>(i);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%s", v.type.c_str());

        if (!open) {
            ImGui::PopID();
            continue;
        }

        char name[96];
        std::snprintf(name, sizeof(name), "%s", v.name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##n", name, sizeof(name))) v.name = name;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            v.name = sanitise(name);
            commit(e);
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::BeginCombo("##t", v.type.c_str())) {
            for (const char* t : {"number", "string", "boolean", "Vec2",
                                  "Vec3", "table", "any"}) {
                if (ImGui::Selectable(t, v.type == t)) {
                    v.type = t;
                    v.defaultValue = defaultFor(t);
                    commit(e);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        char def[128];
        std::snprintf(def, sizeof(def), "%s", v.defaultValue.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##d", "default", def, sizeof(def)))
            v.defaultValue = def;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(e);

        ImGui::TreePop();
        ImGui::PopID();
    }

    if (askDrop) {
        ImGui::OpenPopup("Remove variable");
        askDrop = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 0), ImVec2(400, FLT_MAX));
    if (ImGui::BeginPopupModal("Remove variable", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s is used by %d node%s in this graph.",
                           dropName.c_str(), dropUses,
                           dropUses == 1 ? "" : "s");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.89f, 0.66f, 0.29f, 1.0f),
                           "They will report an error until you delete them");
        ImGui::Separator();
        if (ImGui::Button("Remove anyway", ImVec2(140, 0))) {
            removeAt = dropIndex;
            dropIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            dropIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (removeAt >= 0 && removeAt < static_cast<int>(g.variables.size())) {
        g.variables.erase(g.variables.begin() + removeAt);
        commit(e);
    }
}

void commit(EditorContext& e) {
    e.dirty() = true;
    e.previewStale() = true;
    if (!e.filePath().empty()) e.saveAndCompile();
    e.rebuildGraphFunctions();
}

std::string sanitise(std::string s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
    if (out.empty()) out = "var";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(0, "v");
    return out;
}

std::string uniqueVarName(const Graph& g) {
    for (int n = 1;; ++n) {
        const std::string candidate =
            n == 1 ? std::string("NewVar") : "NewVar" + std::to_string(n);
        bool taken = false;
        for (const VarDecl& v : g.variables)
            if (v.name == candidate) { taken = true; break; }
        if (!taken) return candidate;
    }
}

std::string defaultFor(const std::string& type) {
    if (type == "number")  return "0";
    if (type == "string")  return "\"\"";
    if (type == "boolean") return "false";
    if (type == "Vec2")    return "Vec2.new(0, 0)";
    if (type == "Vec3")    return "Vec3.new(0, 0, 0)";
    if (type == "table")   return "{}";
    return "";
}

}

void drawGraphWindows(EditorContext& e) {
    std::size_t toClose = e.docs.size();
    std::size_t focused = e.docs.size();
    std::size_t visible = e.docs.size();

    for (std::size_t i = 0; i < e.docs.size(); ++i) {
        GraphDoc& d = *e.docs[i];
        ensureCanvas(d);

        if (g_graphDock) ImGui::SetNextWindowDockID(g_graphDock, ImGuiCond_FirstUseEver);

        bool open = true;
        const std::string title = d.windowTitle();
        static std::map<std::uint32_t, bool> varsOpen;
        const std::string label =
            d.dirty ? d.displayName() + " *###limedoc" + std::to_string(d.id)
                    : title;

        if (const ImGuiWindow* w = ImGui::FindWindowByName(label.c_str()))
            if (w->DockId) g_graphDock = w->DockId;

        if (ImGui::Begin(label.c_str(), &open)) {
            visible = i;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
                focused = i;
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
                statusSetArea("Graph");

            e.activeDoc = i;
            g_doc = &d;

            if (d.isText()) {
                drawTextDocument(e);
                g_doc = nullptr;
                ImGui::End();
                if (!open) toClose = i;
                continue;
            }

            bool& showVars = varsOpen.try_emplace(d.id, true).first->second;
            if (showVars) {
                ImGui::BeginChild("vars", ImVec2(kVarsWidth, 0),
                                  ImGuiChildFlags_Border);
                if (ImGui::SmallButton("<")) showVars = false;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide variables");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", d.displayName().c_str());
                ImGui::Separator();
                drawVariables(e);
                ImGui::EndChild();
                ImGui::SameLine();
            } else {
                if (ImGui::SmallButton("Vars")) showVars = true;
                ImGui::SameLine();
            }

            ImGui::BeginChild("canvas", ImVec2(0, 0));
            g_canvas.draw(e);
            ImGui::EndChild();

            g_doc = nullptr;
        }
        ImGui::End();

        if (!open) toClose = i;
    }

    if (focused < e.docs.size())      e.activeDoc = focused;
    else if (visible < e.docs.size()) e.activeDoc = visible;

    if (toClose < e.docs.size()) {
        if (e.docs[toClose]->dirty && !e.docs[toClose]->filePath.empty()) {
            const std::size_t was = e.activeDoc;
            e.activeDoc = toClose;
            e.saveAndCompile();
            e.activeDoc = was;
        }
        e.closeDoc(toClose);
    }
}

void canvasInit() {}

void canvasShutdown() {}

std::vector<std::unique_ptr<IPanel>> makeCorePanels() {
    std::vector<std::unique_ptr<IPanel>> v;
    for (auto& p : makeAssetPanels()) v.push_back(std::move(p));
    v.push_back(std::make_unique<PalettePanel>());
    for (auto& p : makeScenePanels()) v.push_back(std::move(p));
    v.push_back(std::make_unique<InspectorPanel>());
    for (auto& p : makeViewportPanels()) v.push_back(std::move(p));
    v.push_back(std::make_unique<ConsolePanel>());
    v.push_back(std::make_unique<LuaPanel>());
    return v;
}

}
