#include "app/editor.h"

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <sstream>

namespace lime {
namespace {

std::string moduleFromPath(const std::string& path) {
    return graphModuleName(path);
}

}

void CommandRegistry::add(Command c) { commands_.push_back(std::move(c)); }

const Command* CommandRegistry::find(std::string_view id) const {
    for (const Command& c : commands_) if (c.id == id) return &c;
    return nullptr;
}

bool CommandRegistry::invoke(std::string_view id, EditorContext& ed) const {
    const Command* c = find(id);
    if (!c || !c->run) return false;
    if (c->enabled && !c->enabled(ed)) return false;
    c->run(ed);
    return true;
}

std::string GraphDoc::displayName() const {
    if (filePath.empty()) return "untitled";
    const std::size_t slash = filePath.find_last_of("/\\");
    return slash == std::string::npos ? filePath : filePath.substr(slash + 1);
}

std::string GraphDoc::windowTitle() const {
    return displayName() + "###limedoc" + std::to_string(id);
}

EditorContext::EditorContext() { addDoc(); }

GraphDoc& EditorContext::doc() {
    if (docs.empty()) addDoc();
    if (activeDoc >= docs.size()) activeDoc = docs.size() - 1;
    return *docs[activeDoc];
}
const GraphDoc& EditorContext::doc() const {
    return const_cast<EditorContext*>(this)->doc();
}

std::size_t EditorContext::addDoc() {
    static std::uint32_t nextId = 1;
    auto d = std::make_unique<GraphDoc>();
    d->id = nextId++;
    docs.push_back(std::move(d));
    activeDoc = docs.size() - 1;
    return activeDoc;
}

std::size_t EditorContext::findDoc(const std::string& path) const {
    if (path.empty()) return docs.size();
    for (std::size_t i = 0; i < docs.size(); ++i)
        if (docs[i]->filePath == path) return i;
    return docs.size();
}

namespace {
bool endsWith(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
}

bool EditorContext::isGeneratedLua(const std::string& path) const {
    return endsWith(path, ".lua") && isGeneratedLuaPath(path);
}

void EditorContext::openText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        log("cannot open " + path);
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    GraphDoc& d = doc();
    d.kind = GraphDoc::Kind::Text;
    d.text = ss.str();
    if (d.text.compare(0, 3, "\xEF\xBB\xBF") == 0) d.text.erase(0, 3);
    d.filePath = path;
    d.dirty = false;
    d.textUndo.clear();
    d.textBurst = false;
    d.textBefore = d.text;
    adoptProject(path);
    recheckLua();
    log("opened " + path);
}

void EditorContext::newTextFile(const std::string& path) {
    GraphDoc& d = doc();
    d.kind = GraphDoc::Kind::Text;
    d.filePath = path;
    d.textUndo.clear();
    d.textBurst = false;
    d.text = "local M = {}\n\nreturn M\n";
    d.textBefore = d.text;
    d.dirty = true;
    saveText();
    adoptProject(path);
}

bool EditorContext::saveText() {
    GraphDoc& d = doc();
    if (!d.isText() || d.filePath.empty()) return false;

    std::ofstream f(d.filePath, std::ios::binary);
    if (!f) {
        log("cannot write " + d.filePath);
        return false;
    }
    f.write(d.text.data(), static_cast<std::streamsize>(d.text.size()));
    f.close();
    d.dirty = false;
    log("saved " + d.filePath);
    if (project.valid()) project.scan();
    return true;
}

void EditorContext::noteTextEdit(bool burst) {
    GraphDoc& d = doc();
    if (!d.isText()) return;
    d.dirty = true;
    if (burst) d.textBurst = true;
    else       endTextBurst();
}

void EditorContext::recheckLua() {
    luaErrors.clear();
    if (doc().isText()) luaErrors = checkLua(doc().text);
}

void EditorContext::endTextBurst() {
    GraphDoc& d = doc();
    if (!d.isText()) return;
    d.textBurst = false;
    if (d.textBefore == d.text) return;

    const std::string before = d.textBefore;
    const std::string after = d.text;
    d.textUndo.perform(d.text, {"Edit text",
                                [after](std::string& t) { t = after; },
                                [before](std::string& t) { t = before; }});
    d.textBefore = d.text;
    noteEdit(LastEdit::Graph);
    recheckLua();
}

std::size_t EditorContext::openDoc(const std::string& path) {
    const std::size_t existing = findDoc(path);
    if (existing < docs.size()) {
        activeDoc = existing;
        return existing;
    }
    if (docs.size() == 1 && docs[0]->filePath.empty() && !docs[0]->dirty
        && docs[0]->graph.nodes().empty()) {
        activeDoc = 0;
    } else {
        addDoc();
    }
    if (endsWith(path, ".lua")) {
        if (settings.openExternally(path)) {
            log("opened " + path + " externally");
            return activeDoc;
        }
        openText(path);
    } else {
        openFile(path);
    }
    return activeDoc;
}

void EditorContext::closeDoc(std::size_t index) {
    if (index >= docs.size()) return;
    if (releaseDocCanvas) releaseDocCanvas(*docs[index]);
    docs.erase(docs.begin() + static_cast<std::ptrdiff_t>(index));
    if (docs.empty()) {
        addDoc();
        return;
    }
    if (activeDoc >= docs.size()) activeDoc = docs.size() - 1;
}

void EditorContext::log(std::string line) {
    console.push_back(std::move(line));
    if (console.size() > 2000) console.erase(console.begin());
}

void EditorContext::apply(EditAction a) {
    undo().perform(graph(), std::move(a));
    dirty() = true;
    previewStale() = true;
    noteEdit(LastEdit::Graph);
}

void EditorContext::noteEdit(LastEdit kind) {
    lastEdit = kind;
    history.resize(historyCursor);
    history.push_back({kind, kind == LastEdit::Graph ? doc().id : 0u});
    ++historyCursor;
    trimHistory();
}

namespace {
GraphDoc* findDocById(std::vector<std::unique_ptr<GraphDoc>>& docs,
                      std::uint32_t id) {
    for (auto& d : docs)
        if (d->id == id) return d.get();
    return nullptr;
}
}

void EditorContext::trimHistory() {
    const std::size_t limit = settings.undoLimit;
    if (limit == 0 || history.size() <= limit) return;

    const std::size_t drop = history.size() - limit;
    for (std::size_t i = 0; i < drop; ++i) {
        const EditRecord& r = history[i];
        if (r.kind == LastEdit::Scene) {
            sceneUndo.dropOldest();
        } else if (GraphDoc* d = findDocById(docs, r.docId)) {
            if (d->isText()) d->textUndo.dropOldest();
            else             d->undo.dropOldest();
        }
    }
    history.erase(history.begin(),
                  history.begin() + static_cast<std::ptrdiff_t>(drop));
    historyCursor -= std::min(historyCursor, drop);
}

bool EditorContext::undoLast() {
    while (historyCursor > 0) {
        const EditRecord r = history[historyCursor - 1];
        --historyCursor;

        if (r.kind == LastEdit::Scene) {
            const std::string what(sceneUndo.undoLabel());
            if (!sceneUndo.undo(scene)) continue;
            note(NoteKind::Action, "Undo: " + what);
            sceneDirty = true;
            lastEdit = LastEdit::Scene;
            return true;
        }
        GraphDoc* d = findDocById(docs, r.docId);
        if (!d) continue;
        if (d->isText()) {
            const std::string what(d->textUndo.undoLabel());
            if (!d->textUndo.undo(d->text)) continue;
            note(NoteKind::Action, "Undo: " + what);
            d->textBefore = d->text;
            d->dirty = true;
            lastEdit = LastEdit::Graph;
            return true;
        }
        const std::string what(d->undo.undoLabel());
        if (!d->undo.undo(d->graph)) continue;
        note(NoteKind::Action, "Undo: " + what);
        d->dirty = true;
        d->previewStale = true;
        d->placed.clear();
        lastEdit = LastEdit::Graph;
        return true;
    }
    return false;
}

bool EditorContext::redoLast() {
    while (historyCursor < history.size()) {
        const EditRecord r = history[historyCursor];
        ++historyCursor;

        if (r.kind == LastEdit::Scene) {
            const std::string what(sceneUndo.redoLabel());
            if (!sceneUndo.redo(scene)) continue;
            note(NoteKind::Action, "Redo: " + what);
            sceneDirty = true;
            lastEdit = LastEdit::Scene;
            return true;
        }
        GraphDoc* d = findDocById(docs, r.docId);
        if (!d) continue;
        if (d->isText()) {
            const std::string what(d->textUndo.redoLabel());
            if (!d->textUndo.redo(d->text)) continue;
            note(NoteKind::Action, "Redo: " + what);
            d->textBefore = d->text;
            d->dirty = true;
            lastEdit = LastEdit::Graph;
            return true;
        }
        const std::string what(d->undo.redoLabel());
        if (!d->undo.redo(d->graph)) continue;
        note(NoteKind::Action, "Redo: " + what);
        d->dirty = true;
        d->previewStale = true;
        d->placed.clear();
        lastEdit = LastEdit::Graph;
        return true;
    }
    return false;
}

bool EditorContext::isSelected(NodeId n) const {
    return std::find(selection().begin(), selection().end(), n) != selection().end();
}

NodeId EditorContext::addNode(const NodeDesc& d, float x, float y) {
    const NodeId id{graph().nextId};
    const std::string type = d.id;

    std::vector<std::pair<std::string, std::string>> defaults;
    for (const PinDesc& p : d.pins)
        if (p.dir == PinDir::In && p.kind == PinKind::Data && !p.defaultValue.empty())
            defaults.emplace_back(p.name, p.defaultValue);

    apply({"Add " + (d.display.empty() ? d.id : d.display),
           [id, type, x, y, defaults](Graph& g) {
               g.addNodeWithId(id, type, x, y);
               if (Node* n = g.node(id)) n->values = defaults;
           },
           [id](Graph& g) { g.removeNode(id); }});
    return id;
}

void EditorContext::deleteNodes(std::span<const NodeId> ids) {
    std::vector<Node> saved;
    std::vector<Link> savedLinks;
    for (NodeId id : ids)
        if (const Node* n = graph().node(id)) saved.push_back(*n);
    for (const Link& l : graph().links())
        if (std::find(ids.begin(), ids.end(), l.from.node) != ids.end()
            || std::find(ids.begin(), ids.end(), l.to.node) != ids.end())
            savedLinks.push_back(l);

    if (saved.empty()) return;
    const std::vector<NodeId> list(ids.begin(), ids.end());

    apply({saved.size() == 1 ? "Delete node" : "Delete nodes",
           [list](Graph& g) { for (NodeId id : list) g.removeNode(id); },
           [saved, savedLinks](Graph& g) {
               for (const Node& n : saved) {
                   g.addNodeWithId(n.id, n.type, n.x, n.y);
                   if (Node* nn = g.node(n.id)) *nn = n;
               }
               for (const Link& l : savedLinks) g.connect(l.from, l.to, l.kind);
           }});
    selection().clear();
}

void EditorContext::connect(PinId from, PinId to, PinKind kind) {
    std::optional<Link> displaced;
    for (const Link& l : graph().links()) {
        const bool clash = (kind == PinKind::Data)
                               ? (l.kind == PinKind::Data && l.to == to)
                               : (l.kind == PinKind::Exec && l.from == from);
        if (clash) { displaced = l; break; }
    }

    apply({"Connect",
           [from, to, kind](Graph& g) { g.connect(from, to, kind); },
           [from, to, kind, displaced](Graph& g) {
               if (kind == PinKind::Data) g.disconnect(to);
               else                       g.disconnectFrom(from);
               if (displaced) g.connect(displaced->from, displaced->to, displaced->kind);
           }});
}

void EditorContext::disconnectOutput(PinId from) {
    std::vector<Link> dropped;
    for (const Link& l : graph().links())
        if (l.from == from) dropped.push_back(l);
    if (dropped.empty()) return;

    apply({dropped.size() == 1 ? "Detach wire" : "Detach wires",
           [dropped](Graph& g) {
               for (const Link& l : dropped)
                   if (l.kind == PinKind::Exec) g.disconnectFrom(l.from);
                   else                          g.disconnect(l.to);
           },
           [dropped](Graph& g) {
               for (const Link& l : dropped) g.connect(l.from, l.to, l.kind);
           }});
}

void EditorContext::disconnectInput(PinId to) {
    std::optional<Link> removed;
    for (const Link& l : graph().links())
        if (l.to == to) { removed = l; break; }
    if (!removed) return;

    const Link r = *removed;
    apply({"Disconnect",
           [r](Graph& g) {
               if (r.kind == PinKind::Data) g.disconnect(r.to);
               else                         g.disconnectFrom(r.from);
           },
           [r](Graph& g) { g.connect(r.from, r.to, r.kind); }});
}

void EditorContext::setValue(NodeId n, const std::string& pin, const std::string& v) {
    const Node* node = graph().node(n);
    if (!node) return;

    std::string old;
    bool had = false;
    for (const auto& [k, val] : node->values)
        if (k == pin) { old = val; had = true; break; }
    if (had && old == v) return;

    auto assign = [n, pin](Graph& g, const std::string& value, bool present) {
        Node* nn = g.node(n);
        if (!nn) return;
        for (auto it = nn->values.begin(); it != nn->values.end(); ++it) {
            if (it->first != pin) continue;
            if (present) it->second = value;
            else         nn->values.erase(it);
            return;
        }
        if (present) nn->values.emplace_back(pin, value);
    };

    apply({"Set " + pin,
           [assign, v](Graph& g) { assign(g, v, true); },
           [assign, old, had](Graph& g) { assign(g, old, had); }});
}

void EditorContext::placeNode(NodeId n, float x, float y) {
    Node* node = graph().node(n);
    if (!node || (node->x == x && node->y == y)) return;
    node->x = x;
    node->y = y;
    dirty() = true;
}

void EditorContext::moveNodes(std::span<const NodeMove> moves) {
    std::vector<NodeMove> list;
    for (const NodeMove& m : moves)
        if (m.fromX != m.toX || m.fromY != m.toY) list.push_back(m);
    if (list.empty()) return;

    noteEdit(LastEdit::Graph);
    undo().perform(
        graph(),
        {list.size() == 1 ? "Move node" : "Move nodes",
         [list](Graph& g) {
             for (const NodeMove& m : list)
                 if (Node* n = g.node(m.id)) { n->x = m.toX; n->y = m.toY; }
         },
         [list](Graph& g) {
             for (const NodeMove& m : list)
                 if (Node* n = g.node(m.id)) { n->x = m.fromX; n->y = m.fromY; }
         },
         0});
    dirty() = true;
}

void EditorContext::moveNode(NodeId n, float x, float y) {
    const Node* node = graph().node(n);
    if (!node) return;
    const NodeMove m{n, node->x, node->y, x, y};
    moveNodes({&m, 1});
}

void EditorContext::sizeNode(NodeId n, float w, float h) {
    Node* node = graph().node(n);
    if (!node || (node->w == w && node->h == h)) return;
    node->w = w;
    node->h = h;
    dirty() = true;
}

void EditorContext::resizeNodes(std::span<const NodeResize> sizes) {
    std::vector<NodeResize> list;
    for (const NodeResize& r : sizes)
        if (r.fromW != r.toW || r.fromH != r.toH) list.push_back(r);
    if (list.empty()) return;

    noteEdit(LastEdit::Graph);
    undo().perform(
        graph(),
        {list.size() == 1 ? "Resize comment" : "Resize comments",
         [list](Graph& g) {
             for (const NodeResize& r : list)
                 if (Node* n = g.node(r.id)) { n->w = r.toW; n->h = r.toH; }
         },
         [list](Graph& g) {
             for (const NodeResize& r : list)
                 if (Node* n = g.node(r.id)) { n->w = r.fromW; n->h = r.fromH; }
         },
         0});
    dirty() = true;
}

void EditorContext::resizeNode(NodeId n, float w, float h) {
    const Node* node = graph().node(n);
    if (!node) return;
    const NodeResize r{n, node->w, node->h, w, h};
    resizeNodes({&r, 1});
}

void EditorContext::note(NoteKind kind, std::string text) {
    if (text.empty()) return;
    notes.push_back({std::move(text), kind, ++noteSerial});
    if (notes.size() > kMaxNotes)
        notes.erase(notes.begin(),
                    notes.begin()
                        + static_cast<std::ptrdiff_t>(notes.size() - kMaxNotes));
}

void EditorContext::report(const Diagnostics& d) {
    for (const Diagnostic& x : d.all()) {
        const char* sev = x.severity == Severity::Error   ? "error: "
                        : x.severity == Severity::Warning ? "warning: "
                                                          : "";
        log(std::string(sev) + x.message);
        if (x.severity == Severity::Error)
            note(NoteKind::Error, x.message);
        else if (x.severity == Severity::Warning)
            note(NoteKind::Warning, x.message);
    }
}

void EditorContext::setComment(NodeId n, const std::string& text) {
    const Node* node = graph().node(n);
    if (!node || node->comment == text) return;
    const std::string old = node->comment;
    apply({"Edit comment",
           [n, text](Graph& g) { if (Node* x = g.node(n)) x->comment = text; },
           [n, old](Graph& g)  { if (Node* x = g.node(n)) x->comment = old; }});
}

bool EditorContext::saveAndCompile() {
    if (doc().isText()) return saveText();

    if (filePath().empty()) return false;
    Diagnostics d;
    if (!writeLimeFile(filePath(), graph(), d)) {
        for (const Diagnostic& x : d.all()) log("error: " + x.message);
        return false;
    }
    dirty() = false;

    Diagnostics cd;
    const CompileResult r = compileGraph(
        graph(), nodes, types, emitters,
        std::filesystem::path(filePath()).filename().string(), cd);
    if (!r.ok) {
        for (const Diagnostic& x : cd.all())
            if (x.severity == Severity::Error) log("error: " + x.message);
        return false;
    }

    const std::filesystem::path out = generatedLuaPath(filePath());
    std::error_code oec;
    std::filesystem::create_directories(out.parent_path(), oec);
    std::ofstream f(out, std::ios::binary);
    if (!f) { log("cannot write " + out.string()); return false; }
    f.write(r.lua.data(), static_cast<std::streamsize>(r.lua.size()));
    f.close();

    std::filesystem::path mapPath = out;
    mapPath += ".map";
    std::ofstream m(mapPath, std::ios::binary);
    for (const auto& [line, node] : r.map.lines)
        m << line << ' ' << encodeId(node.v) << '\n';

    log("saved " + filePath() + " -> " + out.filename().string());

    if (project.valid()) {
        project.scan();
        rebuildGraphFunctions();
    }
    return true;
}

void EditorContext::newGraph(const std::string& path, bool withInit) {
    graph() = Graph{};
    filePath() = path;

    graph().moduleName = moduleFromPath(path);

    float y = 0.0f;
    auto seed = [&](const char* type) {
        if (const NodeDesc* d = nodes.find(type)) {
            graph().addNode(d->id, 0.0f, y);
            y += 150.0f;
        }
    };
    if (withInit) seed("Lime.onInit");
    seed("Lime.onStart");
    seed("Lime.onUpdate");
    seed("Lime.onClose");

    dirty() = true;
    previewStale() = true;
    undo().clear();
    selection().clear();
    inspected() = NodeId{};
    doc().placed.clear();
    log(path.empty() ? "new graph (unsaved)" : "new graph " + path);
}

void EditorContext::seedStartScene() {
    const NodeDesc* d = nodes.find("scene.start");
    if (!d) {
        log("Start Scene node is missing from the catalog");
        return;
    }
    NodeId start{};
    for (const Node& n : graph().nodes())
        if (n.type == "Lime.onStart") { start = n.id; break; }
    if (!start.valid()) {
        log("no On Start node to attach Start Scene to");
        return;
    }

    const NodeId scene = graph().addNode(d->id, 320.0f, 150.0f);
    if (!sceneModuleName(project).empty())
        if (Node* n = graph().node(scene))
            n->values.push_back({"module", "\"content.lime_boot\""});
    graph().connect(PinId::make(start, "out"), PinId::make(scene, "in"),
                  PinKind::Exec);
    dirty() = true;
    previewStale() = true;
}

bool EditorContext::renameGraph(const std::string& path,
                                const std::string& newStem) {
    namespace fs = std::filesystem;
    if (newStem.empty()) return false;

    std::error_code ec;
    const fs::path oldPath(path);
    const fs::path newPath = oldPath.parent_path() / (newStem + ".lime");
    if (newPath == oldPath) return true;
    if (fs::exists(newPath, ec)) {
        log("cannot rename: " + newPath.filename().string() + " already exists");
        return false;
    }

    const bool wasOpen = (filePath() == path);
    if (wasOpen && dirty()) saveAndCompile();

    fs::rename(oldPath, newPath, ec);
    if (ec) {
        log("rename failed: " + ec.message());
        return false;
    }

    fs::path oldLua = generatedLuaPath(oldPath.string());
    fs::path oldMap = oldLua;   oldMap += ".map";
    fs::remove(oldLua, ec);
    fs::remove(oldMap, ec);

    {
        const std::string oldModule = moduleFromPath(path);
        const std::string newModule = moduleFromPath(newPath.string());
        const std::string oldPrefix = "fn." + oldModule + ".";
        const std::string newPrefix = "fn." + newModule + ".";

        if (project.valid()) project.scan();
        int rewritten = 0, files = 0;

        for (const std::string& f : project.limeFiles) {
            Diagnostics rd;
            Graph g;
            if (!readLime(f, g, rd)) continue;

            int hits = 0;
            for (const Node& n : g.nodes()) {
                if (n.type.rfind(oldPrefix, 0) != 0) continue;
                Node* mut = g.node(n.id);
                if (!mut) continue;
                mut->type = newPrefix + mut->type.substr(oldPrefix.size());
                ++hits;
            }
            if (hits == 0) continue;

            Diagnostics wd;
            if (writeLimeFile(f, g, wd)) {
                rewritten += hits;
                ++files;
            } else {
                for (const Diagnostic& x : wd.all()) log("error: " + x.message);
            }
        }

        if (rewritten > 0)
            log("re-pointed " + std::to_string(rewritten) + " reference(s) in "
                + std::to_string(files) + " graph(s) to " + newModule);
    }

    if (wasOpen) {
        openFile(newPath.string());
        saveAndCompile();
    }
    if (project.valid()) {
        project.scan();
        rebuildGraphFunctions();
    }
    log("renamed to " + newPath.filename().string());
    return true;
}

void EditorContext::rebuildGraphFunctions() {
    if (!project.valid() || !rebuildCatalog) return;
    rebuildCatalog(*this);
    previewStale() = true;
}

void EditorContext::openFile(const std::string& path) {
    Diagnostics d;
    Graph g;
    if (!readLime(path, g, d)) {
        for (const Diagnostic& x : d.all()) log("error: " + x.message);
        return;
    }
    graph() = std::move(g);
    filePath() = path;
    dirty() = false;
    previewStale() = true;
    undo().clear();
    selection().clear();
    inspected() = NodeId{};
    doc().placed.clear();

    adoptProject(path);
    log("opened " + path);
}

void EditorContext::queueOpenProject(const std::string& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root.empty() || !fs::exists(fs::path(root) / "content", ec)) {
        log("not a LimeX project: " + root + " (no content/ folder)");
        settings.forgetProject(root);
        Diagnostics sd;
        settings.save(sd);
        return;
    }

