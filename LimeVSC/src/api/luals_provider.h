#pragma once

#include "limecore.h"

namespace lime {

class LuaLSProvider final : public INodeProvider {
public:
    LuaLSProvider(std::string apiPath, std::string enumsPath, int priority);

    std::string_view name() const override { return "luals"; }
    int priority() const override { return priority_; }
    void collect(TypeRegistry& types, std::vector<NodeDesc>& out,
                 Diagnostics& diag) override;

private:
    std::string apiPath_;
    std::string enumsPath_;
    int         priority_ = 0;
};

std::string eventClassToPath(std::string_view className);

std::string humanizeIdentifier(std::string_view ident);

std::string cleanDoc(std::string_view doc);

std::vector<std::pair<std::string, std::string>> parseFunSignature(std::string_view fun);

}
