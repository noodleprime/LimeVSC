#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lime {

struct TypeId {
    std::uint32_t v = kInvalid;
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr bool valid() const noexcept { return v != kInvalid; }
    friend constexpr bool operator==(TypeId, TypeId) = default;
};

struct NodeId {
    std::uint32_t v = kInvalid;
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr bool valid() const noexcept { return v != kInvalid; }
    friend constexpr bool operator==(NodeId, NodeId) = default;
    friend constexpr bool operator<(NodeId a, NodeId b) { return a.v < b.v; }
};

class Interner {
public:
    static std::uint32_t id(std::string_view s);
    static std::string_view str(std::uint32_t v);
};

struct PinName {
    std::uint32_t v = kInvalid;
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    static PinName of(std::string_view s) { return PinName{Interner::id(s)}; }
    std::string_view str() const { return valid() ? Interner::str(v) : std::string_view{}; }
    constexpr bool valid() const noexcept { return v != kInvalid; }
    friend constexpr bool operator==(PinName, PinName) = default;
};

struct PinId {
    NodeId  node{};
    PinName pin{};
    static PinId make(NodeId n, std::string_view p) { return PinId{n, PinName::of(p)}; }
    constexpr bool valid() const noexcept { return node.valid() && pin.valid(); }
    friend constexpr bool operator==(PinId, PinId) = default;
};

enum class PinDir : std::uint8_t { In, Out };

enum class PinKind : std::uint8_t { Exec, Data };

enum class Severity : std::uint8_t { Info, Warning, Error };

struct Diagnostic {
    Severity     severity = Severity::Error;
    std::string  message;
    std::string  file;
    NodeId       node{};
    int          line = 0;
};

class Diagnostics {
public:
    void add(Diagnostic d);
    void info(std::string msg, NodeId n = {});
    void warn(std::string msg, NodeId n = {});
    void error(std::string msg, NodeId n = {});

    bool hasErrors() const noexcept { return errors_ > 0; }
    int  errorCount() const noexcept { return errors_; }
    int  warningCount() const noexcept { return warnings_; }
    std::span<const Diagnostic> all() const noexcept { return items_; }
    void clear();

private:
    std::vector<Diagnostic> items_;
    int errors_ = 0;
    int warnings_ = 0;
};

struct TypeDesc {
    std::string   name;
    std::uint32_t color = 0;
    std::vector<std::string> coercesTo;
    bool isEnum = false;
    std::vector<std::pair<std::string, long long>> enumValues;
};

class TypeRegistry {
public:
    TypeId          intern(const TypeDesc& d);
    TypeId          find(std::string_view name) const;
    const TypeDesc& get(TypeId id) const;
    std::size_t     size() const noexcept { return types_.size(); }

    bool canConnect(TypeId from, TypeId to) const;

    bool loadFile(const std::string& path, Diagnostics& diag);

private:
    std::vector<TypeDesc> types_;
};

struct PinDesc {
    std::string  name;
    TypeId       type{};
    PinDir       dir = PinDir::In;
    PinKind      kind = PinKind::Data;
    bool         optional = false;
    std::string  defaultValue;
};

struct NodeDesc {
    std::string  id;
    std::string  display;
    std::string  category;
    std::string  doc;

    std::string  emit;
    std::string  target;

    bool         pure = false;
    bool         isEvent = false;
    std::uint32_t color = 0;

    std::vector<PinDesc> pins;
    int          priority = 0;

    const PinDesc* findPin(std::string_view name) const;
    bool hasExecPins() const;
};

class INodeProvider {
public:
    virtual ~INodeProvider() = default;
    virtual std::string_view name() const = 0;
    virtual int priority() const = 0;
    virtual void collect(TypeRegistry& types,
                         std::vector<NodeDesc>& out,
                         Diagnostics& diag) = 0;
};

class NodeRegistry {
public:
    void addProvider(std::unique_ptr<INodeProvider> p);
    void rebuild(TypeRegistry& types, Diagnostics& diag);

    const NodeDesc* find(std::string_view id) const;
    std::span<const NodeDesc> all() const noexcept { return descs_; }
    std::span<const NodeDesc* const> browseOrder() const noexcept { return browse_; }
    std::vector<std::string> categories() const;

private:
    std::vector<std::unique_ptr<INodeProvider>> providers_;
    std::vector<NodeDesc> descs_;
    std::vector<const NodeDesc*> browse_;
};

struct ExprRef { std::uint32_t v = 0xFFFFFFFFu;
                 constexpr bool valid() const { return v != 0xFFFFFFFFu; } };
struct StmtRef { std::uint32_t v = 0xFFFFFFFFu;
                 constexpr bool valid() const { return v != 0xFFFFFFFFu; } };

class LuaAst {
public:
    enum class EK : std::uint8_t {
        Name, Nil, Bool, Number, String, Raw, Binop, Unop,
        Field, Index, Call, Method, Function, Table, Varargs
    };
    enum class SK : std::uint8_t {
        Block, ExprStat, Local, Assign, If, While, Repeat, NumFor, GenFor,
        Return, Break, Goto, Label, LocalFunc, Func, Comment, Blank
    };

