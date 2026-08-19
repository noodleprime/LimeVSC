#include "ui/panels.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace lime {
namespace {

enum class Tok { Plain, Keyword, String, Number, Comment, Ident };

const char* const kKeywords[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
    "then", "true", "until", "while", "self"};

const ImU32 kTokColor[] = {
    IM_COL32(216, 220, 226, 255),
    IM_COL32(198, 140, 232, 255),
    IM_COL32(160, 205, 130, 255),
    IM_COL32(226, 170, 110, 255),
    IM_COL32(120, 130, 142, 255),
    IM_COL32(120, 180, 230, 255),
};

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isKeyword(std::string_view w) {
    for (const char* k : kKeywords)
        if (w == k) return true;
    return false;
}

struct Span {
    std::size_t begin = 0, end = 0;
    Tok kind = Tok::Plain;
};

void lexLine(std::string_view line, int& longBracket, std::vector<Span>& out) {
    out.clear();
    std::size_t i = 0;

    if (longBracket >= 0) {
        const std::string close = "]" + std::string(static_cast<std::size_t>(longBracket), '=') + "]";
        const std::size_t at = line.find(close);
        if (at == std::string_view::npos) {
            out.push_back({0, line.size(), Tok::Comment});
            return;
        }
        out.push_back({0, at + close.size(), Tok::Comment});
        i = at + close.size();
        longBracket = -1;
    }

    while (i < line.size()) {
        const char c = line[i];

        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            std::size_t j = i + 2;
            if (j < line.size() && line[j] == '[') {
                std::size_t eq = j + 1;
                while (eq < line.size() && line[eq] == '=') ++eq;
                if (eq < line.size() && line[eq] == '[') {
                    longBracket = static_cast<int>(eq - j - 1);
                    const std::string close =
                        "]" + std::string(static_cast<std::size_t>(longBracket), '=') + "]";
                    const std::size_t at = line.find(close, eq + 1);
                    if (at == std::string_view::npos) {
                        out.push_back({i, line.size(), Tok::Comment});
                        return;
                    }
                    out.push_back({i, at + close.size(), Tok::Comment});
                    i = at + close.size();
                    longBracket = -1;
                    continue;
                }
            }
            out.push_back({i, line.size(), Tok::Comment});
            return;
        }

        if (c == '"' || c == '\'') {
            const std::size_t start = i++;
            while (i < line.size()) {
                if (line[i] == '\\') { i += 2; continue; }
                if (line[i] == c) { ++i; break; }
                ++i;
            }
            out.push_back({start, std::min(i, line.size()), Tok::String});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = i;
            while (i < line.size()
                   && (std::isalnum(static_cast<unsigned char>(line[i])) != 0
                       || line[i] == '.'))
                ++i;
            out.push_back({start, i, Tok::Number});
            continue;
        }

        if (isIdentChar(c) && !std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = i;
            while (i < line.size() && isIdentChar(line[i])) ++i;
            const std::string_view word = line.substr(start, i - start);
            Tok kind = Tok::Plain;
            if (isKeyword(word)) {
                kind = Tok::Keyword;
            } else {
                std::size_t k = i;
                while (k < line.size() && line[k] == ' ') ++k;
                const bool call = k < line.size() && (line[k] == '(' || line[k] == '"');
                const bool field = start > 0 && (line[start - 1] == '.'
                                                 || line[start - 1] == ':');
                if (call || field) kind = Tok::Ident;
            }
            out.push_back({start, i, kind});
            continue;
        }
        ++i;
    }
}

int g_lineCount = 1;

void countLines(const std::string& text) {
    int n = 1;
    for (char c : text)
        if (c == '\n') ++n;
    g_lineCount = n;
}

bool wantsLua(const EditorContext& e) {
    return EditorContext::isLuaPath(e.doc().filePath);
}

void drawHighlight(const std::string& text, ImVec2 origin, float lineHeight,
                   float scrollY, float viewHeight) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float spaceW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, " ").x;

    std::vector<Span> spans;
    int longBracket = -1;
    std::size_t pos = 0;
    int lineNo = 0;

    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line =
            std::string_view(text).substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);

        const float y = origin.y + static_cast<float>(lineNo) * lineHeight;
        lexLine(line, longBracket, spans);
        if (y + lineHeight >= origin.y + scrollY
            && y <= origin.y + scrollY + viewHeight) {
            for (const Span& s : spans) {
                if (s.kind == Tok::Plain) continue;
                const std::string_view before = line.substr(0, s.begin);
                const float x = origin.x
                                + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f,
                                                      before.data(),
                                                      before.data() + before.size())
                                      .x;
                const std::string_view tok =
                    line.substr(s.begin, s.end - s.begin);
                dl->AddText(ImVec2(x, y), kTokColor[static_cast<int>(s.kind)],
                            tok.data(), tok.data() + tok.size());
            }
        }
        ++lineNo;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    g_lineCount = std::max(1, lineNo);
    (void)spaceW;
}

int resizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
    auto* str = static_cast<std::string*>(data->UserData);
    str->resize(static_cast<std::size_t>(data->BufTextLen));
    data->Buf = str->data();
    return 0;
}

}

void drawTextDocument(EditorContext& e) {
    GraphDoc& d = e.doc();

    ImGui::TextDisabled("%s%s", d.displayName().c_str(), d.dirty ? " *" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("| %d lines", g_lineCount);
    if (e.isGeneratedLua(d.filePath)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1),
                           "| generated - the next build overwrites this");
    }
    if (!e.luaErrors.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        for (const LuaDiagnostic& x : e.luaErrors)
            ImGui::Text("line %d: %s", x.line, x.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    const float lineHeight = ImGui::GetTextLineHeight();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    const float gutterW = ImGui::CalcTextSize("0000").x + 8.0f;
    ImGui::BeginChild("gutter", ImVec2(gutterW, avail.y),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar
                          | ImGuiWindowFlags_NoScrollWithMouse);
    static float syncedScroll = 0.0f;
    ImGui::SetScrollY(syncedScroll);
    for (int i = 1; i <= g_lineCount; ++i)
        ImGui::TextDisabled("%4d", i);
    ImGui::EndChild();
    ImGui::SameLine(0, 2);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(24, 26, 30, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(216, 220, 226, 255));

    const ImVec2 boxSize(-FLT_MIN, avail.y);
    const ImVec2 cursorBefore = ImGui::GetCursorScreenPos();

    const bool edited = ImGui::InputTextMultiline(
        "##lua", d.text.data(), d.text.capacity() + 1, boxSize,
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize
            | ImGuiInputTextFlags_NoHorizontalScroll,
        resizeCallback, &d.text);

    if (ImGuiWindow* box = ImGui::FindWindowByName("##lua")) syncedScroll = box->Scroll.y;

    ImGui::PopStyleColor(2);

    if (edited) {
        e.noteTextEdit(true);
        e.recheckLua();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) e.endTextBurst();

    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    if (wantsLua(e))
        drawHighlight(d.text, ImVec2(cursorBefore.x + pad.x,
                                     cursorBefore.y + pad.y - syncedScroll),
                      lineHeight, syncedScroll, avail.y);
    else
        countLines(d.text);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
        && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        e.endTextBurst();
        e.saveText();
    }
}

}
