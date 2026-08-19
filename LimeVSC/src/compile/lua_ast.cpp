#include "limecore.h"

namespace lime {

using EK = LuaAst::EK;
using SK = LuaAst::SK;

namespace {
ExprRef mkExpr(std::vector<LuaAst::ExprNode>& v, LuaAst::ExprNode n) {
    v.push_back(std::move(n));
    return ExprRef{static_cast<std::uint32_t>(v.size() - 1)};
}
}

ExprRef LuaAst::name(std::string_view ident) {
    ExprNode n; n.kind = EK::Name; n.text = std::string(ident);
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::nil() {
    ExprNode n; n.kind = EK::Nil;
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::boolean(bool b) {
    ExprNode n; n.kind = EK::Bool; n.b = b;
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::number(double d) {
    ExprNode n; n.kind = EK::Number; n.num = d;
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::string(std::string_view s) {
    ExprNode n; n.kind = EK::String; n.text = std::string(s);
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::rawExpr(std::string_view lua) {
    ExprNode n; n.kind = EK::Raw; n.text = std::string(lua);
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::binop(std::string_view op, ExprRef a, ExprRef b) {
    ExprNode n; n.kind = EK::Binop; n.text = std::string(op); n.kids = {a, b};
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::unop(std::string_view op, ExprRef a) {
    ExprNode n; n.kind = EK::Unop; n.text = std::string(op); n.kids = {a};
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::field(ExprRef obj, std::string_view f) {
    ExprNode n; n.kind = EK::Field; n.text = std::string(f); n.kids = {obj};
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::indexExpr(ExprRef obj, ExprRef key) {
    ExprNode n; n.kind = EK::Index; n.kids = {obj, key};
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::call(ExprRef fn, std::span<const ExprRef> args) {
    ExprNode n; n.kind = EK::Call; n.kids.push_back(fn);
    n.kids.insert(n.kids.end(), args.begin(), args.end());
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::methodCall(ExprRef obj, std::string_view m,
                           std::span<const ExprRef> args) {
    ExprNode n; n.kind = EK::Method; n.text = std::string(m);
    n.kids.push_back(obj);
    n.kids.insert(n.kids.end(), args.begin(), args.end());
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::function(std::span<const std::string> params, StmtRef body) {
    ExprNode n; n.kind = EK::Function;
    n.params.assign(params.begin(), params.end());
    n.body = body;
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::table(std::span<const std::pair<std::string, ExprRef>> fields) {
    ExprNode n; n.kind = EK::Table;
    for (const auto& [k, v] : fields) { n.params.push_back(k); n.kids.push_back(v); }
    return mkExpr(exprs_, std::move(n));
}
ExprRef LuaAst::varargs() {
    ExprNode n; n.kind = EK::Varargs;
    return mkExpr(exprs_, std::move(n));
}

StmtRef LuaAst::push(StmtNode n) {
    n.attr = pendingAttr_;
    pendingAttr_ = NodeId{};
    stmts_.push_back(std::move(n));
    return StmtRef{static_cast<std::uint32_t>(stmts_.size() - 1)};
}

StmtRef LuaAst::block(std::span<const StmtRef> s) {
    StmtNode n; n.kind = SK::Block; n.kids.assign(s.begin(), s.end());
    return push(std::move(n));
}
StmtRef LuaAst::exprStat(ExprRef e) {
    StmtNode n; n.kind = SK::ExprStat; n.exprs = {e};
    return push(std::move(n));
}
StmtRef LuaAst::localDecl(std::span<const std::string> names,
                          std::span<const ExprRef> values) {
    StmtNode n; n.kind = SK::Local;
    n.names.assign(names.begin(), names.end());
    n.exprs.assign(values.begin(), values.end());
    return push(std::move(n));
}
StmtRef LuaAst::assign(std::span<const ExprRef> targets,
                       std::span<const ExprRef> values) {
    StmtNode n; n.kind = SK::Assign;
    n.exprs.assign(targets.begin(), targets.end());
    n.count = static_cast<int>(targets.size());
    n.exprs.insert(n.exprs.end(), values.begin(), values.end());
    return push(std::move(n));
}
StmtRef LuaAst::ifStat(std::span<const std::pair<ExprRef, StmtRef>> arms,
                       StmtRef elseBlock) {
    StmtNode n; n.kind = SK::If;
    for (const auto& [c, b] : arms) { n.exprs.push_back(c); n.kids.push_back(b); }
    if (elseBlock.valid()) n.kids.push_back(elseBlock);
    return push(std::move(n));
}
StmtRef LuaAst::whileStat(ExprRef cond, StmtRef body) {
    StmtNode n; n.kind = SK::While; n.exprs = {cond}; n.kids = {body};
    return push(std::move(n));
}
StmtRef LuaAst::repeatStat(StmtRef body, ExprRef until) {
    StmtNode n; n.kind = SK::Repeat; n.exprs = {until}; n.kids = {body};
    return push(std::move(n));
}
StmtRef LuaAst::numericFor(std::string_view var, ExprRef from, ExprRef to,
                           ExprRef step, StmtRef body) {
    StmtNode n; n.kind = SK::NumFor; n.names = {std::string(var)};
    n.exprs = {from, to};
    if (step.valid()) n.exprs.push_back(step);
    n.kids = {body};
    return push(std::move(n));
}
StmtRef LuaAst::genericFor(std::span<const std::string> vars,
                           std::span<const ExprRef> exprs, StmtRef body) {
    StmtNode n; n.kind = SK::GenFor;
    n.names.assign(vars.begin(), vars.end());
    n.exprs.assign(exprs.begin(), exprs.end());
    n.kids = {body};
    return push(std::move(n));
}
StmtRef LuaAst::returnStat(std::span<const ExprRef> values) {
    StmtNode n; n.kind = SK::Return; n.exprs.assign(values.begin(), values.end());
    return push(std::move(n));
}
StmtRef LuaAst::breakStat() {
    StmtNode n; n.kind = SK::Break;
    return push(std::move(n));
}
StmtRef LuaAst::gotoStat(std::string_view label) {
    StmtNode n; n.kind = SK::Goto; n.text = std::string(label);
    return push(std::move(n));
}
StmtRef LuaAst::labelStat(std::string_view label) {
    StmtNode n; n.kind = SK::Label; n.text = std::string(label);
    return push(std::move(n));
}
StmtRef LuaAst::localFunction(std::string_view fname,
                              std::span<const std::string> params, StmtRef body) {
    StmtNode n; n.kind = SK::LocalFunc; n.text = std::string(fname);
    n.names.assign(params.begin(), params.end());
    n.kids = {body};
    return push(std::move(n));
}
StmtRef LuaAst::functionStat(std::string_view qualifiedName,
                             std::span<const std::string> params, StmtRef body) {
    StmtNode n; n.kind = SK::Func; n.text = std::string(qualifiedName);
    n.names.assign(params.begin(), params.end());
    n.kids = {body};
    return push(std::move(n));
}

StmtRef LuaAst::comment(std::string_view text) {
    StmtNode n; n.kind = SK::Comment; n.text = std::string(text);
    return push(std::move(n));
}
StmtRef LuaAst::blankLine() {
    StmtNode n; n.kind = SK::Blank;
    return push(std::move(n));
}

void LuaAst::attribute(NodeId n) { pendingAttr_ = n; }

const LuaAst::ExprNode& LuaAst::expr(ExprRef r) const {
    static const ExprNode kNil{};
    return r.valid() && r.v < exprs_.size() ? exprs_[r.v] : kNil;
}
const LuaAst::StmtNode& LuaAst::stmt(StmtRef r) const {
    static const StmtNode kNil{};
    return r.valid() && r.v < stmts_.size() ? stmts_[r.v] : kNil;
}

NodeId SourceMap::nodeForLine(int line) const {
    NodeId best{};
    for (const auto& [l, n] : lines) {
        if (l > line) break;
        best = n;
    }
    return best;
}

}
