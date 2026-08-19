#pragma once

#include "scene/scene.h"

namespace lime {

class ComponentFileProvider final : public IComponentProvider {
public:
    ComponentFileProvider(std::string path, std::string name, int priority);

    std::string_view name() const override { return name_; }
    int priority() const override { return priority_; }
    void collect(TypeRegistry& types, std::vector<ComponentDesc>& out,
                 Diagnostics& diag) override;

private:
    std::string path_;
    std::string name_;
    int         priority_ = 0;
};

inline constexpr int kComponentPriorityCore      = 20;
inline constexpr int kComponentPriorityProject   = 25;
inline constexpr int kComponentPriorityOverrides = 30;

}
