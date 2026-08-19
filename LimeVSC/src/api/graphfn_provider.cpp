#include "api/graphfn_provider.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lime {

std::string graphFnCallId(std::string_view module, std::string_view fn) {
    return "fn." + std::string(module) + "." + std::string(fn);
}
std::string graphFnEntryId(std::string_view module, std::string_view fn) {
    return graphFnCallId(module, fn) + ".entry";
}

std::string graphPropGetId(std::string_view module, std::string_view prop) {
    return "prop." + std::string(module) + "." + std::string(prop);
}
std::string graphPropSetId(std::string_view module, std::string_view prop) {
    return graphPropGetId(module, prop) + ".set";
}

std::string graphVarGetId(std::string_view module, std::string_view var) {
    return "var." + std::string(module) + "." + std::string(var);
}
std::string graphVarSetId(std::string_view module, std::string_view var) {
    return graphVarGetId(module, var) + ".set";
}

GraphFnProvider::GraphFnProvider(std::string projectRoot, int priority)
    : root_(std::move(projectRoot)), priority_(priority) {}

bool GraphFnProvider::readSignatures(const std::string& path,
                                     std::string& moduleOut,
                                     std::vector<FnDecl>& fnsOut) {
    std::vector<PropDecl> ignored;
    return readSignatures(path, moduleOut, fnsOut, ignored);
}

bool GraphFnProvider::readSignatures(const std::string& path,
                                     std::string& moduleOut,
                                     std::vector<FnDecl>& fnsOut,
                                     std::vector<PropDecl>& propsOut) {
    std::vector<VarDecl> ignored;
    return readSignatures(path, moduleOut, fnsOut, propsOut, ignored);
}

bool GraphFnProvider::readSignatures(const std::string& path,
                                     std::string& moduleOut,
                                     std::vector<FnDecl>& fnsOut,
                                     std::vector<PropDecl>& propsOut,
                                     std::vector<VarDecl>& varsOut) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    moduleOut.clear();
    fnsOut.clear();
    propsOut.clear();
    varsOut.clear();
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;
        if (line[0] != '!') break;

        std::istringstream ls(line.substr(1));
        std::string key;
        ls >> key;
        if (key == "module") {
            ls >> moduleOut;
        } else if (key == "fn") {
            FnDecl fn;
            ls >> fn.name;
            std::string tok;
            while (ls >> tok) {
                if (tok == "->") { ls >> fn.ret; break; }
                FnParam p;
                const std::size_t colon = tok.find(':');
                if (colon == std::string::npos) {
                    p.name = tok;
                } else {
                    p.name = tok.substr(0, colon);
                    p.type = tok.substr(colon + 1);
                }
                fn.params.push_back(std::move(p));
            }
            if (!fn.name.empty()) fnsOut.push_back(std::move(fn));
        } else if (key == "prop") {
            PropDecl pd;
            ls >> pd.name >> pd.type;
            if (pd.type == "=") {
                pd.type = "any";
                std::getline(ls, pd.defaultValue);
            } else {
                std::string eq;
                if (ls >> eq && eq == "=") std::getline(ls, pd.defaultValue);
            }
            while (!pd.defaultValue.empty() && pd.defaultValue.front() == ' ')
                pd.defaultValue.erase(pd.defaultValue.begin());
            if (pd.type.empty()) pd.type = "any";
            if (!pd.name.empty()) propsOut.push_back(std::move(pd));
        } else if (key == "var") {
            VarDecl vd;
            ls >> vd.name >> vd.type;
            if (vd.type == "=") {
                vd.type = "any";
                std::getline(ls, vd.defaultValue);
            } else {
                std::string eq;
                if (ls >> eq && eq == "=") std::getline(ls, vd.defaultValue);
            }
            while (!vd.defaultValue.empty() && vd.defaultValue.front() == ' ')
                vd.defaultValue.erase(vd.defaultValue.begin());
            if (vd.type.empty()) vd.type = "any";
            if (!vd.name.empty()) varsOut.push_back(std::move(vd));
        }
    }
    return true;
}

