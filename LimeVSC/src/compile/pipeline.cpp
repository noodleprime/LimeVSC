#include "limecore.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace lime {
namespace {

std::string sanitiseIdent(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "v";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
    return out;
}

class Compiler {
public:
    Compiler(CompileContext& ctx, Diagnostics& diag) : ctx_(ctx), diag_(diag) {}

    bool run() {
        std::vector<StmtRef> body;
        for (const Node& n : ctx_.graph.nodes()) {
            const NodeDesc* d = ctx_.nodes.find(n.type);
            if (!d || (d->emit != "varget" && d->emit != "varset")) continue;
            const std::size_t dot = d->id.find_last_of('.');
            const std::string owner =
                dot == std::string::npos ? std::string() : d->id.substr(0, dot);
            const std::string want = "var." + ctx_.graph.moduleName;
            if (owner != want && owner != want + "." + d->target)
                diag_.error("variable '" + d->target + "' belongs to another "
                            "graph; declare one here instead", n.id);
        }

        std::vector<const Node*> roots;
        bool hasBehaviour = false;
        for (const Node& n : ctx_.graph.nodes()) {
            const NodeDesc* d = ctx_.nodes.find(n.type);
            if (!d || !d->isEvent) continue;
            if (d->emit == "struct:behaviour") hasBehaviour = true;
            roots.push_back(&n);
        }
        const bool isModule = !ctx_.graph.functions.empty() || hasBehaviour;
        std::sort(roots.begin(), roots.end(),
                  [](const Node* a, const Node* b) { return a->id.v < b->id.v; });

        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (i) body.push_back(ctx_.ast.blankLine());
            const NodeDesc* d = ctx_.nodes.find(roots[i]->type);
            StmtRef s;
            if (d && d->emit == "struct:fnentry")      s = emitFunction(*roots[i], *d);
            else if (d && d->emit == "struct:behaviour") s = emitBehaviour(*roots[i], *d);
            else                                        s = emitEvent(*roots[i]);
            if (s.valid()) body.push_back(s);
        }

        if (roots.empty())
            diag_.warn("graph has no event or function nodes, so it produces no code");

        std::vector<StmtRef> top;
        for (const auto& [module, alias] : requires_) {
            const ExprRef arg[] = {ctx_.ast.string(module)};
            const std::string names[] = {alias};
            const ExprRef vals[] = {ctx_.ast.call(ctx_.ast.name("require"), arg)};
            top.push_back(ctx_.ast.localDecl(names, vals));
        }
        for (const VarDecl& v : ctx_.graph.variables) {
            const std::string names[] = {varIdent(v.name)};
            const ExprRef vals[] = {
                v.defaultValue.empty() ? ctx_.ast.nil()
                                       : ctx_.ast.rawExpr(v.defaultValue)};
            top.push_back(ctx_.ast.localDecl(names, vals));
        }

        if (isModule) {
            const std::string names[] = {"M"};
            const ExprRef vals[] = {ctx_.ast.table({})};
            top.push_back(ctx_.ast.localDecl(names, vals));
        }
        if (!top.empty()) top.push_back(ctx_.ast.blankLine());
        top.insert(top.end(), body.begin(), body.end());
        if (isModule) {
            top.push_back(ctx_.ast.blankLine());
            const ExprRef rv[] = {ctx_.ast.name("M")};
            top.push_back(ctx_.ast.returnStat(rv));
        }

        ctx_.root = ctx_.ast.block(top);
        ctx_.gotoCount = gotoCount_;
        return !diag_.hasErrors();
    }

private:

    const NodeDesc* descOf(NodeId n) const {
        const Node* node = ctx_.graph.node(n);
        return node ? ctx_.nodes.find(node->type) : nullptr;
    }

    NodeId execTarget(NodeId n, std::string_view pin) const {
        const auto t = ctx_.graph.execTargetOf(PinId::make(n, pin));
        return t ? t->node : NodeId{};
    }

    std::string labelFor(NodeId n) { return "lime_" + encodeId(n.v); }