    loading.clear();
    loading.push("Scanning project", [this, root](std::string& detail) {
        project.root = root;
        project.limeBuilder = ProjectContext::findLimeBuilder(root);
        project.scan();
        detail = std::to_string(project.limeFiles.size()) + " graphs, "
                 + std::to_string(project.sceneFiles.size()) + " scenes";
        return 1.0f;
    });
    loading.push("Building node catalog", [this](std::string& detail) {
        if (rebuildCatalog) rebuildCatalog(*this);
        detail = std::to_string(nodes.all().size()) + " nodes";
        return 1.0f;
    });
    queueAssetScan();
    loading.push("Opening graph", [this, root](std::string& detail) {
        settings.noteProject(root);
        Diagnostics sd;
        settings.save(sd);

        std::string pick;
        for (const std::string& f : project.limeFiles)
            if (std::filesystem::path(f).filename() == "main.lime") pick = f;
        if (pick.empty() && !project.limeFiles.empty())
            pick = project.limeFiles.front();

        if (pick.empty()) {
            log("opened project " + root + " (no graphs yet)");
        } else {
            openDoc(pick);
            detail = std::filesystem::path(pick).filename().string();
        }
        log(std::string("mode: ") + projectModeName(project.mode));
        return 1.0f;
    });
}

bool EditorContext::openProjectAt(const std::string& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root.empty() || !fs::exists(fs::path(root) / "content", ec)) {
        log("not a LimeX project: " + root + " (no content/ folder)");
        settings.forgetProject(root);
        Diagnostics sd;
        settings.save(sd);
        return false;
    }

    project.root = root;
    project.limeBuilder = ProjectContext::findLimeBuilder(root);
    project.scan();
    rebuildGraphFunctions();

    settings.noteProject(root);
    Diagnostics sd;
    settings.save(sd);

    std::string pick;
    for (const std::string& f : project.limeFiles)
        if (fs::path(f).filename() == "main.lime") pick = f;
    if (pick.empty() && !project.limeFiles.empty()) pick = project.limeFiles.front();

    if (pick.empty()) log("opened project " + root + " (no graphs yet)");
    else              openDoc(pick);

    log(std::string("mode: ") + projectModeName(project.mode));
    return true;
}

