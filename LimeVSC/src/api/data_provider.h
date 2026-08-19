#pragma once

#include "limecore.h"

namespace lime {

class DataFileProvider final : public INodeProvider {
public:
    DataFileProvider(std::string path, std::string name, int priority);

    std::string_view name() const override { return name_; }
    int priority() const override { return priority_; }
    void collect(TypeRegistry& types, std::vector<NodeDesc>& out,
                 Diagnostics& diag) override;

private:
    std::string path_;
    std::string name_;
    int         priority_ = 0;
};

inline constexpr int kPriorityGenerated = 10;
inline constexpr int kPriorityCore      = 20;
inline constexpr int kPriorityOverrides = 30;

}