    ExprRef pinValue(PinId out) {
        if (auto it = bound_.find(out); it != bound_.end()) return it->second;

        const NodeDesc* d = descOf(out.node);
        if (!d) {
            diag_.error("unknown node type", out.node);
            return ctx_.ast.nil();
        }

        if (d->pure) {
            const std::size_t uses = ctx_.graph.targetsOf(out).size();
            if (uses > 1) {
                if (auto it = hoisted_.find(out); it != hoisted_.end()) return it->second;
                const ExprRef e = buildValue(out);
                const std::string nm = uniqueName(nameHint(out));
                const std::string names[] = {nm};
                const ExprRef vals[] = {e};
                pendingHoists_.push_back(ctx_.ast.localDecl(names, vals));
                const ExprRef ref = ctx_.ast.name(nm);
                hoisted_[out] = ref;
                return ref;
            }
            return buildValue(out);
        }

        diag_.error("value read from '" + d->id
                    + "' before it is executed - connect its exec input",
                    out.node);
        return ctx_.ast.nil();
    }

    std::string nameHint(PinId out) {
        const NodeDesc* d = descOf(out.node);
        std::string base = std::string(out.pin.str());
        if (base == "ret" || base.empty()) base = d ? d->display : "value";
        return sanitiseIdent(base);
    }

