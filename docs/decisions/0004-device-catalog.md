# Device catalog

Status: accepted

## Context

What a device's configuration looks like is written in several places. `AlpacaHTTP/src/http/router.cpp` builds every driver in a chain of registration arms, one per (vendor, device), validates and normalizes their configs there, and strips secrets in a separate chain of sanitize arms. That makes the router include vendor headers from all 16 vendors (40 includes). `AlpacaHTTP/web/app.js` and `AlpacaHTTP/web/index.html` keep a second, hand-written copy of the field lists, the vendor availability blocks, the edit-population chain and `INDEX_FIELDS`. Adding a device, or a field, touches both libraries and the UI, and the copies drift: #508 lists saved configs that are dropped, mis-parsed or persisted out of range. Upstream design review #584 names this as the first candidate.

## Decision

One descriptor per (vendor, device) in AlpacaCore declares the fields once, in a typed config with no JSON: scalars, string lists (filter names) and nested configs for record lists (a switch's ports). Each field has a role (EnumerationIndex, DeviceId, PortPath, Host, Secret, Discriminator, Plain) and an optional applies_when for fields that only apply under one discriminator value, such as ToupTek's switch type.

The descriptor is two files in the vendor's directory. The schema half compiles in every build and includes no vendor header. The factory half compiles under the vendor's build option. So every build can list all 16 vendors, with whether each one is available.

AlpacaHTTP bridges JSON to the typed config in one place and includes no vendor headers; the layering gate enforces zero once the migration is done. It serves the schema at /management/v1/devicecatalog in the management envelope, as a compact custom shape pinned by a committed fixture.

API configs are rejected on validation failure; persisted configs are normalized with a warning, as `Router::reject_invalid_config()` does today (#380, PR #353). Sanitize keeps every declared non-secret field; only the UI honours applies_when. Aliases, such as iOptron's iCAM routed to the Player One camera driver, are explicit descriptors that delegate construction. Test descriptors register through the same catalog.

## Alternatives rejected

Catalog in AlpacaHTTP, with JSON there and a typed struct and factory per device in AlpacaCore: the field list would exist twice in code and once in the UI, and adding a device would still touch both libraries. Catalog in AlpacaCore using nlohmann::json directly: best locality, but `AlpacaCore/include/alpacacore/alpaca_json.h` states that AlpacaCore holds no JSON and nothing under AlpacaCore includes nlohmann today, so this would add a transport dependency for one consumer. A JSON Schema for the endpoint, and a generic form renderer in the UI: the form layout stays hand-written, and a drift check compares its field names and roles to the catalog. A separate vendor build manifest module: the schema/factory split already answers "known key, build option, compiled?"; generating CMake or packaging tables from it waits for a second consumer.

## Consequences

The router's registration and sanitize arms, and its vendor includes, go away one vendor at a time; until the last vendor moves, the router consults the catalog first and falls back to its arm chain. Every vendor's config round-trip cases exist before its descriptor lands, so a move that changes normalization shows as a diff. #508 inconsistencies are pinned as documented behaviour during the move and fixed after it, failing test first. Router tests that today call `registry.register_device` directly gain a test descriptor with a stub driver, which is a new seam. Adding a device becomes one descriptor pair beside its driver.

## Links

- Upstream design review [#584](https://github.com/open-astro/AlpacaBridge/issues/584) (candidate 1); issues [#508](https://github.com/open-astro/AlpacaBridge/issues/508), [#380](https://github.com/open-astro/AlpacaBridge/issues/380), [#577](https://github.com/open-astro/AlpacaBridge/issues/577), [#572](https://github.com/open-astro/AlpacaBridge/issues/572); PR [#353](https://github.com/open-astro/AlpacaBridge/pull/353).
- Current owners of the rule this replaces: `AlpacaHTTP/src/http/router.cpp` (`register_device_from_config()`, `reject_invalid_config()`, `normalize_persisted_connection_type()`, `sanitize_device_config()`), `AlpacaHTTP/web/app.js` (`INDEX_FIELDS`, `updateVendorOptions`, `startEditDevice`).
- Module overview: [architecture](../architecture.md#modules).
