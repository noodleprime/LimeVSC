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

const LuaDiagnostic* errorOn(const EditorContext& e, int line) {
    for (const LuaDiagnostic& x : e.luaErrors)
        if (x.line == line) return &x;
    return nullptr;
}

void drawErrorLines(const EditorContext& e, ImVec2 origin, float lineHeight,
                    ImVec2 area) {
    if (e.luaErrors.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(origin.x, origin.y),
                     ImVec2(origin.x + area.x, origin.y + area.y), true);
    for (const LuaDiagnostic& x : e.luaErrors) {
        const float y = origin.y + static_cast<float>(x.line - 1) * lineHeight;
        dl->AddRectFilled(ImVec2(origin.x, y),
                          ImVec2(origin.x + area.x, y + lineHeight),
                          IM_COL32(210, 70, 60, 34));
        dl->AddLine(ImVec2(origin.x, y + lineHeight - 1.0f),
                    ImVec2(origin.x + area.x, y + lineHeight - 1.0f),
                    IM_COL32(226, 92, 84, 190));
    }
    dl->PopClipRect();
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

struct Completion {
    std::string label;
    const char* kind;
};

std::string wordBefore(const std::string& text, int at) {
    int i = at;
    while (i > 0 && isIdentChar(text[static_cast<std::size_t>(i) - 1])) --i;
    return text.substr(static_cast<std::size_t>(i),
                       static_cast<std::size_t>(at - i));
}

void gatherWords(const std::string& text, const std::string& skip,
                 std::vector<Completion>& out) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!isIdentChar(text[i])) {
            ++i;
            continue;
        }
        const std::size_t b = i;
        while (i < text.size() && isIdentChar(text[i])) ++i;
        std::string w = text.substr(b, i - b);
        if (w == skip || w.size() < 3) continue;
        if (std::isdigit(static_cast<unsigned char>(w[0]))) continue;
        if (isKeyword(w)) continue;
        bool seen = false;
        for (const Completion& c : out)
            if (c.label == w) { seen = true; break; }
        if (!seen) out.push_back({std::move(w), "word"});
    }
}

std::vector<Completion> completionsFor(const EditorContext& e,
                                       const std::string& prefix) {
    std::vector<Completion> all;
    if (prefix.empty()) return all;

    if (wantsLua(e)) {
        for (const char* k : kKeywords)
            if (std::string_view(k).compare(0, prefix.size(), prefix) == 0)
                all.push_back({k, "keyword"});
        for (const NodeDesc& d : e.nodes.all()) {
            if (d.target.empty()) continue;
            if (d.target.compare(0, prefix.size(), prefix) != 0) continue;
            bool seen = false;
            for (const Completion& c : all)
                if (c.label == d.target) { seen = true; break; }
            if (!seen) all.push_back({d.target, "lime"});
        }
    }

    std::vector<Completion> words;
    gatherWords(e.doc().text, prefix, words);
    for (Completion& w : words)
        if (w.label.compare(0, prefix.size(), prefix) == 0)
            all.push_back(std::move(w));

    if (all.size() > 12) all.resize(12);
    return all;
}

int g_completeAt = -1;
int g_completePick = 0;
std::string g_completePrefix;
std::vector<Completion> g_completions;
bool g_completeInsert = false;
std::string g_completeChosen;

int editCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = str->data();
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (g_completeInsert) {
            g_completeInsert = false;
            const int from = data->CursorPos
                             - static_cast<int>(g_completePrefix.size());
            if (from >= 0) {
                data->DeleteChars(from,
                                  static_cast<int>(g_completePrefix.size()));
                data->InsertChars(from, g_completeChosen.c_str());
            }
            g_completeAt = -1;
            return 0;
        }
        g_completeAt = data->CursorPos;
    }
    return 0;
}


}