    std::string uniqueName(std::string base) {
        if (base.empty()) base = "value";
        base[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(base[0])));
        std::string candidate = base;
        int n = 2;
        while (!used_.insert(candidate).second) candidate = base + std::to_string(n++);
        return candidate;
    }

    static std::string scriptModuleLiteral(std::string_view stored) {
        std::string p(stored);
        if (p.size() >= 2 && p.front() == '"' && p.back() == '"')
            p = p.substr(1, p.size() - 2);
        for (char& c : p)
            if (c == '\\') c = '/';
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".lua") == 0)
            p.resize(p.size() - 4);
        while (!p.empty() && (p.front() == '/' || p.front() == '.'))
            p.erase(p.begin());
        if (p.rfind("content/", 0) != 0) p = "content/" + p;
        for (char& c : p)
            if (c == '/') c = '.';
        return "\"" + p + "\"";
    }

    class Inputs final : public IEmitInputs {
    public:
        Inputs(Compiler& c, const Node& n, const NodeDesc& d) : c_(c), n_(n), d_(d) {}

        ExprRef input(std::string_view pin) const override {
            if (pin == "__body") {
                for (const auto& [k, v] : n_.values)
                    if (k == "script" && !v.empty() && v != "\"\"")
                        return c_.ctx_.ast.rawExpr(
                            "require(" + scriptModuleLiteral(v) + ")");
                return c_.ctx_.ast.rawExpr(n_.rawBody);
            }

            const PinId self = PinId::make(n_.id, pin);
            if (const auto src = c_.ctx_.graph.sourceOf(self))
                return c_.pinValue(*src);

            for (const auto& [k, v] : n_.values)
                if (k == pin && !v.empty()) return c_.ctx_.ast.rawExpr(v);
            if (const PinDesc* pd = d_.findPin(pin);
                pd && !pd->defaultValue.empty())
                return c_.ctx_.ast.rawExpr(pd->defaultValue);
            return ExprRef{};
        }

        bool hasInput(std::string_view pin) const override {
            return input(pin).valid();
        }

    private:
        Compiler&       c_;
        const Node&     n_;
        const NodeDesc& d_;
    };

    ExprRef buildValue(PinId out) {
        const Node* node = ctx_.graph.node(out.node);
        const NodeDesc* d = descOf(out.node);
        if (!node || !d) return ctx_.ast.nil();

        const IEmitter* em = ctx_.emitters.find(d->emit);
        if (!em) {
            diag_.error("no emitter for kind '" + d->emit + "' (node " + d->id + ")",
                        out.node);
            return ctx_.ast.nil();
        }
        Inputs in(*this, *node, *d);
        EmitContext ec{ctx_.ast, *d, in, out.node, diag_};
        return em->emit(ec).value;
    }

    StmtRef emitEvent(const Node& n) {
        const NodeDesc* d = ctx_.nodes.find(n.type);
        if (!d) return {};

        std::vector<std::string> params;
        for (const PinDesc& p : d->pins) {
            if (p.dir != PinDir::Out || p.kind != PinKind::Data) continue;
            const std::string nm = uniqueName(sanitiseIdent(p.name));
            params.push_back(nm);
            bound_[PinId::make(n.id, p.name)] = ctx_.ast.name(nm);
        }

        emitted_.clear();
        onPath_.clear();
        needLabel_.clear();

        discover(execTarget(n.id, "out"), NodeId{});
        emitted_.clear();
        onPath_.clear();

        std::vector<StmtRef> body;
        walk(execTarget(n.id, "out"), NodeId{}, body);

        const StmtRef fnBody = ctx_.ast.block(body);
        const ExprRef fn = ctx_.ast.function(params, fnBody);

        ExprRef receiver;
        if (const PinDesc* selfPin = d->findPin("self");
            selfPin && selfPin->dir == PinDir::In) {
            Inputs in(*this, n, *d);
            const ExprRef self = in.input("self");
            receiver = self.valid()
                           ? ctx_.ast.field(self, d->target)
                           : ctx_.ast.rawExpr(d->target);
        } else {
            receiver = pathOf(d->target);
        }

        const ExprRef args[] = {fn};
        ctx_.ast.attribute(n.id);
        return ctx_.ast.exprStat(ctx_.ast.methodCall(receiver, "hook", args));
    }

    StmtRef emitFunction(const Node& n, const NodeDesc& d) {
        const std::size_t colon = d.target.find(':');
        const std::string fname =
            colon == std::string::npos ? d.target : d.target.substr(colon + 1);

        std::vector<std::string> params;
        for (const PinDesc& p : d.pins) {
            if (p.dir != PinDir::Out || p.kind != PinKind::Data) continue;
            const std::string nm = uniqueName(sanitiseIdent(p.name));
            params.push_back(nm);
            bound_[PinId::make(n.id, p.name)] = ctx_.ast.name(nm);
        }

        emitted_.clear();
        onPath_.clear();
        needLabel_.clear();
        discover(execTarget(n.id, "out"), NodeId{});
        emitted_.clear();
        onPath_.clear();

        std::vector<StmtRef> fnBody;
        walk(execTarget(n.id, "out"), NodeId{}, fnBody);

        ctx_.ast.attribute(n.id);
        return ctx_.ast.functionStat("M." + fname, params, ctx_.ast.block(fnBody));
    }

    StmtRef emitBehaviour(const Node& n, const NodeDesc& d) {
        std::vector<std::string> params;
        for (const PinDesc& p : d.pins) {
            if (p.dir != PinDir::Out || p.kind != PinKind::Data) continue;
            const std::string nm =
                p.name == "self" ? std::string("self")
                                 : uniqueName(sanitiseIdent(p.name));
            params.push_back(nm);
            bound_[PinId::make(n.id, p.name)] = ctx_.ast.name(nm);
        }

        emitted_.clear();
        onPath_.clear();
        needLabel_.clear();
        discover(execTarget(n.id, "out"), NodeId{});
        emitted_.clear();
        onPath_.clear();

        std::vector<StmtRef> fnBody;
        walk(execTarget(n.id, "out"), NodeId{}, fnBody);

        ctx_.ast.attribute(n.id);
        return ctx_.ast.functionStat("M." + d.target, params,
                                     ctx_.ast.block(fnBody));
    }

    ExprRef graphCallExpr(NodeId cur, const NodeDesc& d) {
        const std::size_t colon = d.target.find(':');
        const std::string module =
            colon == std::string::npos ? std::string() : d.target.substr(0, colon);
        const std::string fname =
            colon == std::string::npos ? d.target : d.target.substr(colon + 1);

        const Node* n = ctx_.graph.node(cur);
        if (!n) return ctx_.ast.nil();
        Inputs in(*this, *n, d);

        std::vector<ExprRef> args;
        for (const PinDesc& p : d.pins) {
            if (p.dir != PinDir::In || p.kind != PinKind::Data) continue;
            const ExprRef a = in.input(p.name);
            args.push_back(a.valid() ? a : ctx_.ast.nil());
        }

        ExprRef recv;
        if (module.empty() || module == ctx_.graph.moduleName) {
            recv = ctx_.ast.name("M");
        } else {
            const std::string alias = aliasFor(module);
            requires_[module] = alias;
            recv = ctx_.ast.name(alias);
        }
        return ctx_.ast.call(ctx_.ast.field(recv, fname), args);
    }

    std::string aliasFor(const std::string& module) {
        if (const auto it = requires_.find(module); it != requires_.end())
            return it->second;
        const std::size_t dot = module.find_last_of('.');
        return uniqueName(sanitiseIdent(
            dot == std::string::npos ? module : module.substr(dot + 1)));
    }

    ExprRef pathOf(std::string_view path) {
        std::size_t start = 0;
        ExprRef e{};
        while (start <= path.size()) {
            const std::size_t dot = path.find('.', start);
            const std::string_view part = path.substr(
                start, dot == std::string_view::npos ? std::string_view::npos
                                                     : dot - start);
            if (!part.empty()) e = e.valid() ? ctx_.ast.field(e, part) : ctx_.ast.name(part);
            if (dot == std::string_view::npos) break;
            start = dot + 1;
        }
        return e;
    }

    void discover(NodeId cur, NodeId stop) {
        while (cur.valid() && !(stop.valid() && cur == stop)) {
            if (onPath_.count(cur) || emitted_.count(cur)) {
                needLabel_.insert(cur);
                return;
            }
            const NodeDesc* d = descOf(cur);
            if (!d) return;

            onPath_.insert(cur);
            emitted_.insert(cur);
            const std::string& k = d->emit;

            if (k == "struct:branch") {
                const NodeId t = execTarget(cur, "true");
                const NodeId f = execTarget(cur, "false");
                const NodeId join = findJoin(t, f, stop);
                discover(t, join.valid() ? join : stop);
                discover(f, join.valid() ? join : stop);
                onPath_.erase(cur);
                cur = join;
                continue;
            }
            if (k == "struct:while" || k == "struct:fornum" || k == "struct:forin") {
                discover(execTarget(cur, "body"), cur);
                onPath_.erase(cur);
                cur = execTarget(cur, "out");
                continue;
            }
            if (k == "struct:sequence") {
                for (const PinDesc& p : d->pins)
                    if (p.dir == PinDir::Out && p.kind == PinKind::Exec)
                        discover(execTarget(cur, p.name), stop);
                onPath_.erase(cur);
                return;
            }
            if (k == "struct:break" || k == "struct:return") {
                onPath_.erase(cur);
                return;
            }

            onPath_.erase(cur);
            cur = execTarget(cur, "out");
        }
    }

    void walk(NodeId cur, NodeId stop, std::vector<StmtRef>& out) {
        while (cur.valid() && !(stop.valid() && cur == stop)) {
            if (onPath_.count(cur)) {
                needLabel_.insert(cur);
                out.push_back(ctx_.ast.gotoStat(labelFor(cur)));
                ++gotoCount_;
                return;
            }
            if (emitted_.count(cur)) {
                needLabel_.insert(cur);
                out.push_back(ctx_.ast.gotoStat(labelFor(cur)));
                ++gotoCount_;
                return;
            }

            const NodeDesc* d = descOf(cur);
            if (!d) {
                diag_.error("unknown node type in exec chain", cur);
                return;
            }

            onPath_.insert(cur);
            emitted_.insert(cur);
            if (needLabel_.count(cur)) out.push_back(ctx_.ast.labelStat(labelFor(cur)));

            const NodeId next = emitOne(cur, *d, stop, out);
            onPath_.erase(cur);
            cur = next;
        }
    }

    NodeId emitOne(NodeId cur, const NodeDesc& d, NodeId stop,
                   std::vector<StmtRef>& out) {
        const std::string& k = d.emit;

        if (k == "struct:branch")   return emitBranch(cur, d, stop, out);
        if (k == "struct:while")    return emitWhile(cur, d, out);
        if (k == "struct:fornum")   return emitNumericFor(cur, d, out);
        if (k == "struct:forin")    return emitGenericFor(cur, d, out);
        if (k == "struct:sequence") {
            for (const PinDesc& p : d.pins)
                if (p.dir == PinDir::Out && p.kind == PinKind::Exec)
                    walk(execTarget(cur, p.name), stop, out);
            return {};
        }
        if (k == "struct:break") {
            ctx_.ast.attribute(cur);
            out.push_back(ctx_.ast.breakStat());
            return {};
        }
        if (k == "struct:return") {
            const Node* n = ctx_.graph.node(cur);
            std::vector<ExprRef> vals;
            if (n) {
                Inputs in(*this, *n, d);
                if (const ExprRef v = in.input("value"); v.valid()) vals.push_back(v);
            }
            flushHoists(out);
            ctx_.ast.attribute(cur);
            out.push_back(ctx_.ast.returnStat(vals));
            return {};
        }

        if (k == "graphcall") {
            flushHoists(out);
            const ExprRef call = graphCallExpr(cur, d);
            ctx_.ast.attribute(cur);
            const PinId ret = PinId::make(cur, "ret");
            if (d.findPin("ret") && !ctx_.graph.targetsOf(ret).empty()) {
                const std::string nm = uniqueName(nameHint(ret));
                const std::string names[] = {nm};
                const ExprRef vals[] = {call};
                out.push_back(ctx_.ast.localDecl(names, vals));
                bound_[ret] = ctx_.ast.name(nm);
            } else {
                out.push_back(ctx_.ast.exprStat(call));
            }
            return execTarget(cur, "out");
        }

        emitPlain(cur, d, out);
        return execTarget(cur, "out");
    }

    void emitPlain(NodeId cur, const NodeDesc& d, std::vector<StmtRef>& out) {
        const Node* n = ctx_.graph.node(cur);
        if (!n) return;
        const IEmitter* em = ctx_.emitters.find(d.emit);
        if (!em) {
            diag_.error("no emitter for kind '" + d.emit + "'", cur);
            return;
        }

        Inputs in(*this, *n, d);
        EmitContext ec{ctx_.ast, d, in, cur, diag_};
        const EmitResult r = em->emit(ec);
        flushHoists(out);
        ctx_.ast.attribute(cur);

        PinId ret{};
        for (const PinDesc& p : d.pins)
            if (p.dir == PinDir::Out && p.kind == PinKind::Data) {
                ret = PinId::make(cur, p.name);
                break;
            }

        if (ret.valid() && !ctx_.graph.targetsOf(ret).empty() && r.value.valid()) {
            const std::string nm = uniqueName(nameHint(ret));
            const std::string names[] = {nm};
            const ExprRef vals[] = {r.value};
            out.push_back(ctx_.ast.localDecl(names, vals));
            bound_[ret] = ctx_.ast.name(nm);
        } else if (r.stmt.valid()) {
            out.push_back(r.stmt);
        } else if (r.value.valid()) {
            out.push_back(ctx_.ast.exprStat(r.value));
        }
    }

    NodeId emitBranch(NodeId cur, const NodeDesc& d, NodeId stop,
                      std::vector<StmtRef>& out) {
        const Node* n = ctx_.graph.node(cur);
        if (!n) return {};
        Inputs in(*this, *n, d);
        ExprRef cond = in.input("cond");
        if (!cond.valid()) cond = ctx_.ast.boolean(false);

        const NodeId t = execTarget(cur, "true");
        const NodeId f = execTarget(cur, "false");

        const NodeId join = findJoin(t, f, stop);

        flushHoists(out);

        std::vector<StmtRef> thenB, elseB;
        walk(t, join.valid() ? join : stop, thenB);
        walk(f, join.valid() ? join : stop, elseB);

        ctx_.ast.attribute(cur);
        if (elseB.empty()) {
            const std::pair<ExprRef, StmtRef> arms[] = {{cond, ctx_.ast.block(thenB)}};
            out.push_back(ctx_.ast.ifStat(arms, StmtRef{}));
        } else if (thenB.empty()) {
            const std::pair<ExprRef, StmtRef> arms[] = {
                {ctx_.ast.unop("not", cond), ctx_.ast.block(elseB)}};
            out.push_back(ctx_.ast.ifStat(arms, StmtRef{}));
        } else {
            const std::pair<ExprRef, StmtRef> arms[] = {{cond, ctx_.ast.block(thenB)}};
            out.push_back(ctx_.ast.ifStat(arms, ctx_.ast.block(elseB)));
        }
        return join;
    }

    NodeId findJoin(NodeId a, NodeId b, NodeId stop) const {
        if (!a.valid() || !b.valid()) return {};
        std::set<NodeId> fromB;
        collectReachable(b, stop, fromB);

        std::set<NodeId> seen;
        NodeId cur = a;
        while (cur.valid() && !seen.count(cur)) {
            if (stop.valid() && cur == stop) break;
            if (fromB.count(cur)) return cur;
            seen.insert(cur);
            cur = linearNext(cur);
        }
        return {};
    }

    NodeId linearNext(NodeId n) const {
        const NodeDesc* d = descOf(n);
        if (!d) return {};
        if (d->emit == "struct:branch" || d->emit == "struct:sequence") return {};
        if (d->emit == "struct:break" || d->emit == "struct:return") return {};
        return execTarget(n, "out");
    }

    void collectReachable(NodeId n, NodeId stop, std::set<NodeId>& out) const {
        if (!n.valid() || out.count(n)) return;
        if (stop.valid() && n == stop) { out.insert(n); return; }
        out.insert(n);
        const NodeDesc* d = descOf(n);
        if (!d) return;
        for (const PinDesc& p : d->pins)
            if (p.dir == PinDir::Out && p.kind == PinKind::Exec)
                collectReachable(execTarget(n, p.name), stop, out);
    }

    NodeId emitWhile(NodeId cur, const NodeDesc& d, std::vector<StmtRef>& out) {
        const Node* n = ctx_.graph.node(cur);
        if (!n) return {};
        Inputs in(*this, *n, d);
        ExprRef cond = in.input("cond");
        if (!cond.valid()) cond = ctx_.ast.boolean(false);

        flushHoists(out);
        std::vector<StmtRef> body;
        walk(execTarget(cur, "body"), cur, body);

        ctx_.ast.attribute(cur);
        out.push_back(ctx_.ast.whileStat(cond, ctx_.ast.block(body)));
        return execTarget(cur, "out");
    }

    NodeId emitNumericFor(NodeId cur, const NodeDesc& d, std::vector<StmtRef>& out) {
        const Node* n = ctx_.graph.node(cur);
        if (!n) return {};
        Inputs in(*this, *n, d);
        const ExprRef from = in.input("from");
        const ExprRef to = in.input("to");
        const ExprRef step = in.input("step");

        flushHoists(out);
        const std::string var = uniqueName("i");
        bound_[PinId::make(cur, "i")] = ctx_.ast.name(var);

        std::vector<StmtRef> body;
        walk(execTarget(cur, "body"), cur, body);

        ctx_.ast.attribute(cur);
        out.push_back(ctx_.ast.numericFor(
            var, from.valid() ? from : ctx_.ast.number(1),
            to.valid() ? to : ctx_.ast.number(1), step, ctx_.ast.block(body)));
        return execTarget(cur, "out");
    }

    NodeId emitGenericFor(NodeId cur, const NodeDesc& d, std::vector<StmtRef>& out) {
        const Node* n = ctx_.graph.node(cur);
        if (!n) return {};
        Inputs in(*this, *n, d);
        ExprRef tbl = in.input("table");
        if (!tbl.valid()) tbl = ctx_.ast.rawExpr("{}");

        flushHoists(out);
        const std::string kv = uniqueName("k");
        const std::string vv = uniqueName("v");
        bound_[PinId::make(cur, "k")] = ctx_.ast.name(kv);
        bound_[PinId::make(cur, "v")] = ctx_.ast.name(vv);

        std::vector<StmtRef> body;
        walk(execTarget(cur, "body"), cur, body);

        const ExprRef pairsArgs[] = {tbl};
        const ExprRef iter = ctx_.ast.call(ctx_.ast.name("pairs"), pairsArgs);
        const std::string vars[] = {kv, vv};
        const ExprRef exprs[] = {iter};

        ctx_.ast.attribute(cur);
        out.push_back(ctx_.ast.genericFor(vars, exprs, ctx_.ast.block(body)));
        return execTarget(cur, "out");
    }

    void flushHoists(std::vector<StmtRef>& out) {
        if (pendingHoists_.empty()) return;
        out.insert(out.end(), pendingHoists_.begin(), pendingHoists_.end());
        pendingHoists_.clear();
    }

    CompileContext& ctx_;
    Diagnostics&    diag_;

    std::map<PinId, ExprRef, bool (*)(const PinId&, const PinId&)> bound_{
        +[](const PinId& a, const PinId& b) {
            return a.node.v != b.node.v ? a.node.v < b.node.v : a.pin.v < b.pin.v;
        }};
    std::map<PinId, ExprRef, bool (*)(const PinId&, const PinId&)> hoisted_{
        +[](const PinId& a, const PinId& b) {
            return a.node.v != b.node.v ? a.node.v < b.node.v : a.pin.v < b.pin.v;
        }};

    std::vector<StmtRef>  pendingHoists_;
    std::set<NodeId>      onPath_;
    std::set<NodeId>      emitted_;
    std::set<NodeId>      needLabel_;
    std::set<std::string> used_;
    std::map<std::string, std::string> requires_;
    int                   gotoCount_ = 0;
};

