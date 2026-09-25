// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Tests for the device catalog (#650), over a stub descriptor and stub driver.
// Fake-only: no hardware, no vendor SDK.

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catch2_compat.h"

// Designated initializers of Field<T> leave most members defaulted on purpose.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

using namespace alpacacore;
using namespace alpacacore::catalog;

namespace {

class StubDriver final : public AlpacaDriver {
public:
    explicit StubDriver(int number) : number_(number) {}
    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "stub"; }
    DeviceType get_device_type() const override { return DeviceType::Switch; }
    std::string get_unique_id() const override { return "stub-id"; }
    std::string get_description() const override { return "stub device"; }
    std::string get_driver_info() const override { return "stub driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return connected_; }
    void set_connected(bool c) override { connected_ = c; }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view n, std::string_view) override { return std::string(n); }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return "ok"; }
    bool command_bool(std::string_view, bool) override { return true; }
    std::string command_string(std::string_view, bool) override { return "ok"; }

private:
    int number_;
    bool connected_{false};
};

const char* const kSwitchTypes[] = {"basic", "stellavita"};

const Field<std::string> kSwitchType{
    .key = "switchType", .default_value = "basic", .role = Role::Discriminator, .allowed_values = kSwitchTypes};
const Field<std::string> kPortName{.key = "name", .default_value = "", .required = true};
const Field<bool> kPortPwm{.key = "pwm", .default_value = false};
const Field<double> kPortMax{.key = "maxValue", .default_value = 100.0, .min = 0.0, .max = 100.0};

const std::vector<FieldRef>& port_fields() {
    static const std::vector<FieldRef> f{kPortName.ref(), kPortPwm.ref(), kPortMax.ref()};
    return f;
}

const Field<std::vector<DeviceConfig>>& ports_field() {
    static const Field<std::vector<DeviceConfig>> f{
        .key = "ports", .default_value = {}, .record_fields = port_fields()};
    return f;
}
const Field<std::vector<std::string>> kFilterNames{.key = "filterNames", .default_value = {}};
const Field<std::int64_t> kEnumIndex{
    .key = "enumerationIndex", .default_value = 0, .role = Role::EnumerationIndex, .min = 0};
const Field<std::string> kDeviceId{.key = "deviceId", .default_value = "", .role = Role::DeviceId};
const Field<std::string> kPortPath{.key = "portPath", .default_value = "", .required = true, .role = Role::PortPath};
const Field<std::string> kHost{.key = "host", .default_value = "", .role = Role::Host};
const Field<std::string> kApiKey{.key = "apiKey", .default_value = "", .role = Role::Secret};
const Field<std::int64_t> kStellaChannel{
    .key = "stellaVitaChannel", .default_value = 0, .applies_when = AppliesWhen{"switchType", "stellavita"}};
const Field<std::int64_t> kPollMs{.key = "pollMs", .default_value = 1000, .min = 100, .max = 60000};

const DeviceKey kStubKey{"stub", DeviceType::Switch};

const std::vector<FieldRef>& stub_fields() {
    static const std::vector<FieldRef> f{
        kSwitchType.ref(), ports_field().ref(), kFilterNames.ref(), kEnumIndex.ref(),     kDeviceId.ref(),
        kPortPath.ref(),   kHost.ref(),         kApiKey.ref(),      kStellaChannel.ref(), kPollMs.ref()};
    return f;
}

// Adds the stub descriptor to a catalog instance; no process-wide singleton.
void register_test_descriptors(DeviceCatalog& catalog, bool with_factory) {
    Schema schema;
    schema.key = kStubKey;
    schema.display_name = "Stub Switch";
    schema.build_option = "ALPACACORE_ENABLE_STUB";
    schema.fields = stub_fields();
    // Cross-field rule: switchType stellavita needs stellaVitaChannel.
    schema.normalize = [](const DeviceConfig& in, Source source) {
        NormalizeResult r;
        r.config = in;
        auto type = in.find(kSwitchType);
        if (type && *type == "stellavita" && !in.find(kStellaChannel)) {
            std::string msg = "stellaVitaChannel is required when switchType is stellavita";
            if (source == Source::Api)
                r.rejection = msg;
            else
                r.warnings.push_back(msg);
        }
        return r;
    };
    catalog.add(std::move(schema));
    if (with_factory) {
        catalog.add(Factory{
            kStubKey, [](const DeviceConfig&, int n) { return std::unique_ptr<AlpacaDriver>(new StubDriver(n)); }});
    }
}

