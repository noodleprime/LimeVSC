#include "api/data_provider.h"
#include "api/graphfn_provider.h"
#include "api/luals_provider.h"
#include "app/editor.h"
#include "app/settings.h"
#include "project/project.h"
#include "ui/panels.h"
#include "scene/component_provider.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <d3d11.h>
#include <shellapi.h>
#include <tchar.h>
#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace lime;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool                    g_resize = false;
const std::pair<const char*, const char*> kDockSibling[] = {
    {"Hierarchy", "Content"},
    {"Generated Lua", "Console"},
    {"Viewport",  nullptr},
};

ImGuiID homeDockFor(const std::string& title) {
    for (const auto& [panel, sibling] : kDockSibling) {
        if (title != panel) continue;
        if (!sibling) return graphDockId();
        const ImGuiWindow* w = ImGui::FindWindowByName(sibling);
        return w ? w->DockId : 0;
    }
    return 0;
}
UINT                    g_resizeW = 0, g_resizeH = 0;

void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void releaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;

    createRenderTarget();
    return true;
}

void destroyDevice() {
    releaseRenderTarget();
    if (g_swap)    { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resize = true;
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.SeparatorTextBorderSize = 1.0f;

    s.AntiAliasedLinesUseTex = false;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.11f, 0.12f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.11f, 0.12f, 0.13f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(0.20f, 0.22f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.16f, 0.17f, 0.19f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.14f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.20f, 0.24f, 0.22f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.22f, 0.28f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.28f, 0.36f, 0.30f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.20f, 0.24f, 0.22f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.55f, 0.82f, 0.45f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.55f, 0.82f, 0.45f, 1.00f);
}

bool chordPressed(const std::string& key) {
    if (key.empty()) return false;
    std::string base = key;
    bool ctrl = false, shift = false;
    if (base.rfind("Ctrl+", 0) == 0)  { ctrl = true;  base = base.substr(5); }
    if (base.rfind("Shift+", 0) == 0) { shift = true; base = base.substr(6); }
    if (ctrl != ImGui::GetIO().KeyCtrl) return false;
    if (shift != ImGui::GetIO().KeyShift) return false;

    if (base == "Delete") return ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    if (base.size() >= 2 && base[0] == 'F'
        && std::isdigit(static_cast<unsigned char>(base[1]))) {
        const int n = std::atoi(base.c_str() + 1);
        if (n >= 1 && n <= 12)
            return ImGui::IsKeyPressed(
                static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1)), false);
        return false;
    }
    if (base.size() == 1 && base[0] >= 'A' && base[0] <= 'Z') {
        const auto k = static_cast<ImGuiKey>(ImGuiKey_A + (base[0] - 'A'));
        return ImGui::IsKeyPressed(k, false);
    }
    return false;
}

std::string dataDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p = std::filesystem::path(buf).parent_path() / "data";
    if (std::filesystem::exists(p)) return p.string();
    return LIMEVSC_DATA_DIR;
}

std::string findLimeXApi() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p = std::filesystem::path(buf).parent_path();
    for (int up = 0; up < 6 && !p.empty(); ++up, p = p.parent_path()) {
        const std::filesystem::path api =
            p / "LimeX" / "LimeEngine" / "api" / "Lime.lua";
        if (std::filesystem::exists(api)) return api.parent_path().string();
    }
    return {};
}