    struct ExprNode {
        EK          kind = EK::Nil;
        std::string text;
        double      num = 0;
        bool        b = false;
        std::vector<ExprRef>     kids;
        std::vector<std::string> params;
        StmtRef     body;
    };

    struct StmtNode {
        SK          kind = SK::Blank;
        std::string text;
        std::vector<StmtRef>     kids;
        std::vector<ExprRef>     exprs;
        std::vector<std::string> names;
        int         count = 0;
        NodeId      attr;
    };

    ExprRef name(std::string_view ident);
    ExprRef nil();
    ExprRef boolean(bool b);
    ExprRef number(double d);
    ExprRef string(std::string_view s);
    ExprRef rawExpr(std::string_view lua);
    ExprRef binop(std::string_view op, ExprRef a, ExprRef b);
    ExprRef unop(std::string_view op, ExprRef a);
    ExprRef field(ExprRef obj, std::string_view name);
    ExprRef indexExpr(ExprRef obj, ExprRef key);
    ExprRef call(ExprRef fn, std::span<const ExprRef> args);
    ExprRef methodCall(ExprRef obj, std::string_view m,
                       std::span<const ExprRef> args);
    ExprRef function(std::span<const std::string> params, StmtRef body);
    ExprRef table(std::span<const std::pair<std::string, ExprRef>> fields);
    ExprRef varargs();

    StmtRef block(std::span<const StmtRef> stmts);
    StmtRef exprStat(ExprRef e);
    StmtRef localDecl(std::span<const std::string> names,
                      std::span<const ExprRef> values);
    StmtRef assign(std::span<const ExprRef> targets,
                   std::span<const ExprRef> values);
    StmtRef ifStat(std::span<const std::pair<ExprRef, StmtRef>> arms,
                   StmtRef elseBlock);
    StmtRef whileStat(ExprRef cond, StmtRef body);
    StmtRef repeatStat(StmtRef body, ExprRef untilCond);
    StmtRef numericFor(std::string_view var, ExprRef from, ExprRef to,
                       ExprRef step, StmtRef body);
    StmtRef genericFor(std::span<const std::string> vars,
                       std::span<const ExprRef> exprs, StmtRef body);
    StmtRef returnStat(std::span<const ExprRef> values);
    StmtRef breakStat();
    StmtRef gotoStat(std::string_view label);
    StmtRef labelStat(std::string_view label);
    StmtRef localFunction(std::string_view name,
                          std::span<const std::string> params, StmtRef body);
    StmtRef functionStat(std::string_view qualifiedName,
                         std::span<const std::string> params, StmtRef body);
    StmtRef comment(std::string_view text);
    StmtRef blankLine();

    void    attribute(NodeId n);

    const ExprNode& expr(ExprRef r) const;
    const StmtNode& stmt(StmtRef r) const;
    std::size_t exprCount() const noexcept { return exprs_.size(); }
    std::size_t stmtCount() const noexcept { return stmts_.size(); }

private:
    StmtRef push(StmtNode n);

    std::vector<ExprNode> exprs_;
    std::vector<StmtNode> stmts_;
    NodeId pendingAttr_{};
};

struct SourceMap {
    std::vector<std::pair<int, NodeId>> lines;
    NodeId nodeForLine(int line) const;
};

struct PrintOptions {
    int  indentWidth = 2;
    bool emitHeader = true;
    std::string headerSourceName;
};

std::string printLua(const LuaAst& ast, StmtRef root,
                     const PrintOptions& opts, SourceMap* mapOut);

class IEmitInputs {
public:
    virtual ~IEmitInputs() = default;
    virtual ExprRef input(std::string_view pinName) const = 0;
    virtual bool    hasInput(std::string_view pinName) const = 0;
};

struct EmitContext {
    LuaAst&           ast;
    const NodeDesc&   desc;
    const IEmitInputs& in;
    NodeId            node{};
    Diagnostics&      diag;
};

struct EmitResult {
    ExprRef value{};
    StmtRef stmt{};
};

class IEmitter {
public:
    virtual ~IEmitter() = default;
    virtual std::string_view key() const = 0;
    virtual EmitResult emit(EmitContext& ctx) const = 0;
};

class EmitterRegistry {
public:
    void add(std::unique_ptr<IEmitter> e);
    const IEmitter* find(std::string_view key) const;
    static EmitterRegistry withBuiltins();

private:
    std::vector<std::unique_ptr<IEmitter>> emitters_;
};

struct Node {
    NodeId      id{};
    std::string type;
    float       x = 0, y = 0;
    float       w = 0, h = 0;
    std::vector<std::pair<std::string, std::string>> values;
    std::string rawBody;
    std::string comment;
};

struct Link {
    PinId   from;
    PinId   to;
    PinKind kind = PinKind::Data;
};

