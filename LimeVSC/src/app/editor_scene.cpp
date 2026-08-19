#include "app/editor.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace lime {

void EditorContext::applyScene(SceneAction a) {
    if (a.coalesceKey != 0) {
        if (SceneAction* prev = sceneUndo.top();
            prev && prev->coalesceKey == a.coalesceKey) {
            a.redo(scene);
            prev->redo = std::move(a.redo);
            sceneDirty = true;
            return;
        }
    }
    sceneUndo.perform(scene, std::move(a));
    sceneDirty = true;
    noteEdit(LastEdit::Scene);
}

void EditorContext::endCoalescing() {
    if (SceneAction* prev = sceneUndo.top()) prev->coalesceKey = 0;
    if (EditAction* prevGraph = undo().top()) prevGraph->coalesceKey = 0;
}

EntityId EditorContext::addEntity(std::string name, EntityId parent) {
    const EntityId id{scene.nextId};
    if (name.empty()) name = "Entity";
    applyScene({"Add Entity",
                [id, name, parent](Scene& s) {
                    s.addEntityWithId(id, name, parent);
                },
                [id](Scene& s) { s.removeEntity(id); }});
    selectedEntity = id;
    return id;
}

void EditorContext::deleteEntity(EntityId id) {
    const Entity* e = scene.entity(id);
    if (!e) return;

    std::vector<Entity> removed;
    std::vector<EntityId> order;
    {
        Scene& s = scene;
        std::vector<EntityId> doomed{id};
        for (std::size_t i = 0; i < doomed.size(); ++i)
            for (EntityId c : s.childrenOf(doomed[i])) doomed.push_back(c);
        for (EntityId d : doomed)
            if (const Entity* p = s.entity(d)) removed.push_back(*p);
        order = doomed;
    }

    if (selectedEntity == id ||
        std::find(order.begin(), order.end(), selectedEntity) != order.end())
        selectedEntity = e->parent;

    applyScene({"Delete Entity",
                [id](Scene& s) { s.removeEntity(id); },
                [removed](Scene& s) {
                    for (const Entity& e : removed) {
                        s.addEntityWithId(e.id, e.name, e.parent);
                        if (Entity* n = s.entity(e.id)) n->components = e.components;
                    }
                }});
}

void EditorContext::renameEntity(EntityId id, std::string name) {
    name = normalizeEntityName(std::move(name));
    const Entity* e = scene.entity(id);
    if (!e || e->name == name) return;
    const std::string was = e->name;
    applyScene({"Rename Entity",
                [id, name](Scene& s) {
                    if (Entity* t = s.entity(id)) t->name = name;
                },
                [id, was](Scene& s) {
                    if (Entity* t = s.entity(id)) t->name = was;
                }});
}

namespace {

void collectSubtree(const Scene& s, EntityId id, std::vector<Entity>& out) {
    if (const Entity* e = s.entity(id)) out.push_back(*e);
    for (EntityId c : s.childrenOf(id)) collectSubtree(s, c, out);
}

}

void EditorContext::copyEntity(EntityId id) {
    clipEntities.clear();
    if (!scene.entity(id)) return;
    collectSubtree(scene, id, clipEntities);
}

void EditorContext::pasteEntity(EntityId into) {
    if (clipEntities.empty()) return;

    const EntityId rootOf = clipEntities.front().id;
    std::vector<std::pair<std::uint32_t, EntityId>> remap;
    const auto mapped = [&](EntityId old, EntityId& out) {
        for (const auto& [from, to] : remap)
            if (from == old.v) { out = to; return true; }
        return false;
    };

    EntityId first{};
    for (const Entity& src : clipEntities) {
        EntityId parent = into;
        if (src.id.v != rootOf.v) {
            EntityId mappedParent{};
            if (mapped(src.parent, mappedParent)) parent = mappedParent;
        }
        const EntityId made = addEntity(src.name, parent);
        remap.push_back({src.id.v, made});
        if (!first.valid()) first = made;
        if (Entity* dst = scene.entity(made))
            dst->components = src.components;
    }
    sceneDirty = true;
    if (first.valid()) selectedEntity = first;
}

