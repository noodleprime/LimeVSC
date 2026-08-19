#include "api/data_provider.h"
#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

#include <set>

using namespace lime;

namespace {

void checkConformance(INodeProvider& p, TypeRegistry& types) {
    Diagnostics diag;
    std::vector<NodeDesc> descs;
    p.collect(types, descs, diag);

    INFO("provider: " << p.name());
    CHECK_FALSE(diag.hasErrors());
    CHECK_FALSE(descs.empty());

    std::set<std::string> ids;
    for (const NodeDesc& d : descs) {
        INFO("node: " << d.id);

        CHECK_FALSE(d.id.empty());
        CHECK_FALSE(d.emit.empty());
        CHECK(ids.insert(d.id).second);

        std::set<std::string> pinNames;
        int execIn = 0, dataOut = 0;
        for (const PinDesc& pin : d.pins) {
            INFO("pin: " << pin.name);
            CHECK_FALSE(pin.name.empty());
            CHECK(pinNames.insert(pin.name).second);

            if (pin.kind == PinKind::Exec) {
                CHECK_FALSE(pin.type.valid());
                if (pin.dir == PinDir::In) ++execIn;
            } else {
                CHECK(pin.type.valid());
                if (pin.dir == PinDir::Out) ++dataOut;
            }
        }

        if (d.isEvent) CHECK(execIn == 0);

        if (d.pure) {
            CHECK(dataOut > 0);
            CHECK_FALSE(d.hasExecPins());
        }
    }
}

}

TEST_CASE("DataFileProvider conforms on core.limenodes") {
    TypeRegistry types;
    Diagnostics diag;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);
    REQUIRE_FALSE(diag.hasErrors());

    DataFileProvider p(std::string(LIMEVSC_DATA_DIR) + "/core.limenodes",
                       "core", kPriorityCore);
    checkConformance(p, types);
}

TEST_CASE("core node set covers what a graph needs to be useful") {
    TypeRegistry types;
    Diagnostics diag;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);

    NodeRegistry reg;
    reg.addProvider(std::make_unique<DataFileProvider>(
        std::string(LIMEVSC_DATA_DIR) + "/core.limenodes", "core", kPriorityCore));
    reg.rebuild(types, diag);
    REQUIRE_FALSE(diag.hasErrors());

    for (const char* id : {"Lime.onInit", "Lime.onStart",
                           "Lime.onUpdate", "Lime.onClose"}) {
        INFO("event: " << id);
        const NodeDesc* d = reg.find(id);
        REQUIRE(d != nullptr);
        CHECK(d->isEvent);
        CHECK(d->emit == "struct:event");
    }

    REQUIRE(reg.find("core.raw") != nullptr);

    for (const char* id : {"core.branch", "core.while", "core.forNum",
                           "core.forIn", "core.break", "core.return"}) {
        INFO("flow: " << id);
        REQUIRE(reg.find(id) != nullptr);
    }
}

TEST_CASE("type coercion is directional") {
    TypeRegistry types;
    Diagnostics diag;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);

    const TypeId num  = types.find("number");
    const TypeId intg = types.find("integer");
    const TypeId str  = types.find("string");
    const TypeId any  = types.find("any");
    REQUIRE(num.valid());
    REQUIRE(intg.valid());

    CHECK(types.canConnect(num, num));
    CHECK(types.canConnect(intg, num));
    CHECK_FALSE(types.canConnect(num, intg));
    CHECK_FALSE(types.canConnect(num, str));
    CHECK(types.canConnect(num, any));
    CHECK(types.canConnect(any, num));
}

TEST_CASE("higher priority provider overrides lower on id collision") {
    TypeRegistry types;
    Diagnostics diag;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);

    NodeRegistry reg;
    reg.addProvider(std::make_unique<DataFileProvider>(
        std::string(LIMEVSC_DATA_DIR) + "/core.limenodes", "core", kPriorityCore));
    reg.addProvider(std::make_unique<DataFileProvider>(
        std::string(LIMEVSC_TEST_DIR) + "/data/override.limenodes",
        "test-override", kPriorityOverrides));
    reg.rebuild(types, diag);

    const NodeDesc* d = reg.find("core.add");
    REQUIRE(d != nullptr);
    CHECK(d->display == "Overridden Add");
    CHECK(d->category == "Patched");
}