void loadCatalog(EditorContext& ed) {
    const std::string dir = dataDir();
    ed.nodes = NodeRegistry{};
    ed.types.loadFile(dir + "/core.limetypes", ed.diag);

    if (const std::string api = findLimeXApi(); !api.empty()) {
        ed.nodes.addProvider(std::make_unique<LuaLSProvider>(
            api + "/Lime.lua", api + "/Enums.lua", kPriorityGenerated));
        ed.log("engine API: " + api);
    } else {
        ed.log("no LimeX checkout found - core nodes only");
    }

    ed.nodes.addProvider(std::make_unique<DataFileProvider>(
        dir + "/core.limenodes", "core", kPriorityCore));

    if (ed.project.valid())
        ed.nodes.addProvider(std::make_unique<GraphFnProvider>(
            ed.project.root, kPriorityOverrides));
    const std::string overrides = dir + "/overrides.limenodes";
    if (std::filesystem::exists(overrides))
        ed.nodes.addProvider(std::make_unique<DataFileProvider>(
            overrides, "overrides", kPriorityOverrides));

    ed.nodes.rebuild(ed.types, ed.diag);

    ed.components = ComponentRegistry{};
    ed.components.addProvider(std::make_unique<ComponentFileProvider>(
        dir + "/core.limecomponents", "core", kComponentPriorityCore));
    if (ed.project.valid()) {
        const std::string projComps = ed.project.root + "/components.limecomponents";
        if (std::filesystem::exists(projComps))
            ed.components.addProvider(std::make_unique<ComponentFileProvider>(
                projComps, "project", kComponentPriorityProject));
    }
    const std::string compOverrides = dir + "/overrides.limecomponents";
    if (std::filesystem::exists(compOverrides))
        ed.components.addProvider(std::make_unique<ComponentFileProvider>(
            compOverrides, "overrides", kComponentPriorityOverrides));
    ed.components.rebuild(ed.types, ed.diag);

    ed.assetTypes = AssetTypeRegistry{};
    ed.assetTypes.loadFile(dir + "/core.limeassets", ed.diag);
    if (ed.project.valid()) {
        const std::string projAssets = ed.project.root + "/assets.limeassets";
        if (std::filesystem::exists(projAssets))
            ed.assetTypes.loadFile(projAssets, ed.diag);
    }
    ed.rescanAssets();

    for (const Diagnostic& d : ed.diag.all())
        ed.log((d.severity == Severity::Error ? "error: " : "warning: ") + d.message);
    ed.log("catalog: " + std::to_string(ed.nodes.all().size()) + " nodes, "
           + std::to_string(ed.components.all().size()) + " components, "
           + std::to_string(ed.assets.size()) + " assets, "
           + std::to_string(ed.types.size()) + " types");
}

std::map<std::string, bool> g_visible;

bool& panelVisible(const std::string& title) {
    const auto it = g_visible.find(title);
    if (it != g_visible.end()) return it->second;
    return g_visible.emplace(title, true).first->second;
}

void drawLoading(EditorContext& ed) {
    if (!ed.loading.active()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(420, FLT_MAX));
    ImGui::OpenPopup("Loading##modal");

    if (ImGui::BeginPopupModal("Loading##modal", nullptr,
                               ImGuiWindowFlags_NoTitleBar
                                   | ImGuiWindowFlags_NoResize
                                   | ImGuiWindowFlags_NoMove
                                   | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ed.loading.label().c_str());
        ImGui::ProgressBar(ed.loading.fraction(), ImVec2(-1, 0));

        ImGui::TextDisabled("%s", ed.loading.detail().empty()
                                      ? " "
                                      : ed.loading.detail().c_str());
        ImGui::TextDisabled("Step %zu of %zu", ed.loading.index() + 1,
                            ed.loading.count());
        ImGui::EndPopup();
    }
}

bool        g_showSettings = false;
std::string g_pendingRecent;

bool g_showWelcome = true;