void EditorContext::copyComponent(EntityId id, const std::string& type) {
    const Entity* e = scene.entity(id);
    if (!e) return;
    const Component* c = e->component(type);
    if (!c) return;
    clipComponent = *c;
    hasClipComponent = true;
}

void EditorContext::pasteComponent(EntityId id) {
    if (!hasClipComponent) return;
    const Entity* e = scene.entity(id);
    if (!e) return;

    const Component add = clipComponent;
    const Component* existing = e->component(add.type);
    const bool had = existing != nullptr;
    const Component was = had ? *existing : Component{};

    applyScene({had ? "Paste Component Values" : "Paste Component",
                [id, add](Scene& s) {
                    Entity* t = s.entity(id);
                    if (!t) return;
                    if (Component* c = t->component(add.type)) *c = add;
                    else t->components.push_back(add);
                },
                [id, add, had, was](Scene& s) {
                    Entity* t = s.entity(id);
                    if (!t) return;
                    if (had) {
                        if (Component* c = t->component(add.type)) *c = was;
                        return;
                    }
                    for (std::size_t i = 0; i < t->components.size(); ++i)
                        if (t->components[i].type == add.type) {
                            t->components.erase(t->components.begin()
                                                + static_cast<std::ptrdiff_t>(i));
                            return;
                        }
                }});
}

void EditorContext::reparentEntity(EntityId child, EntityId newParent) {
    const Entity* e = scene.entity(child);
    if (!e || e->parent == newParent) return;
    if (newParent.valid() && scene.isDescendantOf(newParent, child)) {
        log("cannot parent an entity to its own child");
        return;
    }
    const EntityId was = e->parent;
    applyScene({"Reparent Entity",
                [child, newParent](Scene& s) { s.reparent(child, newParent); },
                [child, was](Scene& s) { s.reparent(child, was); }});
}

void EditorContext::addComponent(EntityId id, const ComponentDesc& d) {
    Entity* e = scene.entity(id);
    if (!e) return;
    if (d.unique && e->component(d.id)) {
        log(std::string(d.label()) + " is already on this entity");
        return;
    }
    const std::string type = d.id;
    const std::size_t at = e->components.size();
    applyScene({"Add Component",
                [id, type](Scene& s) {
                    if (Entity* t = s.entity(id)) {
                        Component c;
                        c.type = type;
                        t->components.push_back(std::move(c));
                    }
                },
                [id, at](Scene& s) {
                    if (Entity* t = s.entity(id); t && at < t->components.size())
                        t->components.erase(t->components.begin()
                                            + static_cast<std::ptrdiff_t>(at));
                }});
}

void EditorContext::removeComponent(EntityId id, const std::string& type) {
    Entity* e = scene.entity(id);
    if (!e) return;
    const auto it = std::find_if(e->components.begin(), e->components.end(),
                                 [&](const Component& c) { return c.type == type; });
    if (it == e->components.end()) return;

    const std::size_t at =
        static_cast<std::size_t>(it - e->components.begin());
    const Component was = *it;
    applyScene({"Remove Component",
                [id, at](Scene& s) {
                    if (Entity* t = s.entity(id); t && at < t->components.size())
                        t->components.erase(t->components.begin()
                                            + static_cast<std::ptrdiff_t>(at));
                },
                [id, at, was](Scene& s) {
                    if (Entity* t = s.entity(id))
                        t->components.insert(
                            t->components.begin()
                                + static_cast<std::ptrdiff_t>(
                                      std::min(at, t->components.size())),
                            was);
                }});
}

namespace {

auto propWriter(EntityId id, std::string comp, std::string prop,
                std::string value, bool set) {
    return [id, comp = std::move(comp), prop = std::move(prop),
            value = std::move(value), set](Scene& s) {
        if (Entity* t = s.entity(id))
            if (Component* cc = t->component(comp)) {
                if (set) cc->setValue(prop, value);
                else     cc->clearValue(prop);
            }
    };
}

}

void EditorContext::putProp(EntityId id, const std::string& comp,
                            const std::string& prop, const std::string& value) {
    Entity* e = scene.entity(id);
    if (!e) return;
    Component* c = e->component(comp);
    if (!c) return;
    if (const std::string* cur = c->value(prop); cur && *cur == value) return;
    if (value.empty()) c->clearValue(prop);
    else               c->setValue(prop, value);
    sceneDirty = true;
}

