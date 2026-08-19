#include "scene/scene.h"

#include <algorithm>

namespace lime {

const PropDesc* ComponentDesc::findProp(std::string_view name) const {
    for (const PropDesc& p : props)
        if (p.name == name) return &p;
    return nullptr;
}

void ComponentRegistry::addProvider(std::unique_ptr<IComponentProvider> p) {
    providers_.push_back(std::move(p));
}

void ComponentRegistry::rebuild(TypeRegistry& types, Diagnostics& diag) {
    descs_.clear();
    browse_.clear();

    std::vector<ComponentDesc> collected;
    for (auto& p : providers_) {
        const std::size_t before = collected.size();
        p->collect(types, collected, diag);
        for (std::size_t i = before; i < collected.size(); ++i)
            collected[i].priority = p->priority();
    }

    std::stable_sort(collected.begin(), collected.end(),
                     [](const ComponentDesc& a, const ComponentDesc& b) {
                         if (a.id != b.id) return a.id < b.id;
                         return a.priority < b.priority;
                     });
    for (ComponentDesc& d : collected) {
        if (!descs_.empty() && descs_.back().id == d.id)
            descs_.back() = std::move(d);
        else
            descs_.push_back(std::move(d));
    }

    browse_.reserve(descs_.size());
    for (const ComponentDesc& d : descs_) browse_.push_back(&d);
    std::sort(browse_.begin(), browse_.end(),
              [](const ComponentDesc* a, const ComponentDesc* b) {
                  if (a->category != b->category) return a->category < b->category;
                  return a->label() < b->label();
              });
}

const ComponentDesc* ComponentRegistry::find(std::string_view id) const {
    const auto it = std::lower_bound(descs_.begin(), descs_.end(), id,
                                     [](const ComponentDesc& d, std::string_view s) {
                                         return d.id < s;
                                     });
    if (it == descs_.end() || it->id != id) return nullptr;
    return &*it;
}

std::vector<std::string> ComponentRegistry::categories() const {
    std::vector<std::string> out;
    for (const ComponentDesc* d : browse_)
        if (out.empty() || out.back() != d->category) out.push_back(d->category);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const std::string* Component::value(std::string_view prop) const {
    for (const auto& kv : values)
        if (kv.first == prop) return &kv.second;
    return nullptr;
}

void Component::setValue(std::string_view prop, std::string v) {
    for (auto& kv : values)
        if (kv.first == prop) { kv.second = std::move(v); return; }
    values.emplace_back(std::string(prop), std::move(v));
}

void Component::clearValue(std::string_view prop) {
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&](const auto& kv) { return kv.first == prop; }),
                 values.end());
}

Component* Entity::component(std::string_view type) {
    for (Component& c : components)
        if (c.type == type) return &c;
    return nullptr;
}
const Component* Entity::component(std::string_view type) const {
    return const_cast<Entity*>(this)->component(type);
}

void Scene::reindex() const {
    std::uint32_t hi = 0;
    for (const Entity& e : entities_) hi = std::max(hi, e.id.v + 1);
    index_.assign(hi, EntityId::kInvalid);
    for (std::uint32_t i = 0; i < entities_.size(); ++i)
        index_[entities_[i].id.v] = i;
    indexDirty_ = false;
}

Entity* Scene::entity(EntityId id) {
    if (!id.valid()) return nullptr;
    if (indexDirty_) reindex();
    if (id.v >= index_.size()) return nullptr;
    const std::uint32_t slot = index_[id.v];
    return slot == EntityId::kInvalid ? nullptr : &entities_[slot];
}
const Entity* Scene::entity(EntityId id) const {
    return const_cast<Scene*>(this)->entity(id);
}

EntityId Scene::addEntity(std::string name, EntityId parent) {
    return addEntityWithId(EntityId{nextId}, std::move(name), parent);
}

std::string normalizeEntityName(std::string name) {
    for (char& c : name)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    std::size_t b = 0, e = name.size();
    while (b < e && name[b] == ' ') ++b;
    while (e > b && name[e - 1] == ' ') --e;
    return name.substr(b, e - b);
}

EntityId Scene::addEntityWithId(EntityId id, std::string name, EntityId parent) {
    if (!id.valid()) return {};
    Entity e;
    e.id = id;
    e.name = normalizeEntityName(std::move(name));
    e.parent = parent;
    if (!indexDirty_) {
        if (id.v >= index_.size()) index_.resize(id.v + 1, EntityId::kInvalid);
        index_[id.v] = static_cast<std::uint32_t>(entities_.size());
    }
    entities_.push_back(std::move(e));
    nextId = std::max(nextId, id.v + 1);
    return id;
}

std::vector<EntityId> Scene::removeEntity(EntityId id) {
    std::vector<EntityId> doomed;
    if (!entity(id)) return doomed;

    doomed.push_back(id);
    for (std::size_t i = 0; i < doomed.size(); ++i)
        for (EntityId c : childrenOf(doomed[i])) doomed.push_back(c);
    std::reverse(doomed.begin(), doomed.end());

    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [&](const Entity& e) {
                                       return std::find(doomed.begin(), doomed.end(),
                                                        e.id) != doomed.end();
                                   }),
                    entities_.end());
    indexDirty_ = true;
    return doomed;
}

std::vector<EntityId> Scene::childrenOf(EntityId parent) const {
    std::vector<EntityId> out;
    for (const Entity& e : entities_)
        if (e.parent == parent) out.push_back(e.id);
    return out;
}

bool Scene::isDescendantOf(EntityId maybeChild, EntityId maybeAncestor) const {
    const Entity* e = entity(maybeChild);
    for (std::size_t guard = 0; e && guard <= entities_.size(); ++guard) {
        if (e->parent == maybeAncestor) return true;
        e = entity(e->parent);
    }
    return false;
}

bool Scene::reparent(EntityId child, EntityId newParent) {
    Entity* e = entity(child);
    if (!e) return false;
    if (child == newParent) return false;
    if (newParent.valid() && !entity(newParent)) return false;
    if (newParent.valid() && isDescendantOf(newParent, child)) return false;
    e->parent = newParent;
    return true;
}

void Scene::clear() {
    entities_.clear();
    index_.clear();
    indexDirty_ = true;
    nextId = 0;
    name.clear();
}

}
