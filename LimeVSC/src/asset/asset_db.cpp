#include "asset/asset_db.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <toml.hpp>

namespace fs = std::filesystem;

namespace lime {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toHex(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool fromHex(std::string_view s, std::uint64_t& out) {
    if (s.size() != 16) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

std::uint64_t hashFile(const fs::path& p, std::uint64_t& sizeOut) {
    std::ifstream f(p, std::ios::binary);
    std::uint64_t h = 1469598103934665603ull;
    sizeOut = 0;
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        const std::streamsize n = f.gcount();
        sizeOut += static_cast<std::uint64_t>(n);
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::string generic(const fs::path& p) {
    std::string s = p.generic_string();
    return s;
}

}

std::string AssetGuid::str() const { return toHex(hi) + toHex(lo); }

AssetGuid AssetGuid::parse(std::string_view hex) {
    AssetGuid g;
    if (hex.size() != 32) return {};
    if (!fromHex(hex.substr(0, 16), g.hi) || !fromHex(hex.substr(16), g.lo))
        return {};
    return g;
}

AssetGuid AssetGuid::mint() {
    std::random_device rd;
    std::uniform_int_distribution<std::uint64_t> dist;
    std::mt19937_64 gen(
        (static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    AssetGuid g{dist(gen), dist(gen)};
    if (!g.valid()) g.lo = 1;
    return g;
}

void AssetTypeRegistry::add(AssetTypeDesc d) {
    for (std::string& e : d.extensions) e = lower(std::move(e));
    types_.push_back(std::move(d));
}

bool AssetTypeRegistry::loadFile(const std::string& path, Diagnostics& diag) {
    toml::parse_result res = toml::parse_file(path);
    if (!res) {
        diag.error("failed to parse " + path + ": "
                   + std::string(res.error().description()));
        return false;
    }
    const toml::array* arr = res.table()["type"].as_array();
    if (!arr) {
        diag.error("no [[type]] entries in " + path);
        return false;
    }
    for (const toml::node& n : *arr) {
        const toml::table* t = n.as_table();
        if (!t) continue;
        AssetTypeDesc d;
        d.id = (*t)["id"].value_or(std::string{});
        if (d.id.empty()) {
            diag.warn("skipping [[type]] with no id in " + path);
            continue;
        }
        d.display = (*t)["display"].value_or(std::string{});
        if (const toml::array* ext = (*t)["ext"].as_array())
            for (const toml::node& e : *ext)
                if (auto s = e.value<std::string>()) d.extensions.push_back(*s);
        add(std::move(d));
    }
    return true;
}

std::string_view AssetTypeRegistry::typeForExtension(std::string_view ext) const {
    const std::string e = lower(std::string(ext));
    for (const AssetTypeDesc& d : types_)
        for (const std::string& x : d.extensions)
            if (x == e) return d.id;
    return {};
}

const AssetTypeDesc* AssetTypeRegistry::find(std::string_view id) const {
    for (const AssetTypeDesc& d : types_)
        if (d.id == id) return &d;
    return nullptr;
}

std::string sidecarPath(const std::string& assetPath) {
    return assetPath + ".limeasset";
}

bool readSidecar(const std::string& path, AssetRecord& out, Diagnostics& diag) {
    toml::parse_result res = toml::parse_file(path);
    if (!res) {
        diag.warn("ignoring unreadable sidecar " + path + ": "
                  + std::string(res.error().description()));
        return false;
    }
    const toml::table& t = res.table();
    out.guid = AssetGuid::parse(t["asset"]["guid"].value_or(std::string{}));
    if (!out.guid.valid()) {
        diag.warn("sidecar has no usable guid: " + path);
        return false;
    }
    out.type    = t["asset"]["type"].value_or(std::string{});
    out.relPath = t["asset"]["path"].value_or(std::string{});
    out.size    = t["asset"]["size"].value_or(std::uint64_t{0});
    out.hash    = static_cast<std::uint64_t>(
        std::strtoull(t["asset"]["hash"].value_or(std::string{"0"}).c_str(),
                      nullptr, 16));

    out.importSettings.clear();
    if (const toml::table* imp = t["import"].as_table())
        for (auto&& [k, v] : *imp) {
            std::ostringstream ss;
            ss << toml::json_formatter{v};
            out.importSettings.emplace_back(std::string(k.str()), ss.str());
        }
    std::sort(out.importSettings.begin(), out.importSettings.end());
    return true;
}

bool writeSidecar(const std::string& path, const AssetRecord& rec,
                  Diagnostics& diag) {
    std::ostringstream o;
    o << "# LimeVSC asset. Keep this file beside its asset and commit it:\n"
      << "# the guid below is what every scene reference points at, so losing\n"
      << "# it breaks those references while renaming the asset does not.\n\n"
      << "[asset]\n"
      << "guid = \"" << rec.guid.str() << "\"\n"
      << "type = \"" << rec.type << "\"\n"
      << "path = \"" << rec.relPath << "\"\n"
      << "size = " << rec.size << "\n"
      << "hash = \"" << toHex(rec.hash) << "\"\n";
    if (!rec.importSettings.empty()) {
        o << "\n[import]\n";
        for (const auto& [k, v] : rec.importSettings) o << k << " = " << v << "\n";
    }

    const std::string text = o.str();
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot write " + path);
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

void AssetDatabase::index() {
    byGuid_.clear();
    for (std::size_t i = 0; i < records_.size(); ++i)
        byGuid_[records_[i].guid] = i;
}

void AssetDatabase::scan(const std::string& root, const AssetTypeRegistry& types,
                         Diagnostics& diag) {
    scanBegin(root, types, diag);
    std::string detail;
    while (scanStep(SIZE_MAX, detail, diag) < 1.0f) {}
    scanEnd();
}

void AssetDatabase::scanBegin(const std::string& root,
                              const AssetTypeRegistry& types,
                              Diagnostics& diag) {
    root_ = root;
    types_ = &types;
    records_.clear();
    byGuid_.clear();
    sidecars_.clear();
    sources_.clear();
    cursor_ = 0;

    std::error_code ec;
    const fs::path content = fs::path(root) / "content";
    content_ = content.string();
    if (root.empty() || !fs::exists(content, ec)) return;

    for (auto it = fs::recursive_directory_iterator(content, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() == ".limeasset") {
            Pending o;
            o.sidecar = it->path().string();
            if (readSidecar(o.sidecar, o.rec, diag)) sidecars_.push_back(std::move(o));
        } else if (!types.typeForExtension(it->path().extension().string()).empty()) {
            sources_.push_back(it->path().string());
        }
    }
}

float AssetDatabase::scanStep(std::size_t budget, std::string& detail,
                              Diagnostics& diag) {
    if (!types_) return 1.0f;

    std::error_code ec;
    for (std::size_t n = 0; n < budget && cursor_ < sources_.size(); ++n, ++cursor_) {
        const fs::path p(sources_[cursor_]);

        AssetRecord rec;
        rec.type = std::string(types_->typeForExtension(p.extension().string()));
        rec.relPath = generic(fs::relative(p, fs::path(content_), ec));
        rec.hash = hashFile(p, rec.size);
        detail = rec.relPath;

        const std::string side = sidecarPath(p.string());
        auto own = std::find_if(sidecars_.begin(), sidecars_.end(),
                                [&](const Pending& o) { return o.sidecar == side; });

        if (own != sidecars_.end()) {
            own->claimed = true;
            rec.guid = own->rec.guid;
            rec.importSettings = own->rec.importSettings;
            if (own->rec.relPath != rec.relPath || own->rec.hash != rec.hash
                || own->rec.size != rec.size)
                writeSidecar(side, rec, diag);
        } else {
            auto twin = std::find_if(
                sidecars_.begin(), sidecars_.end(), [&](const Pending& o) {
                    return !o.claimed && o.rec.hash == rec.hash
                           && o.rec.size == rec.size;
                });
            if (twin != sidecars_.end()) {
                twin->claimed = true;
                rec.guid = twin->rec.guid;
                rec.importSettings = twin->rec.importSettings;
                diag.info("asset '" + twin->rec.relPath + "' was renamed to '"
                          + rec.relPath + "'; its references still resolve");
                std::error_code rmec;
                fs::remove(twin->sidecar, rmec);
            } else {
                rec.guid = AssetGuid::mint();
                diag.info("imported " + rec.relPath);
            }
            writeSidecar(side, rec, diag);
        }
        records_.push_back(std::move(rec));
    }

    if (sources_.empty()) return 1.0f;
    return static_cast<float>(cursor_) / static_cast<float>(sources_.size());
}

void AssetDatabase::scanEnd() {
    for (Pending& o : sidecars_) {
        if (o.claimed) continue;
        o.rec.missing = true;
        records_.push_back(std::move(o.rec));
    }
    sidecars_.clear();
    sources_.clear();
    cursor_ = 0;
    types_ = nullptr;

    std::sort(records_.begin(), records_.end(),
              [](const AssetRecord& a, const AssetRecord& b) {
                  if (a.relPath != b.relPath) return a.relPath < b.relPath;
                  return a.guid.str() < b.guid.str();
              });
    index();
}

const AssetRecord* AssetDatabase::find(AssetGuid g) const {
    const auto it = byGuid_.find(g);
    return it == byGuid_.end() ? nullptr : &records_[it->second];
}

const AssetRecord* AssetDatabase::findByPath(std::string_view relPath) const {
    for (const AssetRecord& r : records_)
        if (!r.missing && r.relPath == relPath) return &r;
    return nullptr;
}

std::string AssetDatabase::pathOf(AssetGuid g) const {
    const AssetRecord* r = find(g);
    return (r && !r->missing) ? r->relPath : std::string();
}

AssetGuid AssetDatabase::parseRef(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    constexpr std::string_view kPrefix = "asset:";
    if (value.size() <= kPrefix.size()
        || value.compare(0, kPrefix.size(), kPrefix) != 0)
        return {};
    return AssetGuid::parse(value.substr(kPrefix.size()));
}

std::string AssetDatabase::makeRef(AssetGuid g) {
    return "asset:" + g.str();
}

}
