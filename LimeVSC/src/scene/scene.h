#pragma once

#include "limecore.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lime {

struct EntityId {
    std::uint32_t v = kInvalid;
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr bool valid() const noexcept { return v != kInvalid; }
    friend constexpr bool operator==(EntityId, EntityId) = default;
    friend constexpr bool operator<(EntityId a, EntityId b) { return a.v < b.v; }
};

struct PropDesc {
    std::string name;
    TypeId       type{};
    std::string  typeName;
    bool         optional = false;
    std::string  defaultValue;
    std::string  doc;
};

struct ComponentDesc {
    std::string id;
    std::string display;
    std::string category;
    std::string doc;
    bool        unique = true;
    std::uint32_t color = 0;
    std::vector<PropDesc> props;
    int         priority = 0;

    const PropDesc* findProp(std::string_view name) const;
    std::string_view label() const {
        return display.empty() ? std::string_view(id) : std::string_view(display);
    }
};

class IComponentProvider {
public:
    virtual ~IComponentProvider() = default;
    virtual std::string_view name() const = 0;
    virtual int priority() const = 0;
    virtual void collect(TypeRegistry& types, std::vector<ComponentDesc>& out,
                         Diagnostics& diag) = 0;
};

class ComponentRegistry {
public:
    void addProvider(std::unique_ptr<IComponentProvider> p);
    void rebuild(TypeRegistry& types, Diagnostics& diag);

    const ComponentDesc* find(std::string_view id) const;
    std::span<const ComponentDesc> all() const noexcept { return descs_; }
    std::span<const ComponentDesc* const> browseOrder() const noexcept {
        return browse_;
    }
    std::vector<std::string> categories() const;

private:
    std::vector<std::unique_ptr<IComponentProvider>> providers_;
    std::vector<ComponentDesc> descs_;
    std::vector<const ComponentDesc*> browse_;
};

struct Component {
    std::string type;
    std::vector<std::pair<std::string, std::string>> values;

    const std::string* value(std::string_view prop) const;
    void setValue(std::string_view prop, std::string v);
    void clearValue(std::string_view prop);
};

struct Entity {
    EntityId    id{};
    std::string name;
    EntityId    parent{};
    std::vector<Component> components;

    Component*       component(std::string_view type);
    const Component* component(std::string_view type) const;
};

class Scene {
public:
    std::string name;

    EntityId addEntity(std::string name, EntityId parent = {});
    EntityId addEntityWithId(EntityId id, std::string name, EntityId parent);
    std::vector<EntityId> removeEntity(EntityId id);

    Entity*       entity(EntityId id);
    const Entity* entity(EntityId id) const;
    std::span<const Entity> entities() const noexcept { return entities_; }
    std::size_t   size() const noexcept { return entities_.size(); }

    std::vector<EntityId> childrenOf(EntityId parent) const;
    bool reparent(EntityId child, EntityId newParent);
    bool isDescendantOf(EntityId maybeChild, EntityId maybeAncestor) const;

    void clear();

    std::uint32_t nextId = 0;

private:
    Entity* find(EntityId id);
    void    reindex() const;

    std::vector<Entity> entities_;
    mutable std::vector<std::uint32_t> index_;
    mutable bool indexDirty_ = true;
};

std::string normalizeEntityName(std::string name);

bool readScene(const std::string& path, Scene& out, Diagnostics& diag);
bool parseScene(std::string_view text, Scene& out, Diagnostics& diag);
std::string writeScene(const Scene& s);
bool writeSceneFile(const std::string& path, const Scene& s, Diagnostics& diag);

}