DeviceConfig valid_config() {
    DeviceConfig c;
    c.set("switchType", std::string{"basic"});
    c.set("portPath", std::string{"/dev/ttyUSB0"});
    c.set("pollMs", std::int64_t{1000});
    return c;
}

DeviceConfig make_port(const std::string& name, bool has_name = true) {
    DeviceConfig p;
    if (has_name) p.set("name", name);
    p.set("pwm", true);
    p.set("maxValue", 50.0);
    return p;
}

bool mentions(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

TEST_CASE("Api normalize rejects missing required, out-of-enum, out-of-range", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);

    DeviceConfig missing = valid_config();
    missing.erase("portPath");
    auto r1 = catalog.normalize(kStubKey, missing, Source::Api);
    REQUIRE(r1.rejection.has_value());
    CHECK(mentions(*r1.rejection, "portPath"));

    DeviceConfig bad_enum = valid_config();
    bad_enum.set("switchType", std::string{"bogus"});
    auto r2 = catalog.normalize(kStubKey, bad_enum, Source::Api);
    REQUIRE(r2.rejection.has_value());
    CHECK(mentions(*r2.rejection, "switchType"));

    DeviceConfig bad_range = valid_config();
    bad_range.set("pollMs", std::int64_t{5});
    auto r3 = catalog.normalize(kStubKey, bad_range, Source::Api);
    REQUIRE(r3.rejection.has_value());
    CHECK(mentions(*r3.rejection, "pollMs"));

    DeviceConfig cross = valid_config();
    cross.set("switchType", std::string{"stellavita"});
    auto r4 = catalog.normalize(kStubKey, cross, Source::Api);
    REQUIRE(r4.rejection.has_value());
    CHECK(mentions(*r4.rejection, "stellaVitaChannel"));

    auto ok = catalog.normalize(kStubKey, valid_config(), Source::Api);
    CHECK_FALSE(ok.rejection.has_value());
    CHECK(ok.warnings.empty());
}

TEST_CASE("Persisted normalize warns and keeps the device registrable", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);

    // Required missing: kept raw (absent). Router::reject_invalid_config() warns and
    // still registers with an empty port path; connect fails later.
    DeviceConfig missing = valid_config();
    missing.erase("portPath");
    auto r1 = catalog.normalize(kStubKey, missing, Source::Persisted);
    CHECK_FALSE(r1.rejection.has_value());
    REQUIRE(r1.warnings.size() == 1);
    CHECK(mentions(r1.warnings[0], "portPath"));
    CHECK_FALSE(r1.config.find(kPortPath).has_value());
    CHECK(r1.config.get(kPortPath).empty());

    // Out-of-enum: replaced by the field default, like normalize_persisted_connection_type()
    // substituting "serial".
    DeviceConfig bad_enum = valid_config();
    bad_enum.set("switchType", std::string{"bogus"});
    auto r2 = catalog.normalize(kStubKey, bad_enum, Source::Persisted);
    CHECK_FALSE(r2.rejection.has_value());
    REQUIRE(r2.warnings.size() == 1);
    CHECK(mentions(r2.warnings[0], "switchType"));
    REQUIRE(r2.config.find(kSwitchType).has_value());
    CHECK(*r2.config.find(kSwitchType) == "basic");

    // Out-of-range: dropped to unset, like read_site_coordinates() ignoring the coordinate.
    DeviceConfig bad_range = valid_config();
    bad_range.set("pollMs", std::int64_t{5});
    auto r3 = catalog.normalize(kStubKey, bad_range, Source::Persisted);
    CHECK_FALSE(r3.rejection.has_value());
    REQUIRE(r3.warnings.size() == 1);
    CHECK(mentions(r3.warnings[0], "pollMs"));
    CHECK_FALSE(r3.config.find(kPollMs).has_value());
    CHECK(r3.config.get(kPollMs) == 1000);

    // Cross-field failure is a warning too.
    DeviceConfig cross = valid_config();
    cross.set("switchType", std::string{"stellavita"});
    auto r4 = catalog.normalize(kStubKey, cross, Source::Persisted);
    CHECK_FALSE(r4.rejection.has_value());
    REQUIRE(r4.warnings.size() == 1);
}

