#include "lime/lua_import.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace lime {
namespace {

struct Token {
    enum class K { Name, Number, String, Op, Eof } kind = K::Eof;
    std::string text;
    std::size_t begin = 0;
    std::size_t end = 0;
    int line = 1;
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : s_(src) { }

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skipTrivia();
            if (i_ >= s_.size()) break;
            const std::size_t start = i_;
            const int line = line_;
            Token t;
            t.begin = start;
            t.line = line;

            const char c = s_[i_];
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                while (i_ < s_.size()
                       && (std::isalnum(static_cast<unsigned char>(s_[i_]))
                           || s_[i_] == '_'))
                    ++i_;
                t.kind = Token::K::Name;
            } else if (std::isdigit(static_cast<unsigned char>(c))
                       || (c == '.' && i_ + 1 < s_.size()
                           && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
                while (i_ < s_.size()
                       && (std::isalnum(static_cast<unsigned char>(s_[i_]))
                           || s_[i_] == '.'
                           || ((s_[i_] == '-' || s_[i_] == '+')
                               && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E'))))
                    ++i_;
                t.kind = Token::K::Number;
            } else if (c == '"' || c == '\'') {
                readQuoted(c);
                t.kind = Token::K::String;
            } else if (c == '[' && longBracketLevel(i_) >= 0) {
                readLongBracket();
                t.kind = Token::K::String;
            } else {
                readOperator();
                t.kind = Token::K::Op;
            }
            t.end = i_;
            t.text = std::string(s_.substr(start, i_ - start));
            out.push_back(std::move(t));
        }
        Token eof;
        eof.kind = Token::K::Eof;
        eof.begin = eof.end = s_.size();
        eof.line = line_;
        out.push_back(std::move(eof));
        return out;
    }

private:
    void bump() {
        if (s_[i_] == '\n') ++line_;
        ++i_;
    }

    int longBracketLevel(std::size_t p) const {
        if (p >= s_.size() || s_[p] != '[') return -1;
        std::size_t q = p + 1;
        int level = 0;
        while (q < s_.size() && s_[q] == '=') { ++level; ++q; }
        return (q < s_.size() && s_[q] == '[') ? level : -1;
    }

    void readLongBracket() {
        const int level = longBracketLevel(i_);
        i_ += 2 + static_cast<std::size_t>(level);
        const std::string close = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
        while (i_ < s_.size()) {
            if (s_.compare(i_, close.size(), close) == 0) {
                i_ += close.size();
                return;
            }
            bump();
        }
    }

    void readQuoted(char quote) {
        bump();
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\') { bump(); if (i_ < s_.size()) bump(); continue; }
            if (c == quote) { bump(); return; }
            if (c == '\n') return;
            bump();
        }
    }

    void readOperator() {
        static const char* kThree[] = {"...", nullptr};
        static const char* kTwo[] = {"==", "~=", "<=", ">=", "//", "::", "..",
                                     "<<", ">>", nullptr};
        for (const char** p = kThree; *p; ++p)
            if (s_.compare(i_, 3, *p) == 0) { i_ += 3; return; }
        for (const char** p = kTwo; *p; ++p)
            if (s_.compare(i_, 2, *p) == 0) { i_ += 2; return; }
        bump();
    }

    void skipTrivia() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { bump(); continue; }
            if (c == '-' && i_ + 1 < s_.size() && s_[i_ + 1] == '-') {
                i_ += 2;
                if (longBracketLevel(i_) >= 0) { readLongBracket(); continue; }
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            return;
        }
    }

    std::string_view s_;
    std::size_t i_ = 0;
    int line_ = 1;
};

class Importer {
public:
    Importer(std::string_view src, std::vector<Token> toks,
             const NodeRegistry* nodes, const ImportOptions& opts, Graph& g,
             Diagnostics& diag)
        : src_(src), t_(std::move(toks)), nodes_(nodes), opts_(opts), g_(g),
          diag_(diag) {}

