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

// The JSON bridge of the device catalog (open-astro#664, Part A): converts
// between a plain JSON device config and alpacacore::catalog::DeviceConfig,
// and serializes a DeviceCatalog's descriptors for the devicecatalog
// endpoint. Internal seam (like http/thread_join.h): not part of the public
// AlpacaHTTP API, so it lives under src/, not include/alpacahttp/.
//
// Rules:
//   1. config_from_json converts only declared keys (from the field list);
//      undeclared keys are dropped, exactly like Router::sanitize_device_config.
//   2. A JSON null or an absent key produces no entry in the resulting
//      DeviceConfig -- the #388 rule -- including inside a nested record.
//   3. A value of the wrong JSON kind throws AlpacaException(InvalidValue).
//      The message is byte-identical in shape to config_get()'s: "Device
//      config field '<name>' has the wrong type (got <nlohmann type_name>)".
//      The check is on the JSON kind, not on nlohmann's get<>(): a JSON bool
//      converts to int64 there, so `true` in an int field must still be
//      refused. A JSON float with no fractional part that fits an int64
//      (e.g. 1.0) in an Int field is accepted and stored as that integer,
//      as config_get<int>() accepted it. An Int field given a JSON bool or
//      a finite fractional number (1.5) is refused, a narrowing:
//      config_get<int>() took them as 1, 1 and 0. The message then is
//      "Device config field '<name>' must be a whole number (got <value>)",
//      <value> being the JSON dump. Every other wrong kind, and an
//      out-of-range float (e.g. 1e300), keeps the "has the wrong type" text.
//      A nested record field is named with its index, e.g.
//      "ports[1].name"; a bad list or a bad list element is named by the
//      list's own key ("ports", "filterNames"), never indexed, because the
//      failure is in the list's shape, not in one declared sub-field.
//   4. A JSON integer in a Double field is accepted and stored as the
//      integer it was; DeviceCatalog::normalize widens it to double before
//      the range check, so the value round-trips exactly through
//      json_from_config.
//   5. json_from_config(config_from_json(j)) == j, restricted to the keys
//      that were declared, present and non-null. It converts a DeviceConfig
//      by its STORED variant alternative, not by re-consulting a schema, so
//      it needs no field list.
//   6. describe_json emits the descriptor list ordered by (vendor,
//      deviceType); each field carries `default` for every scalar kind
//      (including a false/""/0 default) and for a list kind only when its
//      default is non-empty; an int field's `range` bounds are JSON
//      integers, a double field's are JSON doubles; `deviceType` is
//      device_type_to_string() lower-cased.

#include <alpacacore/catalog/device_catalog.h>

#include <nlohmann/json.hpp>
#include <span>

namespace alpacahttp::catalog_json {

// Converts the declared, present, non-null keys of `json` into a DeviceConfig,
// per the rules above. Throws alpacacore::AlpacaException(InvalidValue) on a
// JSON-kind mismatch.
alpacacore::catalog::DeviceConfig config_from_json(const nlohmann::json& json,
                                                   std::span<const alpacacore::catalog::FieldRef> fields);

// Convenience overload forwarding schema.fields (DeviceCatalog::find_schema is
// private, so a caller holding only a DescriptorView passes its `fields` span
// to the overload above instead).
alpacacore::catalog::DeviceConfig config_from_json(const nlohmann::json& json,
                                                   const alpacacore::catalog::Schema& schema);

// The inverse of config_from_json: every entry in `config` becomes a JSON key,
// converted by the ConfigValue alternative it actually holds.
nlohmann::json json_from_config(const alpacacore::catalog::DeviceConfig& config);

// The full descriptor list of `catalog`, in the shape GET
// /management/v1/devicecatalog serves, ordered by (vendor, deviceType).
nlohmann::json describe_json(const alpacacore::catalog::DeviceCatalog& catalog);

}  // namespace alpacahttp::catalog_json