void GraphFnProvider::collect(TypeRegistry& types, std::vector<NodeDesc>& out,
                              Diagnostics& diag) {
    std::error_code ec;
    const fs::path content = fs::path(root_) / "content";
    if (root_.empty() || !fs::exists(content, ec)) return;

    auto typeId = [&](const std::string& n) {
        return types.intern(TypeDesc{n.empty() ? "any" : n, 0, {}, false, {}});
    };

    for (auto it = fs::recursive_directory_iterator(content, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".lime") continue;

        std::string module;
        std::vector<FnDecl> fns;
        std::vector<PropDecl> props;
        std::vector<VarDecl> vars;
        if (!readSignatures(it->path().string(), module, fns, props, vars))
            continue;
        if (module.empty() || (fns.empty() && props.empty() && vars.empty()))
            continue;

        for (const VarDecl& vd : vars) {
            NodeDesc get;
            get.id = graphVarGetId(module, vd.name);
            get.display = "Get " + vd.name;
            get.category = "Variables/" + module;
            get.emit = "varget";
            get.target = vd.name;
            get.pure = true;
            get.doc = "Reads the graph variable " + vd.name + ".";
            {
                PinDesc o;
                o.name = "value"; o.dir = PinDir::Out; o.kind = PinKind::Data;
                o.type = typeId(vd.type);
                get.pins.push_back(std::move(o));
            }
            out.push_back(std::move(get));

            NodeDesc set;
            set.id = graphVarSetId(module, vd.name);
            set.display = "Set " + vd.name;
            set.category = "Variables/" + module;
            set.emit = "varset";
            set.target = vd.name;
            set.doc = "Writes the graph variable " + vd.name + ".";
            {
                PinDesc i;
                i.name = "in"; i.dir = PinDir::In; i.kind = PinKind::Exec;
                set.pins.push_back(std::move(i));
                PinDesc v;
                v.name = "value"; v.dir = PinDir::In; v.kind = PinKind::Data;
                v.type = typeId(vd.type);
                v.defaultValue = vd.defaultValue;
                set.pins.push_back(std::move(v));
                PinDesc o;
                o.name = "out"; o.dir = PinDir::Out; o.kind = PinKind::Exec;
                set.pins.push_back(std::move(o));
            }
            out.push_back(std::move(set));
        }

        for (const PropDecl& pd : props) {
            NodeDesc get;
            get.id = graphPropGetId(module, pd.name);
            get.display = "Get " + pd.name;
            get.category = "Properties/" + module;
            get.emit = "selffield";
            get.target = pd.name;
            get.pure = true;
            get.doc = "Reads this entity's " + pd.name + ".";
            {
                PinDesc o;
                o.name = "value"; o.dir = PinDir::Out; o.kind = PinKind::Data;
                o.type = typeId(pd.type);
                get.pins.push_back(std::move(o));
            }
            out.push_back(std::move(get));

            NodeDesc set;
            set.id = graphPropSetId(module, pd.name);
            set.display = "Set " + pd.name;
            set.category = "Properties/" + module;
            set.emit = "selfassign";
            set.target = pd.name;
            set.doc = "Writes this entity's " + pd.name + ".";
            {
                PinDesc i;
                i.name = "in"; i.dir = PinDir::In; i.kind = PinKind::Exec;
                set.pins.push_back(std::move(i));
                PinDesc v;
                v.name = "value"; v.dir = PinDir::In; v.kind = PinKind::Data;
                v.type = typeId(pd.type);
                v.defaultValue = pd.defaultValue;
                set.pins.push_back(std::move(v));
                PinDesc o;
                o.name = "out"; o.dir = PinDir::Out; o.kind = PinKind::Exec;
                set.pins.push_back(std::move(o));
            }
            out.push_back(std::move(set));
        }
        if (fns.empty()) continue;

        for (const FnDecl& fn : fns) {
            const std::string label = module + "." + fn.name;

            NodeDesc entry;
            entry.id = graphFnEntryId(module, fn.name);
            entry.display = fn.name + "  (entry)";
            entry.category = "Functions/" + module;
            entry.emit = "struct:fnentry";
            entry.target = module + ":" + fn.name;
            entry.isEvent = true;
            entry.doc = "Body of " + label + ".";
            {
                PinDesc o;
                o.name = "out"; o.dir = PinDir::Out; o.kind = PinKind::Exec;
                entry.pins.push_back(std::move(o));
            }
            for (const FnParam& p : fn.params) {
                PinDesc pin;
                pin.name = p.name;
                pin.dir = PinDir::Out;
                pin.kind = PinKind::Data;
                pin.type = typeId(p.type);
                entry.pins.push_back(std::move(pin));
            }
            out.push_back(std::move(entry));

            NodeDesc call;
            call.id = graphFnCallId(module, fn.name);
            call.display = fn.name;
            call.category = "Functions/" + module;
            call.emit = "graphcall";
            call.target = module + ":" + fn.name;
            call.doc = "Calls " + label + ".";
            {
                PinDesc in;
                in.name = "in"; in.dir = PinDir::In; in.kind = PinKind::Exec;
                call.pins.push_back(std::move(in));
            }
            for (const FnParam& p : fn.params) {
                PinDesc pin;
                pin.name = p.name;
                pin.dir = PinDir::In;
                pin.kind = PinKind::Data;
                pin.type = typeId(p.type);
                call.pins.push_back(std::move(pin));
            }
            if (!fn.ret.empty()) {
                PinDesc r;
                r.name = "ret"; r.dir = PinDir::Out; r.kind = PinKind::Data;
                r.type = typeId(fn.ret);
                call.pins.push_back(std::move(r));
            }
            {
                PinDesc o;
                o.name = "out"; o.dir = PinDir::Out; o.kind = PinKind::Exec;
                call.pins.push_back(std::move(o));
            }
            out.push_back(std::move(call));
        }
    }
    (void)diag;
}

}
