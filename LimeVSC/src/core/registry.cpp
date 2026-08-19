#include "limecore.h"

#include <algorithm>

namespace lime {

const PinDesc* NodeDesc::findPin(std::string_view name) const {
    for (const PinDesc& p : pins) if (p.name == name) return &p;
    return nullptr;
}

bool NodeDesc::hasExecPins() const {
    for (const PinDesc& p : pins) if (p.kind == PinKind::Exec) return true;
    return false;
}

void NodeRegistry::addProvider(std::unique_ptr<INodeProvider> p) {
    providers_.push_back(std::move(p));
}

void NodeRegistry::rebuild(TypeRegistry& types, Diagnostics& diag) {
    descs_.clear();

    std::vector<INodeProvider*> ordered;
    ordered.reserve(providers_.size());
    for (auto& p : providers_) ordered.push_back(p.get());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const INodeProvider* a, const INodeProvider* b) {
                         return a->priority() < b->priority();
                     });

    for (INodeProvider* p : ordered) {
        std::vector<NodeDesc> batch;
        p->collect(types, batch, diag);
        for (NodeDesc& d : batch) {
            d.priority = p->priority();
            if (d.display.empty()) {
                const std::size_t dot = d.id.find_last_of('.');
                d.display = (dot == std::string::npos) ? d.id
                                                       : d.id.substr(dot + 1);
            }
            auto it = std::find_if(descs_.begin(), descs_.end(),
                                   [&](const NodeDesc& e) { return e.id == d.id; });
            if (it == descs_.end()) descs_.push_back(std::move(d));
            else *it = std::move(d);
        }
    }

    std::sort(descs_.begin(), descs_.end(),
              [](const NodeDesc& a, const NodeDesc& b) { return a.id < b.id; });

    browse_.clear();
    browse_.reserve(descs_.size());
    for (const NodeDesc& d : descs_) browse_.push_back(&d);
    std::sort(browse_.begin(), browse_.end(),
              [](const NodeDesc* a, const NodeDesc* b) {
                  if (a->category != b->category) return a->category < b->category;
                  if (a->display != b->display) return a->display < b->display;
                  return a->id < b->id;
              });
}

const NodeDesc* NodeRegistry::find(std::string_view id) const {
    auto it = std::lower_bound(descs_.begin(), descs_.end(), id,
                               [](const NodeDesc& d, std::string_view v) {
                                   return d.id < v;
                               });
    if (it != descs_.end() && it->id == id) return &*it;
    return nullptr;
}

std::vector<std::string> NodeRegistry::categories() const {
    std::vector<std::string> out;
    for (const NodeDesc& d : descs_)
        if (!d.category.empty()
            && std::find(out.begin(), out.end(), d.category) == out.end())
            out.push_back(d.category);
    std::sort(out.begin(), out.end());
    return out;
}

}