void EditorContext::adoptProject(const std::string& path) {
    const std::filesystem::path content =
        std::filesystem::path(path).parent_path();
    if (content.filename() != "content") return;

    const std::string newRoot = content.parent_path().string();
    const bool changed = newRoot != project.root;
    project.root = newRoot;
    project.limeBuilder = ProjectContext::findLimeBuilder(project.root);
    project.scan();

    if (changed) rebuildGraphFunctions();
    if (changed) {
        settings.noteProject(project.root);
        Diagnostics sd;
        settings.save(sd);
    }
}

void registerCoreCommands(CommandRegistry& reg) {
    reg.add({"edit.undo", "Undo", "Edit", "Ctrl+Z",
             [](EditorContext& ed) {
                 ed.undoLast();
             },
             [](EditorContext& ed) {
                 return ed.canUndoAny();
             }});

    reg.add({"edit.redo", "Redo", "Edit", "Ctrl+Y",
             [](EditorContext& ed) {
                 ed.redoLast();
             },
             [](EditorContext& ed) {
                 return ed.canRedoAny();
             }});

    reg.add({"graph.delete", "Delete Selected", "Graph", "Delete",
             [](EditorContext& ed) { ed.deleteNodes(ed.selection()); },
             [](EditorContext& ed) { return !ed.selection().empty(); }});

    reg.add({"graph.duplicate", "Duplicate Selected", "Graph", "Ctrl+D",
             [](EditorContext& ed) {
                 std::vector<NodeId> made;
                 for (NodeId id : ed.selection()) {
                     const Node* src = ed.graph().node(id);
                     if (!src) continue;
                     const NodeDesc* d = ed.nodes.find(src->type);
                     if (!d) continue;
                     const NodeId n = ed.addNode(*d, src->x + 24.0f, src->y + 24.0f);
                     if (Node* dst = ed.graph().node(n)) dst->values = src->values;
                     made.push_back(n);
                 }
                 ed.selection() = made;
             },
             [](EditorContext& ed) { return !ed.selection().empty(); }});

    reg.add({"graph.comment", "Comment Selection", "Graph", "C",
             [](EditorContext& ed) {
                 float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                 for (NodeId id : ed.selection())
                     if (const Node* n = ed.graph().node(id)) {
                         float w = n->w > 0 ? n->w : 220.0f;
                         float h = n->h > 0 ? n->h : 110.0f;
                         if (const auto it = ed.measured().find(id.v);
                             it != ed.measured().end()) {
                             w = it->second.first;
                             h = it->second.second;
                         }
                         x0 = (std::min)(x0, n->x);
                         y0 = (std::min)(y0, n->y);
                         x1 = (std::max)(x1, n->x + w);
                         y1 = (std::max)(y1, n->y + h);
                     }
                 const NodeDesc* d = ed.nodes.find("core.comment");
                 if (!d) return;
                 const bool empty = ed.selection().empty();
                 const float pad = 34.0f;
                 const float topPad = pad + 26.0f;
                 const NodeId c = ed.addNode(*d, empty ? 0.0f : x0 - pad,
                                             empty ? 0.0f : y0 - topPad);
                 if (Node* n = ed.graph().node(c)) {
                     n->w = empty ? 340.0f : (x1 - x0) + pad * 2;
                     n->h = empty ? 190.0f : (y1 - y0) + topPad + pad;
                     n->comment = "Comment";
                 }
                 ed.selection() = {c};
                 ed.inspected() = c;
             },
             nullptr});

    reg.add({"graph.reroute", "Add Reroute", "Graph", "R",
             [](EditorContext& ed) {
                 if (const NodeDesc* d = ed.nodes.find("core.reroute"))
                     ed.selection() = {ed.addNode(*d, 0.0f, 0.0f)};
             },
             nullptr});

    reg.add({"file.save", "Save", "File", "Ctrl+S",
             [](EditorContext& ed) {
                 if (ed.filePath().empty()) {
                     if (ed.promptSavePath) ed.promptSavePath(ed);
                     if (ed.filePath().empty()) return;
                 }
                 ed.saveAndCompile();
             },
             nullptr});
}

}