struct FnParam {
    std::string name;
    std::string type = "any";
};
struct FnDecl {
    std::string name;
    std::vector<FnParam> params;
    std::string ret;
};

struct PropDecl {
    std::string name;
    std::string type = "any";
    std::string defaultValue;
};

using VarDecl = PropDecl;

std::string varIdent(std::string_view name);

class Graph {
public:
    std::string moduleName;
    std::string graphName;
    std::vector<FnDecl> functions;
    std::vector<PropDecl> properties;
    std::vector<VarDecl> variables;

    NodeId addNode(std::string type, float x, float y);
    NodeId addNodeWithId(NodeId id, std::string type, float x, float y);
    void   removeNode(NodeId id);
    Node*  node(NodeId id);
    const Node* node(NodeId id) const;
    std::span<const Node> nodes() const noexcept { return nodes_; }

    bool   connect(PinId from, PinId to, PinKind kind);
    void   addLinkUnchecked(PinId from, PinId to, PinKind kind);
    bool   validateLinks(Diagnostics& diag) const;
    void   disconnect(PinId to);
    void   disconnectFrom(PinId from);
    void   disconnectAll(NodeId n);
    std::span<const Link> links() const noexcept { return links_; }

    std::optional<PinId> sourceOf(PinId input) const;
    std::optional<PinId> execTargetOf(PinId output) const;
    std::vector<PinId>   targetsOf(PinId output) const;
    std::vector<PinId>   execSourcesOf(PinId input) const;

    std::uint32_t nextId = 0;

private:
    void reindexNodes() const;
    void reindexLinks() const;
    static std::uint64_t pinKey(PinId p) {
        return (static_cast<std::uint64_t>(p.node.v) << 32) | p.pin.v;
    }

    std::vector<Node> nodes_;
    std::vector<Link> links_;

    mutable bool nodeIxDirty_ = true;
    mutable bool linkIxDirty_ = true;
    mutable std::unordered_map<std::uint32_t, std::size_t> nodeIx_;
    mutable std::unordered_map<std::uint64_t, std::size_t> dataBySink_;
    mutable std::unordered_map<std::uint64_t, std::size_t> execBySource_;
    mutable std::unordered_map<std::uint64_t, std::vector<std::size_t>> dataBySource_;
    mutable std::unordered_map<std::uint64_t, std::vector<std::size_t>> execBySink_;
};

bool readLime(const std::string& path, Graph& out, Diagnostics& diag);
bool parseLime(std::string_view text, Graph& out, Diagnostics& diag);
std::string writeLime(const Graph& g);
bool writeLimeFile(const std::string& path, const Graph& g, Diagnostics& diag);

std::string encodeId(std::uint32_t v);
std::optional<std::uint32_t> decodeId(std::string_view s);

struct CompileContext {
    const Graph&         graph;
    const NodeRegistry&  nodes;
    const TypeRegistry&  types;
    const EmitterRegistry& emitters;
    LuaAst               ast;
    StmtRef              root{};
    SourceMap            map;
    std::string          sourceName;
    int                  gotoCount = 0;
};

class IPass {
public:
    virtual ~IPass() = default;
    virtual std::string_view name() const = 0;
    virtual bool run(CompileContext& ctx, Diagnostics& diag) = 0;
};

class PassPipeline {
public:
    void add(std::unique_ptr<IPass> p);
    bool run(CompileContext& ctx, Diagnostics& diag);
    static PassPipeline standard();

private:
    std::vector<std::unique_ptr<IPass>> passes_;
};

struct CompileResult {
    bool        ok = false;
    std::string lua;
    SourceMap   map;
    int         gotoCount = 0;
};

CompileResult compileGraph(const Graph& g, const NodeRegistry& nodes,
                           const TypeRegistry& types,
                           const EmitterRegistry& emitters,
                           std::string sourceName, Diagnostics& diag);

class EditorContext;
class ProjectContext;

class IPanel {
public:
    virtual ~IPanel() = default;
    virtual std::string_view id() const = 0;
    virtual std::string_view title() const = 0;
    virtual void draw(EditorContext& ed) = 0;
    virtual bool defaultOpen() const { return true; }
    virtual bool availableIn(bool engineMode) const { (void)engineMode; return true; }
};

struct Command {
    std::string id;
    std::string title;
    std::string category;
    std::string defaultKey;
    std::function<void(EditorContext&)> run;
    std::function<bool(EditorContext&)> enabled;
};

class CommandRegistry {
public:
    void add(Command c);
    const Command* find(std::string_view id) const;
    std::span<const Command> all() const noexcept { return commands_; }
    bool invoke(std::string_view id, EditorContext& ed) const;

private:
    std::vector<Command> commands_;
};

class IBuildAction {
public:
    virtual ~IBuildAction() = default;
    virtual std::string_view id() const = 0;
    virtual std::string_view title() const = 0;
    virtual bool run(ProjectContext& proj, Diagnostics& diag) = 0;
};

}