TEST_CASE("Record list round-trips and reports the failing record index", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);

    DeviceConfig c = valid_config();
    c.set("ports", std::vector<DeviceConfig>{make_port("a"), make_port("b")});

    auto n = catalog.normalize(kStubKey, c, Source::Api);
    REQUIRE_FALSE(n.rejection.has_value());
    for (const DeviceConfig* cfg : {&n.config}) {
        auto ports = cfg->find(ports_field());
        REQUIRE(ports.has_value());
        REQUIRE(ports->size() == 2);
        CHECK((*ports)[1].get(kPortName) == "b");
        CHECK((*ports)[1].get(kPortPwm) == true);
        CHECK((*ports)[1].get(kPortMax) == 50.0);
    }
    auto s = catalog.sanitize(kStubKey, n.config);
    auto sp = s.find(ports_field());
    REQUIRE(sp.has_value());
    REQUIRE(sp->size() == 2);
    CHECK((*sp)[0].get(kPortName) == "a");
    CHECK((*sp)[0].get(kPortPwm) == true);
    CHECK((*sp)[0].get(kPortMax) == 50.0);

    DeviceConfig bad = valid_config();
    bad.set("ports", std::vector<DeviceConfig>{make_port("a"), make_port("", false)});
    auto r = catalog.normalize(kStubKey, bad, Source::Api);
    REQUIRE(r.rejection.has_value());
    CHECK(mentions(*r.rejection, "ports[1]"));
    CHECK(mentions(*r.rejection, "name"));
}

TEST_CASE("String list round-trips unchanged", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    DeviceConfig c = valid_config();
    const std::vector<std::string> names{"L", "R", "G", "B"};
    c.set("filterNames", names);
    auto n = catalog.normalize(kStubKey, c, Source::Api);
    REQUIRE_FALSE(n.rejection.has_value());
    REQUIRE(n.config.find(kFilterNames).has_value());
    CHECK(*n.config.find(kFilterNames) == names);
    auto s = catalog.sanitize(kStubKey, n.config);
    REQUIRE(s.find(kFilterNames).has_value());
    CHECK(*s.find(kFilterNames) == names);
}

TEST_CASE("Sanitize drops secrets and undeclared keys, ignores applies_when", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    DeviceConfig c = valid_config();
    c.set("apiKey", std::string{"hunter2"});
    c.set("host", std::string{"10.0.0.5"});
    c.set("deviceId", std::string{"abc"});
    c.set("enumerationIndex", std::int64_t{2});
    c.set("stellaVitaChannel", std::int64_t{3});  // switchType is basic: does not apply
    c.set("undeclared", std::string{"x"});

    auto s = catalog.sanitize(kStubKey, c);
    CHECK_FALSE(s.has("apiKey"));
    CHECK_FALSE(s.has("undeclared"));
    CHECK(*s.find(kHost) == "10.0.0.5");
    CHECK(*s.find(kDeviceId) == "abc");
    CHECK(*s.find(kEnumIndex) == 2);
    CHECK(*s.find(kPortPath) == "/dev/ttyUSB0");
    CHECK(*s.find(kSwitchType) == "basic");
    REQUIRE(s.find(kStellaChannel).has_value());
    CHECK(*s.find(kStellaChannel) == 3);
}

TEST_CASE("Describe reports availability from the registered factory", "[catalog]") {
    {
        DeviceCatalog with;
        register_test_descriptors(with, true);
        auto v = with.describe();
        REQUIRE(v.size() == 1);
        CHECK(v[0].key == kStubKey);
        CHECK(v[0].display_name == "Stub Switch");
        CHECK(v[0].build_option == "ALPACACORE_ENABLE_STUB");
        CHECK(v[0].available);
        CHECK(v[0].fields.size() == stub_fields().size());
    }
    DeviceCatalog without;
    register_test_descriptors(without, false);
    auto v = without.describe();
    REQUIRE(v.size() == 1);
    CHECK_FALSE(v[0].available);
}

TEST_CASE("Create builds the stub driver or names the build option", "[catalog]") {
    DeviceCatalog with;
    register_test_descriptors(with, true);
    auto d = with.create(kStubKey, valid_config(), 7);
    REQUIRE(d != nullptr);
    CHECK(d->get_device_number() == 7);

    DeviceCatalog without;
    register_test_descriptors(without, false);
    try {
        (void)without.create(kStubKey, valid_config(), 7);
        FAIL("create should have thrown");
    } catch (const std::runtime_error& e) {
        CHECK(mentions(e.what(), "ALPACACORE_ENABLE_STUB"));
    }
}