void drawCompletions(EditorContext& e, bool active, ImVec2 boxOrigin,
                     float lineHeight, ImVec2 pad, float scroll) {
    if (!active || g_completeAt < 0) {
        g_completions.clear();
        return;
    }

    const std::string& text = e.doc().text;
    const int at = std::min(g_completeAt, static_cast<int>(text.size()));
    const std::string prefix = wordBefore(text, at);
    if (prefix.size() < 2) {
        g_completions.clear();
        return;
    }
    if (prefix != g_completePrefix) {
        g_completePrefix = prefix;
        g_completions = completionsFor(e, prefix);
        g_completePick = 0;
    }
    if (g_completions.empty()) return;

    const int count = static_cast<int>(g_completions.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        g_completePick = (g_completePick + 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        g_completePick = (g_completePick + count - 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_completions.clear();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)
        || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        g_completeChosen = g_completions[static_cast<std::size_t>(g_completePick)].label;
        g_completeInsert = true;
        g_completions.clear();
        return;
    }

    int line = 0, col = 0;
    for (int i = 0; i < at; ++i) {
        if (text[static_cast<std::size_t>(i)] == '\n') { ++line; col = 0; }
        else ++col;
    }
    const float charW = ImGui::CalcTextSize("M").x;
    const ImVec2 at2(boxOrigin.x + pad.x + static_cast<float>(col) * charW,
                     boxOrigin.y + pad.y - scroll
                         + static_cast<float>(line + 1) * lineHeight);

    ImGui::SetNextWindowPos(at2);
    ImGui::SetNextWindowBgAlpha(0.98f);
    if (ImGui::Begin("##complete", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                         | ImGuiWindowFlags_AlwaysAutoResize
                         | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoFocusOnAppearing
                         | ImGuiWindowFlags_NoNav)) {
        for (int i = 0; i < count; ++i) {
            const Completion& c = g_completions[static_cast<std::size_t>(i)];
            const bool on = i == g_completePick;
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 225, 130, 255));
            ImGui::TextUnformatted(c.label.c_str());
            if (on) ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", c.kind);
        }
    }
    ImGui::End();
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
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.38f, 1.0f), "| %zu error%s",
                           e.luaErrors.size(),
                           e.luaErrors.size() == 1 ? "" : "s");
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
    for (int i = 1; i <= g_lineCount; ++i) {
        const LuaDiagnostic* bad = errorOn(e, i);
        if (bad) {
            ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.38f, 1.0f), "%4d", i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bad->message.c_str());
        } else {
            ImGui::TextDisabled("%4d", i);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 2);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(24, 26, 30, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(216, 220, 226, 255));

    const ImVec2 boxSize(-FLT_MIN, avail.y);
    const ImVec2 cursorBefore = ImGui::GetCursorScreenPos();

    const bool edited = ImGui::InputTextMultiline(
        "##lua", d.text.data(), d.text.capacity() + 1, boxSize,
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize
            | ImGuiInputTextFlags_CallbackAlways
            | ImGuiInputTextFlags_NoHorizontalScroll,
        editCallback, &d.text);
    const bool boxActive = ImGui::IsItemActive();

    if (ImGuiWindow* box = ImGui::FindWindowByName("##lua")) syncedScroll = box->Scroll.y;

    ImGui::PopStyleColor(2);

    if (edited) {
        e.noteTextEdit(true);
        e.recheckLua();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) e.endTextBurst();

    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    drawErrorLines(e, ImVec2(cursorBefore.x, cursorBefore.y + pad.y - syncedScroll),
                   lineHeight, avail);

    if (wantsLua(e))
        drawHighlight(d.text, ImVec2(cursorBefore.x + pad.x,
                                     cursorBefore.y + pad.y - syncedScroll),
                      lineHeight, syncedScroll, avail.y);
    else
        countLines(d.text);

    drawCompletions(e, boxActive, cursorBefore, lineHeight, pad, syncedScroll);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
        && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        e.endTextBurst();
        e.saveText();
    }
}

}
