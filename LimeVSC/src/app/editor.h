#pragma once

#include "app/loading.h"
#include "app/settings.h"
#include "asset/asset_db.h"
#include "project/project.h"
#include "scene/scene.h"
#include "lime/lua_check.h"
#include "limecore.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace lime {

template <class Doc>
struct EditActionT {
    std::string label;
    std::function<void(Doc&)> redo;
    std::function<void(Doc&)> undo;
    std::uint64_t coalesceKey = 0;
};

template <class Doc>
class UndoStackT {
public:
    using Action = EditActionT<Doc>;

    void perform(Doc& d, Action a) {
        a.redo(d);
        actions_.resize(cursor_);
        actions_.push_back(std::move(a));
        ++cursor_;
    }
    bool undo(Doc& d) {
        if (!canUndo()) return false;
        actions_[--cursor_].undo(d);
        return true;
    }
    bool redo(Doc& d) {
        if (!canRedo()) return false;
        actions_[cursor_++].redo(d);
        return true;
    }
    bool canUndo() const noexcept { return cursor_ > 0; }
    bool canRedo() const noexcept { return cursor_ < actions_.size(); }
    std::string_view undoLabel() const {
        return canUndo() ? std::string_view(actions_[cursor_ - 1].label)
                         : std::string_view{};
    }
    std::string_view redoLabel() const {
        return canRedo() ? std::string_view(actions_[cursor_].label)
                         : std::string_view{};
    }
    Action* top() { return canUndo() ? &actions_[cursor_ - 1] : nullptr; }
    void clear() {
        actions_.clear();
        cursor_ = 0;
    }
    void dropOldest() {
        if (actions_.empty()) return;
        actions_.erase(actions_.begin());
        if (cursor_ > 0) --cursor_;
    }
    std::size_t size() const noexcept { return actions_.size(); }

private:
    std::vector<Action> actions_;
    std::size_t cursor_ = 0;
};

using EditAction = EditActionT<Graph>;
using UndoStack  = UndoStackT<Graph>;
using SceneAction     = EditActionT<Scene>;
using SceneUndoStack  = UndoStackT<Scene>;

using TextAction    = EditActionT<std::string>;
using TextUndoStack = UndoStackT<std::string>;

struct GraphDoc {
    enum class Kind { Graph, Text };
    Kind         kind = Kind::Graph;

    std::string   text;
    TextUndoStack textUndo;
    std::string   textBefore;
    bool          textBurst = false;

    Graph        graph;
    UndoStack    undo;
    std::string  filePath;
    bool         dirty = false;

    std::vector<NodeId> selection;
    NodeId       inspected{};

    std::unordered_map<std::uint32_t, std::pair<float, float>> measured;

    std::string  previewLua;
    SourceMap    previewMap;
    int          previewGotos = 0;
    std::vector<std::pair<NodeId, std::string>> previewErrors;
    bool         previewStale = true;

    void*        canvas = nullptr;
    std::set<std::uint32_t> placed;

    std::uint32_t id = 0;

    bool isText() const { return kind == Kind::Text; }

    std::string displayName() const;
    std::string windowTitle() const;
};

class EditorContext {
public:
    EditorContext();

    std::vector<std::unique_ptr<GraphDoc>> docs;
    std::size_t activeDoc = 0;

    GraphDoc&       doc();
    const GraphDoc& doc() const;

    Graph&       graph()       { return doc().graph; }
    const Graph& graph() const { return doc().graph; }
    UndoStack&   undo()        { return doc().undo; }
    std::string& filePath()    { return doc().filePath; }
    const std::string& filePath() const { return doc().filePath; }
    bool&        dirty()       { return doc().dirty; }
    bool         dirty() const { return doc().dirty; }
    std::vector<NodeId>& selection() { return doc().selection; }
    const std::vector<NodeId>& selection() const { return doc().selection; }
    NodeId&      inspected()   { return doc().inspected; }
    auto&        measured()    { return doc().measured; }
    std::string& previewLua()  { return doc().previewLua; }
    SourceMap&   previewMap()  { return doc().previewMap; }
    int&         previewGotos(){ return doc().previewGotos; }
    auto&        previewErrors() { return doc().previewErrors; }
    bool&        previewStale() { return doc().previewStale; }

    std::size_t  openDoc(const std::string& path);
    void         openText(const std::string& path);
    void         adoptProject(const std::string& path);
    void         newTextFile(const std::string& path);
    bool         saveText();
    void         noteTextEdit(bool burst);
    void         endTextBurst();
    bool         isGeneratedLua(const std::string& path) const;
    std::vector<LuaDiagnostic> luaErrors;
    void         recheckLua();
    void         closeDoc(std::size_t index);
    std::size_t  addDoc();
    std::size_t  findDoc(const std::string& path) const;

    AppSettings    settings;

    LoadQueue      loading;

    TypeRegistry   types;
    NodeRegistry   nodes;
    EmitterRegistry emitters = EmitterRegistry::withBuiltins();
    CommandRegistry commands;

    ProjectContext project;
    std::vector<LoadedMap> maps;
    Diagnostics    diag;

    enum class Inspecting { Node, Entity };
    Inspecting inspecting = Inspecting::Node;