TEST_CASE("Unset record field is distinct from an empty record list", "[catalog]") {
    DeviceConfig fresh;
    CHECK_FALSE(fresh.has("ports"));
    CHECK_FALSE(fresh.find(ports_field()).has_value());

    DeviceConfig empty;
    empty.set("ports", std::vector<DeviceConfig>{});
    CHECK(empty.has("ports"));
    auto p = empty.find(ports_field());
    REQUIRE(p.has_value());
    CHECK(p->empty());
}

TEST_CASE("NaN in a ranged double is out of range", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    DeviceConfig cfg = valid_config();
    DeviceConfig port = make_port("p");
    port.set("maxValue", std::numeric_limits<double>::quiet_NaN());
    cfg.set("ports", std::vector<DeviceConfig>{port});

    auto api = catalog.normalize(kStubKey, cfg, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(mentions(*api.rejection, "maxValue"));

    auto persisted = catalog.normalize(kStubKey, cfg, Source::Persisted);
    REQUIRE(persisted.warnings.size() == 1);
    CHECK(mentions(persisted.warnings[0], "maxValue"));
}

TEST_CASE("A cross-field rejection never empties a Persisted config", "[catalog]") {
    DeviceCatalog catalog;
    Schema schema;
    schema.key = DeviceKey{"wipe", DeviceType::Switch};
    schema.display_name = "Wipe";
    schema.build_option = "ALPACACORE_ENABLE_WIPE";
    schema.fields = stub_fields();
    // Careless callback: rejects without copying the input config.
    schema.normalize = [](const DeviceConfig&, Source) {
        NormalizeResult r;
        r.rejection = "cross-field failure";
        return r;
    };
    catalog.add(std::move(schema));

    auto r = catalog.normalize(DeviceKey{"wipe", DeviceType::Switch}, valid_config(), Source::Persisted);
    CHECK_FALSE(r.rejection.has_value());
    REQUIRE(r.warnings.size() == 1);
    CHECK(mentions(r.warnings[0], "cross-field failure"));
    CHECK(r.config.find(kPortPath).has_value());
}

TEST_CASE("Unset field inside a record stays unset through normalize and sanitize", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    DeviceConfig port;
    port.set("name", std::string{"p"});  // pwm and maxValue left unset
    DeviceConfig cfg = valid_config();
    cfg.set("ports", std::vector<DeviceConfig>{port});
    DeviceConfig empty = valid_config();
    empty.set("ports", std::vector<DeviceConfig>{});

    for (Source s : {Source::Api, Source::Persisted}) {
        auto n = catalog.normalize(kStubKey, cfg, s);
        REQUIRE_FALSE(n.rejection.has_value());
        auto clean = catalog.sanitize(kStubKey, n.config);
        auto ports = clean.get(ports_field());
        REQUIRE(ports.size() == 1);
        CHECK_FALSE(ports[0].find(kPortPwm).has_value());
        CHECK_FALSE(ports[0].find(kPortMax).has_value());

        auto ne = catalog.sanitize(kStubKey, catalog.normalize(kStubKey, empty, s).config);
        CHECK(ne.find_value("ports") != nullptr);
        CHECK(ne.get(ports_field()).empty());
    }
}

TEST_CASE("Persisted record with bad fields warns with the index and keeps the device", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    DeviceConfig bad = make_port("", false);
    bad.set("maxValue", 500.0);
    DeviceConfig cfg = valid_config();
    cfg.set("ports", std::vector<DeviceConfig>{make_port("ok"), bad});
    auto r = catalog.normalize(kStubKey, cfg, Source::Persisted);
    CHECK_FALSE(r.rejection.has_value());
    REQUIRE(r.warnings.size() == 2);
    CHECK(mentions(r.warnings[0], "ports[1].name"));
    CHECK(mentions(r.warnings[1], "ports[1].maxValue"));
    CHECK(r.config.get(ports_field()).size() == 2);
}

TEST_CASE("A value of the wrong type is rejected from the API and erased from a persisted config", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    DeviceConfig cfg = valid_config();
    cfg.set("pollMs", std::string{"1000"});  // declared int64

    CHECK_FALSE(cfg.find(kPollMs).has_value());  // find() gives nullopt for a different type

    auto api = catalog.normalize(kStubKey, cfg, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(mentions(*api.rejection, "pollMs"));
    CHECK(mentions(*api.rejection, "wrong type"));

    auto persisted = catalog.normalize(kStubKey, cfg, Source::Persisted);
    CHECK_FALSE(persisted.rejection.has_value());
    REQUIRE(persisted.warnings.size() == 1);
    CHECK(mentions(persisted.warnings[0], "pollMs"));
    CHECK_FALSE(persisted.config.has("pollMs"));  // erased, not kept with the wrong type

    // The same rule applies inside a record: an int64 where the record declares a double.
    DeviceConfig port = make_port("p1");
    port.set("maxValue", std::int64_t{50});
    DeviceConfig with_ports = valid_config();
    with_ports.set("ports", std::vector<DeviceConfig>{port});
    auto nested = catalog.normalize(kStubKey, with_ports, Source::Api);
    REQUIRE(nested.rejection.has_value());
    CHECK(mentions(*nested.rejection, "ports[0].maxValue"));
}

TEST_CASE("An unknown key is rejected from the API, warned for persisted, and cannot be created", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);
    const DeviceKey unknown{"nosuchvendor", DeviceType::Switch};
    const DeviceConfig cfg = valid_config();

    auto api = catalog.normalize(unknown, cfg, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(mentions(*api.rejection, "unknown device"));

    auto persisted = catalog.normalize(unknown, cfg, Source::Persisted);
    CHECK_FALSE(persisted.rejection.has_value());
    REQUIRE(persisted.warnings.size() == 1);
    CHECK(mentions(persisted.warnings[0], "unknown device"));
    CHECK(persisted.config.has("portPath"));  // the config comes back as given

    CHECK(catalog.sanitize(unknown, cfg).find_value("portPath") == nullptr);

    try {
        (void)catalog.create(unknown, cfg, 1);
        FAIL("create should have thrown");
    } catch (const std::runtime_error& e) {
        CHECK(mentions(e.what(), "cannot create unknown device"));
    }
}

TEST_CASE("Adding a schema or factory with an existing key replaces it", "[catalog]") {
    DeviceCatalog catalog;
    register_test_descriptors(catalog, true);

    Schema replacement;
    replacement.key = kStubKey;
    replacement.display_name = "Replaced Switch";
    replacement.build_option = "ALPACACORE_ENABLE_STUB";
    replacement.fields = stub_fields();
    catalog.add(std::move(replacement));
    catalog.add(Factory{kStubKey, [](const DeviceConfig&, int n) {
                            return std::unique_ptr<AlpacaDriver>(new StubDriver(n + 100));
                        }});

    auto v = catalog.describe();
    REQUIRE(v.size() == 1);  // one entry per key, not two
    CHECK(v[0].display_name == "Replaced Switch");
    CHECK(v[0].available);
    auto d = catalog.create(kStubKey, valid_config(), 7);
    REQUIRE(d != nullptr);
    CHECK(d->get_device_number() == 107);  // the replacement factory built it
}

TEST_CASE("Sanitize drops a secret inside a record list", "[catalog]") {
    const Field<std::string> account_name{.key = "name", .default_value = ""};
    const Field<std::string> account_token{.key = "token", .default_value = "", .role = Role::Secret};
    const std::vector<FieldRef> account_fields{account_name.ref(), account_token.ref()};
    const Field<std::vector<DeviceConfig>> accounts{
        .key = "accounts", .default_value = {}, .record_fields = account_fields};
    const std::vector<FieldRef> fields{accounts.ref()};

    DeviceCatalog catalog;
    const DeviceKey key{"secretrecords", DeviceType::Switch};
    Schema schema;
    schema.key = key;
    schema.display_name = "Secret Records";
    schema.build_option = "ALPACACORE_ENABLE_STUB";
    schema.fields = fields;
    catalog.add(std::move(schema));

    DeviceConfig account;
    account.set("name", std::string{"main"});
    account.set("token", std::string{"hunter2"});
    DeviceConfig cfg;
    cfg.set("accounts", std::vector<DeviceConfig>{account});

    auto clean = catalog.sanitize(key, cfg).get(accounts);
    REQUIRE(clean.size() == 1);
    CHECK(clean[0].find(account_name) == std::optional<std::string>{"main"});
    CHECK_FALSE(clean[0].has("token"));
}