class ValidatePass final : public IPass {
public:
    std::string_view name() const override { return "validate"; }

private:
    static std::set<NodeId> participating(const CompileContext& ctx) {
        std::set<NodeId> live;
        std::vector<NodeId> work;

        for (const Node& n : ctx.graph.nodes()) {
            const NodeDesc* d = ctx.nodes.find(n.type);
            if (d && d->isEvent) { live.insert(n.id); work.push_back(n.id); }
        }

        while (!work.empty()) {
            const NodeId cur = work.back();
            work.pop_back();
            const Node* n = ctx.graph.node(cur);
            const NodeDesc* d = n ? ctx.nodes.find(n->type) : nullptr;
            if (!d) continue;

            for (const PinDesc& p : d->pins) {
                if (p.kind == PinKind::Exec && p.dir == PinDir::Out) {
                    if (const auto t = ctx.graph.execTargetOf(PinId::make(cur, p.name)))
                        if (live.insert(t->node).second) work.push_back(t->node);
                } else if (p.kind == PinKind::Data && p.dir == PinDir::In) {
                    if (const auto s = ctx.graph.sourceOf(PinId::make(cur, p.name)))
                        if (live.insert(s->node).second) work.push_back(s->node);
                }
            }
        }
        return live;
    }

public:

    bool run(CompileContext& ctx, Diagnostics& diag) override {
        const std::set<NodeId> live = participating(ctx);

        for (const Node& n : ctx.graph.nodes()) {
            if (!live.count(n.id)) continue;
            const NodeDesc* d = ctx.nodes.find(n.type);
            if (!d) {
                diag.error("no provider defines node type '" + n.type + "'", n.id);
                continue;
            }
            for (const PinDesc& p : d->pins) {
                if (p.dir != PinDir::In || p.kind != PinKind::Data) continue;
                if (p.optional) continue;
                const PinId pin = PinId::make(n.id, p.name);
                if (ctx.graph.sourceOf(pin)) continue;
                bool literal = false;
                for (const auto& [k, v] : n.values)
                    if (k == p.name && !v.empty()) literal = true;
                if (!literal && p.defaultValue.empty())
                    diag.error("required input '" + p.name + "' of '" + d->id
                               + "' is not connected", n.id);
            }
        }

        for (const Link& l : ctx.graph.links()) {
            if (l.kind != PinKind::Exec) continue;
            const Node* fn = ctx.graph.node(l.from.node);
            const Node* tn = ctx.graph.node(l.to.node);
            const NodeDesc* fd = fn ? ctx.nodes.find(fn->type) : nullptr;
            const NodeDesc* td = tn ? ctx.nodes.find(tn->type) : nullptr;
            if (!fd || !td) continue;

            const PinDesc* fp = fd->findPin(l.from.pin.str());
            const PinDesc* tp = td->findPin(l.to.pin.str());
            if (!fp || fp->kind != PinKind::Exec || fp->dir != PinDir::Out)
                diag.error("'" + fd->id + "' has no exec output named '"
                               + std::string(l.from.pin.str()) + "'",
                           l.from.node);
            if (!tp || tp->kind != PinKind::Exec || tp->dir != PinDir::In)
                diag.error("'" + td->id + "' has no exec input named '"
                               + std::string(l.to.pin.str())
                               + "' - it cannot be sequenced",
                           l.to.node);
        }

        for (const Link& l : ctx.graph.links()) {
            if (l.kind != PinKind::Data) continue;
            const NodeDesc* fd = ctx.nodes.find(
                ctx.graph.node(l.from.node) ? ctx.graph.node(l.from.node)->type : "");
            const NodeDesc* td = ctx.nodes.find(
                ctx.graph.node(l.to.node) ? ctx.graph.node(l.to.node)->type : "");
            if (!fd || !td) continue;
            const PinDesc* fp = fd->findPin(l.from.pin.str());
            const PinDesc* tp = td->findPin(l.to.pin.str());
            if (!fp || !tp) continue;
            if (!ctx.types.canConnect(fp->type, tp->type))
                diag.error("cannot connect " + std::string(ctx.types.get(fp->type).name)
                               + " to " + std::string(ctx.types.get(tp->type).name),
                           l.to.node);
        }

        std::set<NodeId> visiting, done;
        std::function<bool(NodeId)> visit = [&](NodeId n) -> bool {
            if (done.count(n)) return true;
            if (!visiting.insert(n).second) {
                diag.error("data cycle detected", n);
                return false;
            }
            const Node* node = ctx.graph.node(n);
            const NodeDesc* d = node ? ctx.nodes.find(node->type) : nullptr;
            if (d && d->pure)
                for (const PinDesc& p : d->pins) {
                    if (p.dir != PinDir::In || p.kind != PinKind::Data) continue;
                    if (const auto s = ctx.graph.sourceOf(PinId::make(n, p.name)))
                        if (!visit(s->node)) return false;
                }
            visiting.erase(n);
            done.insert(n);
            return true;
        };
        for (const Node& n : ctx.graph.nodes())
            if (!visit(n.id)) break;

        return !diag.hasErrors();
    }
};