    void run() {
        NodeId looseRoot{};
        NodeId looseTail{};

        while (!at(Token::K::Eof)) {
            const std::size_t before = p_;
            if (tryHookRegistration()) continue;

            const std::size_t stmtStart = t_[p_].begin;
            skipStatement();
            if (p_ == before) { advance(); continue; }

            const std::string text = slice(stmtStart, prevEnd());
            if (text.empty()) continue;

            if (!looseRoot.valid()) {
                looseRoot = make("Lime.onStart", 0, row_);
                looseTail = looseRoot;
                ++col_;
            }
            const NodeId raw = makeRaw(text, col_ * opts_.columnWidth, row_);
            chain(looseTail, raw);
            looseTail = raw;
            row_ += opts_.rowHeight;
        }
    }

private:

    const Token& cur() const { return t_[p_]; }
    bool at(Token::K k) const { return t_[p_].kind == k; }
    bool isWord(const char* w) const {
        return t_[p_].kind == Token::K::Name && t_[p_].text == w;
    }
    bool isOp(const char* o) const {
        return t_[p_].kind == Token::K::Op && t_[p_].text == o;
    }
    void advance() { if (t_[p_].kind != Token::K::Eof) ++p_; }
    std::size_t prevEnd() const { return p_ > 0 ? t_[p_ - 1].end : 0; }

