#include "limecore.h"

#include <algorithm>

namespace lime {

void Graph::reindexNodes() const {
    nodeIx_.clear();
    nodeIx_.reserve(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) nodeIx_[nodes_[i].id.v] = i;
    nodeIxDirty_ = false;
}

void Graph::reindexLinks() const {
    dataBySink_.clear();
    execBySource_.clear();
    dataBySource_.clear();
    execBySink_.clear();
    dataBySink_.reserve(links_.size());
    for (std::size_t i = 0; i < links_.size(); ++i) {
        const Link& l = links_[i];
        if (l.kind == PinKind::Data) {
            dataBySink_[pinKey(l.to)] = i;
            dataBySource_[pinKey(l.from)].push_back(i);
        } else {
            execBySource_[pinKey(l.from)] = i;
            execBySink_[pinKey(l.to)].push_back(i);
        }
    }
    linkIxDirty_ = false;
}

NodeId Graph::addNode(std::string type, float x, float y) {
    return addNodeWithId(NodeId{nextId}, std::move(type), x, y);
}

NodeId Graph::addNodeWithId(NodeId id, std::string type, float x, float y) {
    Node n;
    n.id = id;
    n.type = std::move(type);
    n.x = x;
    n.y = y;
    nodes_.push_back(std::move(n));
    if (id.v >= nextId) nextId = id.v + 1;
    if (!nodeIxDirty_) nodeIx_[id.v] = nodes_.size() - 1;
    return id;
}

void Graph::removeNode(NodeId id) {
    disconnectAll(id);
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [&](const Node& n) { return n.id == id; }),
                 nodes_.end());
    nodeIxDirty_ = true;
}

Node* Graph::node(NodeId id) {
    if (nodeIxDirty_) reindexNodes();
    const auto it = nodeIx_.find(id.v);
    return it == nodeIx_.end() ? nullptr : &nodes_[it->second];
}

const Node* Graph::node(NodeId id) const {
    if (nodeIxDirty_) reindexNodes();
    const auto it = nodeIx_.find(id.v);
    return it == nodeIx_.end() ? nullptr : &nodes_[it->second];
}

bool Graph::connect(PinId from, PinId to, PinKind kind) {
    if (!from.valid() || !to.valid()) return false;
    if (from.node == to.node) return false;

    if (kind == PinKind::Data) disconnect(to);
    else                       disconnectFrom(from);

    links_.push_back(Link{from, to, kind});
    if (!linkIxDirty_) {
        const std::size_t i = links_.size() - 1;
        if (kind == PinKind::Data) {
            dataBySink_[pinKey(to)] = i;
            dataBySource_[pinKey(from)].push_back(i);
        } else {
            execBySource_[pinKey(from)] = i;
            execBySink_[pinKey(to)].push_back(i);
        }
    }
    return true;
}

void Graph::addLinkUnchecked(PinId from, PinId to, PinKind kind) {
    if (!from.valid() || !to.valid() || from.node == to.node) return;
    links_.push_back(Link{from, to, kind});
    if (linkIxDirty_) return;
    const std::size_t i = links_.size() - 1;
    if (kind == PinKind::Data) {
        dataBySink_[pinKey(to)] = i;
        dataBySource_[pinKey(from)].push_back(i);
    } else {
        execBySource_[pinKey(from)] = i;
    }
}

bool Graph::validateLinks(Diagnostics& diag) const {
    std::unordered_map<std::uint64_t, int> sinkCount, execCount;
    for (const Link& l : links_) {
        if (l.kind == PinKind::Data) ++sinkCount[pinKey(l.to)];
        else                         ++execCount[pinKey(l.from)];
    }
    bool ok = true;
    for (const Link& l : links_) {
        if (l.kind == PinKind::Data && sinkCount[pinKey(l.to)] > 1) {
            diag.error("input '" + std::string(l.to.pin.str())
                       + "' has more than one source", l.to.node);
            ok = false;
            sinkCount[pinKey(l.to)] = 1;
        } else if (l.kind == PinKind::Exec && execCount[pinKey(l.from)] > 1) {
            diag.error("exec output '" + std::string(l.from.pin.str())
                       + "' has more than one target", l.from.node);
            ok = false;
            execCount[pinKey(l.from)] = 1;
        }
    }
    return ok;
}

void Graph::disconnect(PinId to) {
    const auto n = links_.size();
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const Link& l) {
                                    return l.kind == PinKind::Data && l.to == to;
                                }),
                 links_.end());
    if (links_.size() != n) linkIxDirty_ = true;
}

void Graph::disconnectFrom(PinId from) {
    const auto n = links_.size();
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const Link& l) {
                                    return l.kind == PinKind::Exec && l.from == from;
                                }),
                 links_.end());
    if (links_.size() != n) linkIxDirty_ = true;
}

void Graph::disconnectAll(NodeId n) {
    const auto before = links_.size();
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const Link& l) {
                                    return l.from.node == n || l.to.node == n;
                                }),
                 links_.end());
    if (links_.size() != before) linkIxDirty_ = true;
}

std::optional<PinId> Graph::sourceOf(PinId input) const {
    if (linkIxDirty_) reindexLinks();
    const auto it = dataBySink_.find(pinKey(input));
    if (it == dataBySink_.end()) return std::nullopt;
    return links_[it->second].from;
}

std::optional<PinId> Graph::execTargetOf(PinId output) const {
    if (linkIxDirty_) reindexLinks();
    const auto it = execBySource_.find(pinKey(output));
    if (it == execBySource_.end()) return std::nullopt;
    return links_[it->second].to;
}

std::vector<PinId> Graph::execSourcesOf(PinId input) const {
    if (linkIxDirty_) reindexLinks();
    std::vector<PinId> out;
    const auto it = execBySink_.find(pinKey(input));
    if (it == execBySink_.end()) return out;
    out.reserve(it->second.size());
    for (std::size_t i : it->second) out.push_back(links_[i].from);
    return out;
}

std::vector<PinId> Graph::targetsOf(PinId output) const {
    if (linkIxDirty_) reindexLinks();
    std::vector<PinId> out;
    const auto it = dataBySource_.find(pinKey(output));
    if (it == dataBySource_.end()) return out;
    out.reserve(it->second.size());
    for (std::size_t i : it->second) out.push_back(links_[i].to);
    return out;
}

}
