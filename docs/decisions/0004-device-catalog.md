# Device catalog

Status: accepted

## Context

What a device's configuration looks like is written in several places. `AlpacaHTTP/src/http/router.cpp` builds every driver in a chain of registration arms, one per (vendor, device), validates and normalizes their configs there, and strips secrets in a separate chain of sanitize arms. That made the router include vendor headers from every vendor (40 includes at the time). `AlpacaHTTP/web/app.js` and `AlpacaHTTP/web/index.html` keep a second, hand-written copy of the field lists, the vendor availability blocks, the edit-population chain and `INDEX_FIELDS`. Adding a device, or a field, touches both libraries and the UI, and the copies drift: #508 lists saved configs that are dropped, mis-parsed or persisted out of range. Upstream design review #584 names this as the first candidate.

## Decision

One descriptor per (vendor, device) in AlpacaCore declares the fields once, in a typed config with no JSON: scalars, string lists (filter names) and nested configs for record lists (a switch's ports). Each field has a role (EnumerationIndex, DeviceId, PortPath, Host, Secret, Discriminator, Plain) and an optional applies_when for fields that only apply under one discriminator value, such as ToupTek's switch type.

The descriptor is two files in the vendor's directory. The schema half compiles in every build and includes no vendor header. The factory half compiles under the vendor's build option. So every build can list every vendor, with whether each one is available.

AlpacaHTTP bridges JSON to the typed config in one place and includes no vendor headers; the layering gate enforces zero now that the migration is done. It serves the schema at /management/v1/devicecatalog in the management envelope, as a compact custom shape pinned by a committed fixture.

API configs are rejected on validation failure; persisted configs are normalized with a warning, as the router did before the catalog (#380, PR #353). Sanitize keeps every declared non-secret field; only the UI honours applies_when. Aliases, such as iOptron's iCAM routed to the Player One camera driver, are explicit descriptors that delegate construction. Test descriptors register through the same catalog.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. Each (vendor, device) declares its config fields once, in a descriptor pair in the vendor directory, and the schema half includes no vendor header. Scope: `AlpacaCore/src/vendors/*/*_schema.cpp`, `AlpacaCore/src/vendors/*/*_catalog.cpp`. Checked by: `scripts/check_layering.py`.
2. AlpacaHTTP includes no vendor header and builds no driver outside the catalog. Scope: `AlpacaHTTP/src/**`, `AlpacaHTTP/include/**`. Checked by: `scripts/check_layering.py` (`MAX_ALPACAHTTP_VENDOR_INCLUDES` is 0).
3. AlpacaCore holds no JSON type; AlpacaHTTP bridges JSON to the typed config in one place. Scope: `AlpacaCore/src/**`, `AlpacaCore/include/**`. Checked by: review ([decision 0013](0013-designs-to-avoid.md), section 4).
4. A config from the API is rejected on validation failure; a persisted config is normalized with a warning and stays registered. Scope: `AlpacaCore/src/catalog/*.cpp`, `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp`.
5. Sanitize keeps every declared non-secret field and drops every `Secret` field. Scope: `AlpacaCore/src/catalog/*.cpp`, `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp`.

## Alternatives rejected

Catalog in AlpacaHTTP, with JSON there and a typed struct and factory per device in AlpacaCore: the field list would exist twice in code and once in the UI, and adding a device would still touch both libraries. Catalog in AlpacaCore using nlohmann::json directly: best locality, but `AGENTS.md` says AlpacaCore has no JSON transport code and nothing under AlpacaCore includes nlohmann today, so this would add a transport dependency for one consumer. A JSON Schema for the endpoint, and a generic form renderer in the UI: the form layout stays hand-written, and a drift check compares its field names and roles to the catalog. A separate vendor build manifest module: the schema/factory split already answers "known key, build option, compiled?"; generating CMake or packaging tables from it waits for a second consumer.

## Consequences

The router's registration and sanitize arms, and its vendor includes, went away one vendor at a time; every vendor has moved, `AlpacaHTTP/src/http/router.cpp` includes no vendor header, and the layering gate holds the count at zero (`MAX_ALPACAHTTP_VENDOR_INCLUDES` in `scripts/check_layering.py`). Every vendor's config round-trip cases exist before its descriptor lands, so a move that changes normalization shows as a diff. #508 inconsistencies are pinned as documented behaviour during the move and fixed after it, failing test first. Router tests that today call `registry.register_device` directly gain a test descriptor with a stub driver, which is a new seam. Adding a device becomes one descriptor pair beside its driver.

## Links

- Upstream design review [#584](https://github.com/open-astro/AlpacaBridge/issues/584) (candidate 1); issues [#508](https://github.com/open-astro/AlpacaBridge/issues/508), [#380](https://github.com/open-astro/AlpacaBridge/issues/380), [#577](https://github.com/open-astro/AlpacaBridge/issues/577), [#572](https://github.com/open-astro/AlpacaBridge/issues/572); PR [#353](https://github.com/open-astro/AlpacaBridge/pull/353).
- Owners of the rule before the migration (`INDEX_FIELDS` is gone): `AlpacaHTTP/src/http/router.cpp` (`register_device_from_config()`, `sanitize_device_config()`), `AlpacaHTTP/web/app.js` (`INDEX_FIELDS`, `updateVendorOptions`, `startEditDevice`).
- Module overview: [architecture](../architecture.md#modules).