void drawWelcome(EditorContext& ed) {
    if (!g_showWelcome) return;
    if (ed.project.valid()) {
        g_showWelcome = false;
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    if (!ImGui::Begin("##welcome", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking
                          | ImGuiWindowFlags_NoSavedSettings
                          | ImGuiWindowFlags_NoMove
                          | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    constexpr float kColumns = 860.0f;
    const float slack = ImGui::GetContentRegionAvail().x - kColumns;
    if (slack > 0.0f) ImGui::Indent(slack * 0.5f);
    const float top = ImGui::GetContentRegionAvail().y;
    if (top > 620.0f) ImGui::Dummy(ImVec2(0, (top - 620.0f) * 0.35f));

    constexpr float kLogo = 56.0f;
    const ImTextureID logo = statusLogo();
    if (logo) {
        ImGui::Image(logo, ImVec2(kLogo, kLogo));
        ImGui::SameLine(0, 16);
    }
    ImGui::BeginGroup();
    if (logo) {
        const float lead = (kLogo - ImGui::GetTextLineHeight()) * 0.5f
                           - ImGui::GetStyle().ItemSpacing.y;
        if (lead > 0.0f) ImGui::Dummy(ImVec2(0, lead));
    }
    ImGui::TextUnformatted("LimeVSC");
    ImGui::SameLine(0, 10);
    ImGui::TextDisabled("v%s", LIMEVSC_VERSION);
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 20));

    const float gap = ImGui::GetStyle().ItemSpacing.x * 3.0f;
    const float colW =
        ((slack > 0.0f ? kColumns : ImGui::GetContentRegionAvail().x) - gap)
        * 0.5f;

    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(colW);
    if (ImGui::Button("New Project...", ImVec2(colW, 40))) {
        ed.commands.invoke("project.new", ed);
        g_showWelcome = false;
    }
    ImGui::TextDisabled("Start something from scratch");
    ImGui::Spacing();
    if (ImGui::Button("Open Project...", ImVec2(colW, 40))) {
        ed.commands.invoke("project.open", ed);
        g_showWelcome = false;
    }
    ImGui::TextDisabled("Point at a folder you already have");
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);

    ImGui::BeginGroup();
    if (ed.settings.recentProjects.empty()) {
        ImGui::TextDisabled("Nothing yet.");
    } else {
        ImGui::BeginChild("recent", ImVec2(colW, 340), ImGuiChildFlags_None);
        std::string pick;
        for (const std::string& r : ed.settings.recentProjects) {
            std::error_code ec;
            const bool there = std::filesystem::exists(r, ec);
            const std::string name =
                std::filesystem::path(r).filename().string();

            ImGui::PushID(r.c_str());
            if (!there)
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(name.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick)
                && there)
                pick = r;
            if (!there) ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(there ? r.c_str() : "missing");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!pick.empty()) {
            ed.queueOpenProject(pick);
            g_showWelcome = false;
        }
    }
    ImGui::EndGroup();

    ImGui::End();
    ImGui::PopStyleVar();
}

