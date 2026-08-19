#include "limecore.h"

#include <cctype>
#include <vector>

namespace lime {
namespace {

ExprRef pathExpr(LuaAst& ast, std::string_view path) {
    std::size_t start = 0;
    ExprRef e{};
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string_view part =
            path.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                             : dot - start);
        if (!part.empty())
            e = e.valid() ? ast.field(e, part) : ast.name(part);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return e;
}

std::string varIdentImpl(std::string_view name) {
    std::string out = "v_";
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return out;
}

std::vector<ExprRef> gatherArgs(EmitContext& ctx, bool skipSelf) {
    std::vector<ExprRef> args;
    for (const PinDesc& p : ctx.desc.pins) {
        if (p.dir != PinDir::In || p.kind != PinKind::Data) continue;
        if (skipSelf && p.name == "self") continue;
        args.push_back(ctx.in.input(p.name));
    }
    while (!args.empty() && !args.back().valid()) args.pop_back();
    for (ExprRef& a : args) if (!a.valid()) a = ctx.ast.nil();
    return args;
}

EmitResult valueOrStatement(EmitContext& ctx, ExprRef value) {
    EmitResult r;
    r.value = value;
    if (ctx.desc.hasExecPins()) r.stmt = ctx.ast.exprStat(value);
    return r;
}

class CallEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "call"; }
    EmitResult emit(EmitContext& ctx) const override {
        const auto args = gatherArgs(ctx, false);
        return valueOrStatement(
            ctx, ctx.ast.call(pathExpr(ctx.ast, ctx.desc.target), args));
    }
};

class ConstructEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "construct"; }
    EmitResult emit(EmitContext& ctx) const override {
        const auto args = gatherArgs(ctx, false);
        EmitResult r;
        r.value = ctx.ast.call(pathExpr(ctx.ast, ctx.desc.target), args);
        return r;
    }
};

class MethodEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "method"; }
    EmitResult emit(EmitContext& ctx) const override {
        ExprRef self = ctx.in.input("self");
        if (!self.valid()) {
            ctx.diag.error("method node '" + ctx.desc.id + "' has no receiver",
                           ctx.node);
            self = ctx.ast.nil();
        }
        const auto args = gatherArgs(ctx, true);
        return valueOrStatement(ctx, ctx.ast.methodCall(self, ctx.desc.target, args));
    }
};

class IndexEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "index"; }
    EmitResult emit(EmitContext& ctx) const override {
        ExprRef self = ctx.in.input("self");
        if (!self.valid()) {
            ctx.diag.error("field node '" + ctx.desc.id + "' has no receiver",
                           ctx.node);
            self = ctx.ast.nil();
        }
        EmitResult r;
        r.value = ctx.ast.field(self, ctx.desc.target);
        return r;
    }
};

class SelfFieldEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "selffield"; }
    EmitResult emit(EmitContext& ctx) const override {
        EmitResult r;
        r.value = ctx.ast.field(ctx.ast.name("self"), ctx.desc.target);
        return r;
    }
};

class SelfAssignEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "selfassign"; }
    EmitResult emit(EmitContext& ctx) const override {
        ExprRef v = ctx.in.input("value");
        if (!v.valid()) v = ctx.ast.nil();
        const ExprRef target[] = {ctx.ast.field(ctx.ast.name("self"), ctx.desc.target)};
        const ExprRef value[] = {v};
        EmitResult r;
        r.stmt = ctx.ast.assign(target, value);
        return r;
    }
};

class VarGetEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "varget"; }
    EmitResult emit(EmitContext& ctx) const override {
        EmitResult r;
        r.value = ctx.ast.name(varIdent(ctx.desc.target));
        return r;
    }
};

class VarSetEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "varset"; }
    EmitResult emit(EmitContext& ctx) const override {
        ExprRef v = ctx.in.input("value");
        if (!v.valid()) v = ctx.ast.nil();
        const ExprRef target[] = {ctx.ast.name(varIdent(ctx.desc.target))};
        const ExprRef value[] = {v};
        EmitResult r;
        r.stmt = ctx.ast.assign(target, value);
        return r;
    }
};

class BinopEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "binop"; }
    EmitResult emit(EmitContext& ctx) const override {
        ExprRef a = ctx.in.input("a");
        ExprRef b = ctx.in.input("b");
        if (!a.valid()) a = ctx.ast.nil();
        if (!b.valid()) b = ctx.ast.nil();
        EmitResult r;
        r.value = ctx.ast.binop(ctx.desc.target, a, b);
        return r;
    }
};

class UnopEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "unop"; }
    EmitResult emit(EmitContext& ctx) const override {
        ExprRef a = ctx.in.input("a");
        if (!a.valid()) a = ctx.ast.nil();
        EmitResult r;
        r.value = ctx.ast.unop(ctx.desc.target, a);
        return r;
    }
};

class LiteralEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "literal"; }
    EmitResult emit(EmitContext& ctx) const override {
        EmitResult r;
        ExprRef v = ctx.in.input("value");
        r.value = v.valid() ? v : ctx.ast.nil();
        return r;
    }
};

class RerouteEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "reroute"; }
    EmitResult emit(EmitContext& ctx) const override {
        EmitResult r;
        const ExprRef v = ctx.in.input("in");
        r.value = v.valid() ? v : ctx.ast.nil();
        return r;
    }
};

class ExecRerouteEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "reroute.exec"; }
    EmitResult emit(EmitContext&) const override { return {}; }
};

class RawEmitter final : public IEmitter {
public:
    std::string_view key() const override { return "raw"; }
    EmitResult emit(EmitContext& ctx) const override {
        EmitResult r;
        ExprRef body = ctx.in.input("__body");
        if (!body.valid()) body = ctx.ast.rawExpr("");
        r.value = body;
        r.stmt = ctx.ast.exprStat(body);
        return r;
    }
};

}

void EmitterRegistry::add(std::unique_ptr<IEmitter> e) {
    emitters_.push_back(std::move(e));
}

const IEmitter* EmitterRegistry::find(std::string_view key) const {
    for (const auto& e : emitters_) if (e->key() == key) return e.get();
    return nullptr;
}

std::string varIdent(std::string_view name) { return varIdentImpl(name); }

EmitterRegistry EmitterRegistry::withBuiltins() {
    EmitterRegistry r;
    r.add(std::make_unique<CallEmitter>());
    r.add(std::make_unique<ConstructEmitter>());
    r.add(std::make_unique<MethodEmitter>());
    r.add(std::make_unique<IndexEmitter>());
    r.add(std::make_unique<SelfFieldEmitter>());
    r.add(std::make_unique<SelfAssignEmitter>());
    r.add(std::make_unique<VarGetEmitter>());
    r.add(std::make_unique<VarSetEmitter>());
    r.add(std::make_unique<BinopEmitter>());
    r.add(std::make_unique<UnopEmitter>());
    r.add(std::make_unique<LiteralEmitter>());
    r.add(std::make_unique<RawEmitter>());
    r.add(std::make_unique<RerouteEmitter>());
    r.add(std::make_unique<ExecRerouteEmitter>());
    return r;
}

}
