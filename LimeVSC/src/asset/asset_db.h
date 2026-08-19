#pragma once

#include "limecore.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lime {

struct AssetGuid {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    bool valid() const noexcept { return hi != 0 || lo != 0; }
    std::string str() const;
    static AssetGuid parse(std::string_view hex);
    static AssetGuid mint();

    friend bool operator==(AssetGuid, AssetGuid) = default;
};

struct AssetGuidHash {
    std::size_t operator()(AssetGuid g) const noexcept {
        return static_cast<std::size_t>(g.hi ^ (g.lo * 1099511628211ull));
    }
};

struct AssetTypeDesc {
    std::string id;
    std::string display;
    std::vector<std::string> extensions;
};

class AssetTypeRegistry {
public:
    bool loadFile(const std::string& path, Diagnostics& diag);
    void add(AssetTypeDesc d);
    std::string_view typeForExtension(std::string_view ext) const;
    std::span<const AssetTypeDesc> all() const noexcept { return types_; }
    const AssetTypeDesc* find(std::string_view id) const;

private:
    std::vector<AssetTypeDesc> types_;
};

struct AssetRecord {
    AssetGuid   guid;
    std::string relPath;
    std::string type;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
    bool missing = false;
    std::vector<std::pair<std::string, std::string>> importSettings;
};

class AssetDatabase {
public:
    void scan(const std::string& root, const AssetTypeRegistry& types,
              Diagnostics& diag);

    void  scanBegin(const std::string& root, const AssetTypeRegistry& types,
                    Diagnostics& diag);
    float scanStep(std::size_t budget, std::string& detail, Diagnostics& diag);
    void  scanEnd();

    const AssetRecord* find(AssetGuid g) const;
    const AssetRecord* findByPath(std::string_view relPath) const;
    std::span<const AssetRecord> all() const noexcept { return records_; }

    std::string pathOf(AssetGuid g) const;

    static AssetGuid parseRef(std::string_view value);
    static std::string makeRef(AssetGuid g);

    std::size_t size() const noexcept { return records_.size(); }
    const std::string& root() const noexcept { return root_; }

private:
    void index();

    struct Pending {
        AssetRecord rec;
        std::string sidecar;
        bool        claimed = false;
    };
    std::vector<Pending>     sidecars_;
    std::vector<std::string> sources_;
    std::size_t              cursor_ = 0;
    std::string              content_;
    const AssetTypeRegistry* types_ = nullptr;

    std::string root_;
    std::vector<AssetRecord> records_;
    std::unordered_map<AssetGuid, std::size_t, AssetGuidHash> byGuid_;
};

std::string sidecarPath(const std::string& assetPath);

bool readSidecar(const std::string& path, AssetRecord& out, Diagnostics& diag);
bool writeSidecar(const std::string& path, const AssetRecord& rec,
                  Diagnostics& diag);

}