class EmitPass final : public IPass {
public:
    std::string_view name() const override { return "emit"; }
    bool run(CompileContext& ctx, Diagnostics& diag) override {
        Compiler c(ctx, diag);
        return c.run();
    }
};

}

void PassPipeline::add(std::unique_ptr<IPass> p) { passes_.push_back(std::move(p)); }

bool PassPipeline::run(CompileContext& ctx, Diagnostics& diag) {
    for (auto& p : passes_)
        if (!p->run(ctx, diag)) return false;
    return true;
}

PassPipeline PassPipeline::standard() {
    PassPipeline p;
    p.add(std::make_unique<ValidatePass>());
    p.add(std::make_unique<EmitPass>());
    return p;
}

CompileResult compileGraph(const Graph& g, const NodeRegistry& nodes,
                           const TypeRegistry& types,
                           const EmitterRegistry& emitters,
                           std::string sourceName, Diagnostics& diag) {
    CompileResult result;
    CompileContext ctx{g, nodes, types, emitters, {}, {}, {}, sourceName, 0};

    PassPipeline pipeline = PassPipeline::standard();
    if (!pipeline.run(ctx, diag)) return result;

    PrintOptions opts;
    opts.headerSourceName = std::move(sourceName);
    result.lua = printLua(ctx.ast, ctx.root, opts, &ctx.map);
    result.map = std::move(ctx.map);
    result.gotoCount = ctx.gotoCount;
    result.ok = true;
    return result;
}

}
