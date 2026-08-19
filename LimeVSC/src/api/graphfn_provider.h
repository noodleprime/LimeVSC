#pragma once

#include "limecore.h"

namespace lime {

class GraphFnProvider final : public INodeProvider {
public:
    explicit GraphFnProvider(std::string projectRoot, int priority);

    std::string_view name() const override { return "graphfn"; }
    int priority() const override { return priority_; }
    void collect(TypeRegistry& types, std::vector<NodeDesc>& out,
                 Diagnostics& diag) override;

    static bool readSignatures(const std::string& path, std::string& moduleOut,
                               std::vector<FnDecl>& fnsOut);
    static bool readSignatures(const std::string& path, std::string& moduleOut,
                               std::vector<FnDecl>& fnsOut,
                               std::vector<PropDecl>& propsOut);
    static bool readSignatures(const std::string& path, std::string& moduleOut,
                               std::vector<FnDecl>& fnsOut,
                               std::vector<PropDecl>& propsOut,
                               std::vector<VarDecl>& varsOut);

private:
    std::string root_;
    int         priority_ = 0;
};

std::string graphFnCallId(std::string_view module, std::string_view fn);
std::string graphFnEntryId(std::string_view module, std::string_view fn);
std::string graphPropGetId(std::string_view module, std::string_view prop);
std::string graphPropSetId(std::string_view module, std::string_view prop);
std::string graphVarGetId(std::string_view module, std::string_view var);
std::string graphVarSetId(std::string_view module, std::string_view var);

}