void drawNewProject(EditorContext& ed) {
    static char        name[128] = "";
    static std::string where;
    static bool        engine = true;
    static bool        script = false;
    static bool        primed = false;

    if (ed.showNewProject) {
        ed.showNewProject = false;
        std::snprintf(name, sizeof(name), "%s", "MyGame");
        where = ed.settings.projectsDir.empty()
                    ? AppSettings::defaultProjectsDir()
                    : ed.settings.projectsDir;
        engine = true;
        script = false;
        primed = true;
        ImGui::OpenPopup("New Project");
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 0), ImVec2(560, FLT_MAX));
    if (!ImGui::BeginPopupModal("New Project", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::SeparatorText("Name");
    ImGui::SetNextItemWidth(-1);
    if (primed) {
        ImGui::SetKeyboardFocusHere();
        primed = false;
    }
    ImGui::InputText("##name", name, sizeof(name));

    ImGui::Spacing();
    ImGui::SeparatorText("Location");
    char dir[512];
    std::snprintf(dir, sizeof(dir), "%s", where.c_str());
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##where", dir, sizeof(dir))) where = dir;
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(80, 0))) {
        const std::string picked =
            pickFolder("Where should the project live?", where);
        if (!picked.empty()) where = picked;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Mode");
    if (ImGui::RadioButton("Engine", engine)) engine = true;
    ImGui::TextDisabled(
        "Standard modern engine experience with systems provided");
    ImGui::Spacing();
    if (ImGui::RadioButton("Framework", !engine)) engine = false;
    ImGui::TextDisabled("Barebones with just the framework of Lime");

    ImGui::Spacing();
    ImGui::SeparatorText("Main File");
    if (ImGui::RadioButton("Graph", !script)) script = false;
    ImGui::TextDisabled("Start from a .lime graph");
    ImGui::Spacing();
    if (ImGui::RadioButton("Script", script)) script = true;
    ImGui::TextDisabled("Start from a plain .lua script");

    std::string trimmed(name);
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.pop_back();

    std::filesystem::path target;
    if (!trimmed.empty() && !where.empty())
        target = std::filesystem::path(where) / trimmed;

    std::string problem;
    if (trimmed.empty()) {
        problem = "Give the project a name.";
    } else if (where.empty()) {
        problem = "Choose where it should live.";
    } else if (trimmed.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        problem = "A name cannot contain \\ / : * ? \" < > or |";
    } else {
        std::error_code ec;
        if (std::filesystem::exists(target, ec)
            && !std::filesystem::is_empty(target, ec))
            problem = "That folder already exists and is not empty.";
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (!target.empty())
        ImGui::TextDisabled("%s", target.string().c_str());
    else
        ImGui::TextDisabled(" ");

    if (!problem.empty())
        ImGui::TextColored(ImVec4(0.89f, 0.36f, 0.33f, 1.0f), "%s",
                           problem.c_str());
    else
        ImGui::TextDisabled(" ");

    ImGui::BeginDisabled(!problem.empty());
    if (ImGui::Button("Create", ImVec2(120, 0))) {
        ed.createProjectAt(target.string(),
                           engine ? ProjectMode::Engine : ProjectMode::Framework,
                           script);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void drawSettings(EditorContext& ed) {
    if (g_showSettings) {
        ImGui::OpenPopup("Settings");
        g_showSettings = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 0), ImVec2(560, FLT_MAX));
    if (!ImGui::BeginPopupModal("Settings", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::SeparatorText("Projects");
    char dir[512];
    std::snprintf(dir, sizeof(dir), "%s", ed.settings.projectsDir.c_str());
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##dir", dir, sizeof(dir)))
        ed.settings.projectsDir = dir;
    ImGui::SameLine();
    if (ImGui::Button("Default", ImVec2(80, 0)))
        ed.settings.projectsDir = AppSettings::defaultProjectsDir();

    ImGui::Spacing();
    ImGui::SeparatorText("Lua editing");
    ImGui::Checkbox("Open .lua in an external editor",
                    &ed.settings.useExternalEditor);
    ImGui::BeginDisabled(!ed.settings.useExternalEditor);

    const std::vector<AppSettings::EditorChoice> editors =
        AppSettings::knownEditors();
    const int match = ed.settings.matchKnownEditor();
    const std::string preview =
        match >= 0 ? editors[static_cast<std::size_t>(match)].name
                   : (ed.settings.externalEditor.empty() ? "Choose..." : "Custom");

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##editor", preview.c_str())) {
        for (std::size_t i = 0; i < editors.size(); ++i) {
            const bool on = match == static_cast<int>(i);
            if (ImGui::Selectable(editors[i].name, on))
                ed.settings.externalEditor = editors[i].command;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", editors[i].command);
            if (on) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Custom...", match < 0))
            ed.settings.externalEditor.clear();
        ImGui::EndCombo();
    }

    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "%s", ed.settings.externalEditor.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##cmd", "command to run", cmd, sizeof(cmd)))
        ed.settings.externalEditor = cmd;
    ImGui::TextDisabled("The file is appended, or put where you write {file}.");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText("Undo");
    int steps = static_cast<int>(ed.settings.undoLimit);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##undo", &steps, 1, 10,
                        ImGuiInputTextFlags_CharsDecimal)) {
        steps = std::clamp(steps, 0, static_cast<int>(AppSettings::kMaxUndoLimit));
        ed.settings.undoLimit = static_cast<std::size_t>(steps);
        ed.trimHistory();
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(120, 0))) {
        Diagnostics d;
        ed.settings.save(d);
        for (const Diagnostic& x : d.all()) ed.log(x.message);
        ed.log("settings saved to " + AppSettings::filePath());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        Diagnostics d;
        ed.settings.load(d);
        ed.trimHistory();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void drawMenuBar(EditorContext& ed, bool& running,
                 const std::vector<std::unique_ptr<IPanel>>& panels) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        for (const Command& c : ed.commands.all()) {
            if (c.category != "File") continue;
            const bool on = !c.enabled || c.enabled(ed);
            if (ImGui::MenuItem(c.title.c_str(), c.defaultKey.c_str(), false, on))
                ed.commands.invoke(c.id, ed);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Project...")) ed.commands.invoke("project.new", ed);
        if (ImGui::MenuItem("Open Project...", "Ctrl+Shift+O"))
            ed.commands.invoke("project.open", ed);
        if (ImGui::BeginMenu("Recent Projects",
                             !ed.settings.recentProjects.empty())) {
            const std::vector<std::string> recent = ed.settings.recentProjects;
            for (const std::string& r : recent) {
                const bool there = std::filesystem::exists(r);
                if (ImGui::MenuItem(r.c_str(), nullptr, false, there))
                    g_pendingRecent = r;
                if (!there && ImGui::IsItemHovered())
                    ImGui::SetTooltip("This folder is gone.");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear")) {
                ed.settings.recentProjects.clear();
                Diagnostics d;
                ed.settings.save(d);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) g_showSettings = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) running = false;
        ImGui::EndMenu();
    }

    for (const char* cat : {"Edit", "Graph", "Project"}) {
        if (!ImGui::BeginMenu(cat)) continue;
        for (const Command& c : ed.commands.all()) {
            if (c.category != cat) continue;
            const bool on = !c.enabled || c.enabled(ed);
            if (ImGui::MenuItem(c.title.c_str(), c.defaultKey.c_str(), false, on))
                ed.commands.invoke(c.id, ed);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        for (const auto& p : panels) {
            if (!p->availableIn(ed.project.isEngine())) continue;
            const std::string title(p->title());
            ImGui::MenuItem(title.c_str(), nullptr, &panelVisible(title));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show All")) {
            for (auto& [k, v] : g_visible) v = true;
        }
        ImGui::EndMenu();
    }

    const float right = ImGui::GetWindowWidth() - 320.0f;
    ImGui::SameLine(right > 0 ? right : 0);
    ImGui::TextDisabled("[%s] %s%s | %zu nodes | %zu links",
                        projectModeName(ed.project.mode),
                        ed.filePath().empty() ? "<unsaved>"
                                            : std::filesystem::path(ed.filePath())
                                                  .filename().string().c_str(),
                        ed.dirty() ? " *" : "",
                        ed.graph().nodes().size(), ed.graph().links().size());
    ImGui::EndMainMenuBar();
}

}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc, 0, 0, inst,
                   nullptr, nullptr, nullptr, nullptr, L"LimeVSC", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"LimeVSC", WS_OVERLAPPEDWINDOW,
                              80, 60, 1600, 950, nullptr, nullptr, inst, nullptr);
    if (!createDevice(hwnd)) {
        destroyDevice();
        UnregisterClassW(wc.lpszClassName, inst);
        MessageBoxW(nullptr, L"Failed to create a D3D11 device.", L"LimeVSC",
                    MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    static std::string iniPath;
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        iniPath = (std::filesystem::path(buf).parent_path()
                   / "limevsc-layout-v7.ini").string();
        io.IniFilename = iniPath.c_str();
    }
    bool buildLayout = !std::filesystem::exists(iniPath);

    applyTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    canvasInit();

    EditorContext ed;
    ed.rebuildCatalog = [](EditorContext& e) { loadCatalog(e); };
    ed.releaseDocCanvas = [](GraphDoc& d) { releaseCanvas(d); };
    ed.promptSavePath = [](EditorContext& e) {
        if (const Command* c = e.commands.find("file.saveAs")) c->run(e);
    };
    ed.settings.load(ed.diag);
    if (ed.settings.projectsDir.empty())
        ed.settings.projectsDir = AppSettings::defaultProjectsDir();

    loadCatalog(ed);
    registerCoreCommands(ed.commands);
    registerProjectCommands(ed.commands);

    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
        if (argv && argc > 0) {
            ed.openDoc(std::filesystem::path(argv[0]).string());
        } else {
            ed.newGraph("",  false);
            ed.dirty() = false;
        }
        if (argv) LocalFree(argv);
    }

    viewportInit(g_device, g_context);
    statusInitLogo(g_device);
    std::vector<std::unique_ptr<IPanel>> panels = makeCorePanels();

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_resize) {
            releaseRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
            g_resize = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::GetCurrentContext()->DimBgRatio =
            ImGui::GetTopMostPopupModal() != nullptr ? 1.0f : 0.0f;

        drawStatusBar(ed);
        drawNotes(ed);

        const ImGuiID dockspace =
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        if (buildLayout) {
            buildLayout = false;
            ImGui::DockBuilderRemoveNode(dockspace);
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace,
                                          ImGui::GetMainViewport()->WorkSize);
            ImGuiID centre = dockspace;
            const ImGuiID left =
                ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.16f, nullptr, &centre);
            const ImGuiID right =
                ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.30f, nullptr, &centre);
            const ImGuiID bottom =
                ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.22f, nullptr, &centre);

            ImGuiID leftLower = left;
            const ImGuiID leftUpper = ImGui::DockBuilderSplitNode(
                left, ImGuiDir_Up, 0.40f, nullptr, &leftLower);

            ImGui::DockBuilderDockWindow("Hierarchy", leftUpper);
            ImGui::DockBuilderDockWindow("Palette", leftLower);
            ImGui::DockBuilderDockWindow("Content", leftLower);
            ImGui::DockBuilderDockWindow("Viewport", centre);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Console", bottom);
            ImGui::DockBuilderDockWindow("Generated Lua", bottom);
            ImGui::DockBuilderFinish(dockspace);

            setGraphDockId(centre);

        }

        if (ed.previewStale()) {
            ed.previewStale() = false;
            Diagnostics cd;
            const CompileResult r = compileGraph(
                ed.graph(), ed.nodes, ed.types, ed.emitters,
                ed.filePath().empty()
                    ? std::string("untitled.lime")
                    : std::filesystem::path(ed.filePath()).filename().string(),
                cd);
            if (r.ok) {
                ed.previewLua() = r.lua;
                ed.previewMap() = r.map;
                ed.previewGotos() = r.gotoCount;
                ed.previewErrors().clear();
            } else {
                ed.previewErrors().clear();
                for (const Diagnostic& d : cd.all())
                    if (d.severity == Severity::Error)
                        ed.previewErrors().push_back({d.node, d.message});
            }
        }

        drawMenuBar(ed, running, panels);
        drawSettings(ed);
        drawNewProject(ed);
        drawWelcome(ed);
        drawLoading(ed);
        if (!g_pendingRecent.empty()) {
            ed.queueOpenProject(g_pendingRecent);
            g_pendingRecent.clear();
        }

        if (!ImGui::GetIO().WantTextInput) {
            for (const Command& c : ed.commands.all())
                if (chordPressed(c.defaultKey)) { ed.commands.invoke(c.id, ed); break; }
        }

        for (auto& p : panels) {
            if (!p->availableIn(ed.project.isEngine())) continue;
            const std::string title(p->title());

            bool& open = panelVisible(title);
            if (!open) continue;

            if (const ImGuiID home = homeDockFor(title))
                ImGui::SetNextWindowDockID(home, ImGuiCond_FirstUseEver);

            if (ImGui::Begin(title.c_str(), &open)) {
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
                    statusSetArea(title.c_str());
                p->draw(ed);
            }
            ImGui::End();
        }

        drawGraphWindows(ed);

        ed.loading.step();

        ImGui::Render();
        const float clear[4] = {0.06f, 0.07f, 0.08f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    viewportShutdown();
    canvasShutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