void EditorContext::recordProp(EntityId id, const std::string& comp,
                               const std::string& prop,
                               const std::string& fromValue, bool fromSet) {
    const Entity* e = scene.entity(id);
    if (!e) return;
    const Component* c = e->component(comp);
    if (!c) return;

    const std::string* cur = c->value(prop);
    const bool         toSet = cur != nullptr;
    const std::string  to = toSet ? *cur : std::string{};
    if (toSet == fromSet && to == fromValue) return;

    applyScene({"Set Property",
                propWriter(id, comp, prop, to, toSet),
                propWriter(id, comp, prop, fromValue, fromSet),
                0});
}

void EditorContext::setProp(EntityId id, const std::string& comp,
                            const std::string& prop, const std::string& value) {
    Entity* e = scene.entity(id);
    if (!e) return;
    Component* c = e->component(comp);
    if (!c) return;

    const std::string* cur = c->value(prop);
    if (cur && *cur == value) return;
    if (!cur && value.empty()) return;

    const bool hadValue = cur != nullptr;
    const std::string was = hadValue ? *cur : std::string{};

    applyScene({"Set Property",
                propWriter(id, comp, prop, value, !value.empty()),
                propWriter(id, comp, prop, was, hadValue),
                0});
}

std::string EditorContext::propValue(const Component& c, const ComponentDesc* d,
                                     const std::string& prop) const {
    if (const std::string* v = c.value(prop)) return *v;
    if (d)
        if (const PropDesc* p = d->findProp(prop)) return p->defaultValue;
    return {};
}

void EditorContext::rescanAssets() {
    if (!project.valid()) return;
    Diagnostics d;
    assets.scan(project.root, assetTypes, d);
    for (const Diagnostic& x : d.all()) log(x.message);
}

void EditorContext::queueAssetScan() {
    if (!project.valid()) return;
    loading.push("Importing assets", [this](std::string& detail) -> float {
        static bool started = false;
        Diagnostics d;
        if (!started) {
            assets.scanBegin(project.root, assetTypes, d);
            started = true;
        }
        const float f = assets.scanStep(16, detail, d);
        for (const Diagnostic& x : d.all()) log(x.message);
        if (f >= 1.0f) {
            assets.scanEnd();
            started = false;
        }
        return f;
    });
}

bool EditorContext::saveScene() {
    if (scenePath.empty()) {
        log("scene has no path yet");
        return false;
    }
    Diagnostics d;
    if (!writeSceneFile(scenePath, scene, d)) {
        for (const Diagnostic& x : d.all()) log(x.message);
        return false;
    }
    sceneDirty = false;
    log("saved " + fs::path(scenePath).filename().string());
    project.scan();
    return true;
}

void EditorContext::openScene(const std::string& path) {
    Diagnostics d;
    Scene loaded;
    if (!readScene(path, loaded, d)) {
        for (const Diagnostic& x : d.all()) log(x.message);
        return;
    }
    for (const Diagnostic& x : d.all()) log(x.message);

    scene = std::move(loaded);
    scenePath = path;
    sceneDirty = false;
    sceneUndo.clear();
    selectedEntity = {};
    log("opened " + fs::path(path).filename().string());
}

void EditorContext::newScene(const std::string& path, const std::string& name) {
    scene.clear();
    scene.name = name.empty() ? fs::path(path).stem().string() : name;
    scenePath = path;
    sceneUndo.clear();
    selectedEntity = {};

    const EntityId cam = scene.addEntity("Main Camera", {});
    if (Entity* e = scene.entity(cam)) {
        Component t; t.type = "Transform";
        t.setValue("position", "Vec3.new(0, 2, -8)");
        e->components.push_back(std::move(t));
        Component c; c.type = "Camera";
        e->components.push_back(std::move(c));
    }
    const EntityId light = scene.addEntity("Sun", {});
    if (Entity* e = scene.entity(light)) {
        Component t; t.type = "Transform";
        t.setValue("position", "Vec3.new(0, 10, 0)");
        e->components.push_back(std::move(t));
        Component l; l.type = "Light";
        e->components.push_back(std::move(l));
    }

    sceneDirty = true;
    saveScene();
}

}
