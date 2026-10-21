// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// The test descriptor for the device catalog's AlpacaHTTP consumers
// (open-astro#664): a vendor no router arm knows ("zzz", focuser) carrying
// every field kind, one enum, one range of each numeric shape, one
// applies_when, one record list with a nested required field and one secret.
// Shared by test_catalog_json.cpp (the JSON bridge) and test_routing.cpp (the
// devicecatalog endpoint and the router's catalog consult). Everything a
// Schema borrows from here has static storage, as schema.h requires.
//
// The stub driver reports the config the factory was handed in its Name, so a
// test can tell a normalized config from the raw one through configureddevices.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpacadriver.h>
#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacahttp::test_catalog {

namespace catalog = alpacacore::catalog;

inline const char* const kModes[] = {"a", "b"};

inline const catalog::Field<bool> kEnabled{.key = "enabled", .default_value = false};
inline const catalog::Field<std::int64_t> kCount{.key = "count", .default_value = 1, .min = 1, .max = 8};
inline const catalog::Field<double> kRatio{.key = "ratio", .default_value = 1.5, .min = 0.5};
inline const catalog::Field<std::string> kMode{
    .key = "mode", .default_value = "a", .role = catalog::Role::Discriminator, .allowed_values = kModes};
inline const catalog::Field<std::string> kHost{
    .key = "host", .default_value = "", .role = catalog::Role::Host, .applies_when = catalog::AppliesWhen{"mode", "b"}};
inline const catalog::Field<std::string> kToken{.key = "token", .default_value = "", .role = catalog::Role::Secret};
inline const catalog::Field<std::vector<std::string>> kFilterNames{.key = "filterNames", .default_value = {}};
inline const catalog::Field<std::string> kPortName{.key = "name", .default_value = "", .required = true};
inline const catalog::Field<bool> kPortPwm{.key = "pwm", .default_value = false};

inline const std::vector<catalog::FieldRef>& port_fields() {
    static const std::vector<catalog::FieldRef> f{kPortName.ref(), kPortPwm.ref()};
    return f;
}

inline const catalog::Field<std::vector<catalog::DeviceConfig>>& ports_field() {
    static const catalog::Field<std::vector<catalog::DeviceConfig>> f{
        .key = "ports", .default_value = {}, .record_fields = port_fields()};
    return f;
}

inline const std::vector<catalog::FieldRef>& fields() {
    static const std::vector<catalog::FieldRef> f{kEnabled.ref(),     kCount.ref(),       kRatio.ref(),
                                                  kMode.ref(),        kHost.ref(),        kToken.ref(),
                                                  kFilterNames.ref(), ports_field().ref()};
    return f;
}

inline const catalog::DeviceKey kKey{"zzz", alpacacore::DeviceType::Focuser};

inline catalog::Schema make_schema() {
    catalog::Schema s;
    s.key = kKey;
    s.display_name = "Test Focuser";
    s.build_option = "ALPACACORE_ENABLE_ZZZ";
    s.fields = fields();
    return s;
}

// The device numbers whose stub was asked to adopt_legacy_state(), in order.
inline std::vector<int>& adopted_device_numbers() {
    static std::vector<int> numbers;
    return numbers;
}

class StubFocuser final : public alpacacore::AlpacaDriver {
public:
    StubFocuser(int number, std::string name) : number_(number), name_(std::move(name)) {}
    int get_device_number() const override { return number_; }
    std::string get_name() const override { return name_; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Focuser; }
    std::string fallback_unique_id() const override { return "zzz-focuser-" + std::to_string(number_); }
    std::string get_description() const override { return "test descriptor stub"; }
    std::string get_driver_info() const override { return "test descriptor stub"; }
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
    void adopt_legacy_state() override { adopted_device_numbers().push_back(number_); }

private:
    int number_;
    std::string name_;
    bool connected_{false};
};

// The Name the stub reports for the config its factory received.
inline std::string stub_name(const catalog::DeviceConfig& c) {
    return "zzz count=" + std::to_string(c.get(kCount)) + " mode=" + c.get(kMode);
}

inline catalog::Factory make_factory() {
    return catalog::Factory{kKey, [](const catalog::DeviceConfig& c, int n) {
                                return std::unique_ptr<alpacacore::AlpacaDriver>(new StubFocuser(n, stub_name(c)));
                            }};
}

// A factory with a bind hook, for the router's peer and learned-config handling:
// it refuses a `count` another stored entry of the key holds, learns `host`
// ("peers=<N>", N = the stored entries it was shown, itself included when stored)
// and erases `ratio`. No `create`: the stub is only reachable through bind.
inline catalog::Factory make_bind_factory() {
    catalog::Factory factory;
    factory.key = kKey;
    factory.bind = [](const catalog::DeviceConfig& c, int n, const catalog::BindContext& context) {
        catalog::BindResult result;
        for (const catalog::PeerEntry& peer : context.peers) {
            if (peer.device_number != n && peer.config.get(kCount) == c.get(kCount)) {
                result.rejection = "Invalid value for count: another zzz focuser already uses it";
                return result;
            }
        }
        result.driver = std::unique_ptr<alpacacore::AlpacaDriver>(new StubFocuser(n, stub_name(c)));
        result.learned.set(kHost.key, "peers=" + std::to_string(context.peers.size()));
        result.erased.emplace_back(kRatio.key);
        return result;
    };
    return factory;
}

inline void add_schema_and_bind_factory(catalog::DeviceCatalog& c) {
    c.add(make_schema());
    c.add(make_bind_factory());
}

inline void add_schema(catalog::DeviceCatalog& c) { c.add(make_schema()); }

inline void add_schema_and_factory(catalog::DeviceCatalog& c) {
    c.add(make_schema());
    c.add(make_factory());
}

}  // namespace alpacahttp::test_catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