    std::string slice(std::size_t b, std::size_t e) const {
        if (e <= b || e > src_.size()) return {};
        std::string out(src_.substr(b, e - b));
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'
                                || out.back() == ' ' || out.back() == '\t'))
            out.pop_back();
        return out;
    }

    NodeId make(const std::string& type, float x, float y) {
        return g_.addNode(type, x, y);
    }

    NodeId makeRaw(const std::string& body, float x, float y) {
        const NodeId n = make("core.raw", x, y);
        if (Node* node = g_.node(n)) node->rawBody = body;
        return n;
    }

    void chain(NodeId from, NodeId to, const char* pin = "out") {
        if (!from.valid() || !to.valid()) return;
        g_.connect(PinId::make(from, pin), PinId::make(to, "in"), PinKind::Exec);
    }

    void setInput(NodeId n, const std::string& pin, const std::string& text) {
        if (Node* node = g_.node(n)) node->values.emplace_back(pin, text);
    }

    void skipStatement() {
        if (isWord("if") || isWord("while") || isWord("for") || isWord("do")
            || isWord("function") || isWord("repeat")) {
            skipBlockStatement();
            return;
        }
        const int startLine = cur().line;
        while (!at(Token::K::Eof)) {
            if (blockKeyword()) break;
            if (cur().line != startLine && !continuesStatement()) break;
            if (isWord("function") || isWord("if") || isWord("while")
                || isWord("for") || isWord("do") || isWord("repeat")) {
                skipBlockStatement();
                continue;
            }
            advance();
        }
    }

    bool continuesStatement() const {
        return isOp(")") || isOp("}") || isOp("]") || isOp(",") || isOp(".")
               || isOp("..") || isOp("+") || isOp("-") || isOp("*") || isOp("/")
               || isOp("=") || isOp("==") || isOp("and") || isOp("or")
               || isWord("and") || isWord("or") || isWord("end");
    }

    bool blockKeyword() const {
        return isWord("end") || isWord("else") || isWord("elseif")
               || isWord("until");
    }

    void skipBlockStatement() {
        int depth = 0;
        bool inLoopHeader = false;
        while (!at(Token::K::Eof)) {
            if (isWord("for") || isWord("while")) {
                ++depth;
                inLoopHeader = true;
            } else if (isWord("if") || isWord("function") || isWord("repeat")) {
                ++depth;
            } else if (isWord("do")) {
                if (inLoopHeader) inLoopHeader = false;
                else              ++depth;
            } else if (isWord("end") || isWord("until")) {
                --depth;
                advance();
                if (depth <= 0) return;
                continue;
            }
            advance();
        }
    }

    bool tryHookRegistration() {
        const std::size_t save = p_;
        if (!at(Token::K::Name)) return false;

        std::string path = cur().text;
        advance();
        while (isOp(".")) {
            advance();
            if (!at(Token::K::Name)) { p_ = save; return false; }
            path += "." + cur().text;
            advance();
        }
        if (!isOp(":")) { p_ = save; return false; }
        advance();
        if (!isWord("hook")) { p_ = save; return false; }
        advance();
        if (!isOp("(")) { p_ = save; return false; }
        advance();
        if (!isWord("function")) { p_ = save; return false; }
        advance();

        std::vector<std::string> params;
        if (isOp("(")) {
            advance();
            while (!isOp(")") && !at(Token::K::Eof)) {
                if (at(Token::K::Name)) params.push_back(cur().text);
                advance();
            }
            advance();
        }

        const NodeDesc* d = nodes_ ? nodes_->find(path) : nullptr;
        NodeId root;
        if (d && d->isEvent) {
            root = make(path, 0, row_);
        } else {
            root = make("Lime.onStart", 0, row_);
            diag_.warn("no event node named '" + path
                       + "'; its body was imported under On Start");
        }

        const float bodyX = opts_.columnWidth;
        float bodyY = row_;
        NodeId tail = root;
        parseBlock(tail, root, "out", bodyX, bodyY);

        while (!at(Token::K::Eof) && !isOp(")")) advance();
        if (isOp(")")) advance();

        row_ = bodyY + opts_.rowHeight * 2;
        return true;
    }

    void parseBlock(NodeId& tail, NodeId parent, const char* pin, float x,
                    float& y) {
        bool first = true;
        while (!at(Token::K::Eof) && !blockKeyword()) {
            const std::size_t before = p_;
            NodeId head, last;
            if (!parseStatement(head, last, x, y)) break;
            if (p_ == before) { advance(); continue; }
            if (!head.valid()) continue;

            if (first) { chain(parent, head, pin); first = false; }
            else       { chain(tail, head); }
            tail = last.valid() ? last : head;
        }
        if (isWord("end")) advance();
    }

    bool parseStatement(NodeId& head, NodeId& last, float x, float& y) {
        head = last = NodeId{};

        if (isWord("if"))     { return parseIf(head, last, x, y); }
        if (isWord("while"))  { return parseWhile(head, last, x, y); }
        if (isWord("for"))    { return parseFor(head, last, x, y); }
        if (isWord("break")) {
            advance();
            head = last = make("core.break", x, y);
            y += opts_.rowHeight;
            return true;
        }
        if (isWord("return")) {
            const std::size_t b = cur().begin;
            advance();
            const std::size_t valStart = cur().begin;
            const int line = t_[p_ - 1].line;
            while (!at(Token::K::Eof) && !blockKeyword() && cur().line == line)
                advance();
            head = last = make("core.return", x, y);
            const std::string val = slice(valStart, prevEnd());
            if (!val.empty()) setInput(head, "value", val);
            (void)b;
            y += opts_.rowHeight;
            return true;
        }

        const std::size_t b = cur().begin;
        skipStatement();
        const std::string text = slice(b, prevEnd());
        if (text.empty()) return false;
        head = last = makeRaw(text, x, y);
        y += opts_.rowHeight;
        return true;
    }

    bool parseIf(NodeId& head, NodeId& last, float x, float& y) {
        advance();
        const std::size_t condStart = cur().begin;
        while (!at(Token::K::Eof) && !isWord("then")) advance();
        const std::string cond = slice(condStart, prevEnd());
        if (isWord("then")) advance();

        const NodeId branch = make("core.branch", x, y);
        if (!cond.empty()) setInput(branch, "cond", cond);
        head = branch;

        float ty = y;
        NodeId tTail = branch;
        parseBlockNoEnd(tTail, branch, "true", x + opts_.columnWidth, ty);

        float ey = y + opts_.rowHeight * 3;
        NodeId eTail = branch;
        if (isWord("elseif")) {
            NodeId nhead, nlast;
            t_[p_].text = "if";
            parseIf(nhead, nlast, x + opts_.columnWidth, ey);
            if (nhead.valid()) chain(branch, nhead, "false");
        } else if (isWord("else")) {
            advance();
            parseBlockNoEnd(eTail, branch, "false", x + opts_.columnWidth, ey);
        }
        if (isWord("end")) advance();

        y = (ey > ty ? ey : ty) + opts_.rowHeight;
        last = branch;
        return true;
    }

    bool parseWhile(NodeId& head, NodeId& last, float x, float& y) {
        advance();
        const std::size_t condStart = cur().begin;
        while (!at(Token::K::Eof) && !isWord("do")) advance();
        const std::string cond = slice(condStart, prevEnd());
        if (isWord("do")) advance();

        const NodeId loop = make("core.while", x, y);
        if (!cond.empty()) setInput(loop, "cond", cond);
        head = last = loop;

        float by = y;
        NodeId tail = loop;
        parseBlockNoEnd(tail, loop, "body", x + opts_.columnWidth, by);
        if (isWord("end")) advance();
        y = by + opts_.rowHeight;
        return true;
    }

    bool parseFor(NodeId& head, NodeId& last, float x, float& y) {
        advance();
        const std::size_t headStart = cur().begin;
        bool generic = false;
        while (!at(Token::K::Eof) && !isWord("do")) {
            if (isWord("in")) generic = true;
            advance();
        }
        const std::string spec = slice(headStart, prevEnd());
        if (isWord("do")) advance();

        const NodeId loop = make(generic ? "core.forIn" : "core.forNum", x, y);
        if (Node* n = g_.node(loop)) n->comment = spec;
        head = last = loop;

        float by = y;
        NodeId tail = loop;
        parseBlockNoEnd(tail, loop, "body", x + opts_.columnWidth, by);
        if (isWord("end")) advance();
        y = by + opts_.rowHeight;
        return true;
    }

    void parseBlockNoEnd(NodeId& tail, NodeId parent, const char* pin, float x,
                         float& y) {
        bool first = true;
        while (!at(Token::K::Eof) && !blockKeyword()) {
            const std::size_t before = p_;
            NodeId head, last;
            if (!parseStatement(head, last, x, y)) break;
            if (p_ == before) { advance(); continue; }
            if (!head.valid()) continue;
            if (first) { chain(parent, head, pin); first = false; }
            else       { chain(tail, head); }
            tail = last.valid() ? last : head;
        }
    }

    std::string_view   src_;
    std::vector<Token> t_;
    std::size_t        p_ = 0;
    const NodeRegistry* nodes_ = nullptr;
    ImportOptions      opts_;
    Graph&             g_;
    Diagnostics&       diag_;
    float              row_ = 0;
    int                col_ = 0;
};

}