    bool showsEntity() const {
        return project.isEngine() && !scenePath.empty()
               && inspecting == Inspecting::Entity
               && scene.entity(selectedEntity) != nullptr;
    }

    enum class LastEdit { Graph, Scene };
    struct EditRecord {
        LastEdit      kind = LastEdit::Graph;
        std::uint32_t docId = 0;
    };
    std::vector<EditRecord> history;
    std::size_t             historyCursor = 0;
    LastEdit                lastEdit = LastEdit::Graph;

    void noteEdit(LastEdit kind);
    void trimHistory();
    bool canUndoAny() const { return historyCursor > 0; }
    bool canRedoAny() const { return historyCursor < history.size(); }

    Scene             scene;
    ComponentRegistry components;
    AssetTypeRegistry assetTypes;
    AssetDatabase     assets;
    SceneUndoStack    sceneUndo;
    std::string       scenePath;
    bool              sceneDirty = false;
    EntityId          selectedEntity{};

    enum class NoteKind { Action, Warning, Error };
    struct Note {
        std::string   text;
        NoteKind      kind = NoteKind::Action;
        std::uint64_t serial = 0;
    };
    static constexpr std::size_t kMaxNotes = 64;
    std::vector<Note> notes;
    std::uint64_t     noteSerial = 0;
    void note(NoteKind kind, std::string text);
    void report(const Diagnostics& d);

    std::vector<std::string> console;

    void log(std::string line);
    void apply(EditAction a);
    bool isSelected(NodeId n) const;

    NodeId addNode(const NodeDesc& d, float x, float y);
    void   deleteNodes(std::span<const NodeId> ids);
    void   connect(PinId from, PinId to, PinKind kind);
    void   disconnectInput(PinId to);
    void   disconnectOutput(PinId from);
    void   setValue(NodeId n, const std::string& pin, const std::string& v);
    struct NodeMove {
        NodeId id;
        float  fromX = 0, fromY = 0;
        float  toX = 0, toY = 0;
    };
    void   placeNode(NodeId n, float x, float y);
    void   moveNodes(std::span<const NodeMove> moves);
    void   moveNode(NodeId n, float x, float y);
    struct NodeResize {
        NodeId id;
        float  fromW = 0, fromH = 0;
        float  toW = 0, toH = 0;
    };
    void   sizeNode(NodeId n, float w, float h);
    void   resizeNodes(std::span<const NodeResize> sizes);
    void   resizeNode(NodeId n, float w, float h);
    void   setComment(NodeId n, const std::string& text);

    void     applyScene(SceneAction a);
    EntityId addEntity(std::string name, EntityId parent = {});
    void     deleteEntity(EntityId id);
    void     renameEntity(EntityId id, std::string name);
    void     reparentEntity(EntityId child, EntityId newParent);
    void     addComponent(EntityId id, const ComponentDesc& d);
    void     removeComponent(EntityId id, const std::string& type);
    void     setProp(EntityId id, const std::string& comp, const std::string& prop,
                     const std::string& value);
    void     putProp(EntityId id, const std::string& comp, const std::string& prop,
                     const std::string& value);
    void     recordProp(EntityId id, const std::string& comp,
                        const std::string& prop, const std::string& fromValue,
                        bool fromSet);
    void     endCoalescing();

    bool showNewProject = false;
    bool createProjectAt(const std::string& dest, ProjectMode mode,
                         bool mainIsScript = false);

    bool     undoLast();
    bool     redoLast();

    void   seedStartScene();

    std::vector<Node> clipNodes;
    std::vector<Link> clipLinks;
    void   copySelection();
    void   pasteClipboard();

    std::vector<Entity> clipEntities;
    void   copyEntity(EntityId id);
    void   pasteEntity(EntityId into);

    Component clipComponent;
    bool      hasClipComponent = false;
    void   copyComponent(EntityId id, const std::string& type);
    void   pasteComponent(EntityId id);

    bool   saveEntityAsPrefab(EntityId id, const std::string& path);
    void   newPrefab(const std::string& path, const std::string& name);
    bool   editingPrefab() const;
    void   forgetDeleted(const std::string& path);
    void   openStartScene();
    static bool isLuaPath(const std::string& path);
    static bool isTextPath(const std::string& path);
    void   closeProject();
    bool   instantiatePrefab(const std::string& path, EntityId parent);
    void   rescanAssets();
    void   queueAssetScan();
    void   queueOpenProject(const std::string& root);
    bool   saveScene();
    void   openScene(const std::string& path);
    void   newScene(const std::string& path, const std::string& name);
    std::string propValue(const Component& c, const ComponentDesc* d,
                          const std::string& prop) const;

    bool   saveAndCompile();
    void   openFile(const std::string& path);
    void   newGraph(const std::string& path, bool withInit);
    bool   renameGraph(const std::string& path, const std::string& newStem);

    std::function<void(EditorContext&)> rebuildCatalog;
    std::function<void(GraphDoc&)> releaseDocCanvas;
    std::function<void(EditorContext&)> promptSavePath;
    void   rebuildGraphFunctions();
};

void registerCoreCommands(CommandRegistry& reg);
void registerProjectCommands(CommandRegistry& reg);

}