bool importLua(std::string_view luaSource, const NodeRegistry* nodes,
               const ImportOptions& opts, Graph& out, Diagnostics& diag) {
    out = Graph{};
    out.moduleName = opts.moduleName;

    if (luaSource.size() >= 3
        && static_cast<unsigned char>(luaSource[0]) == 0xEF
        && static_cast<unsigned char>(luaSource[1]) == 0xBB
        && static_cast<unsigned char>(luaSource[2]) == 0xBF)
        luaSource.remove_prefix(3);

    Lexer lex(luaSource);
    Importer imp(luaSource, lex.run(), nodes, opts, out, diag);
    imp.run();
    return true;
}

bool importLuaFile(const std::string& path, const NodeRegistry* nodes,
                   const ImportOptions& opts, Graph& out, Diagnostics& diag) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot open " + path);
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    ImportOptions o = opts;
    if (o.moduleName.empty()) {
        std::string p = path;
        for (char& c : p) if (c == '\\') c = '/';
        const std::size_t slash = p.find("content/");
        std::string rel = slash == std::string::npos
                              ? p.substr(p.find_last_of('/') + 1)
                              : p.substr(slash);
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".lua") == 0)
            rel.resize(rel.size() - 4);
        for (char& c : rel) if (c == '/') c = '.';
        o.moduleName = rel;
    }
    return importLua(text, nodes, o, out, diag);
}

}
