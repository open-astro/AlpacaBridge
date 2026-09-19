# Reference Version and Source Policy

## Pinned baseline

| Material | Version | Date | Governs |
| --- | --- | --- | --- |
| ASCOM Alpaca API Reference | V12 | Published 23 February 2026 | Shared HTTP/JSON rules, management, discovery, versioning, client behavior, ImageBytes |
| Alpaca Device API | v1, OpenAPI 3.1.1 | Official schema verified 18 September 2026 | Device paths, verbs, parameters, response schemas, Platform 7 additions |
| ASCOM Master Interface Definitions | Platform 7 generation | Verified 18 September 2026 | Member semantics, units, ranges, capabilities, state, errors, interface versions |
| Alpaca Management API | v1 | Verified 18 September 2026 | Management response schemas |
| Alpaca Discovery | Protocol v1 | API Reference V12 | UDP discovery messages and responses |

The LF-normalized Device API schema snapshot used to produce the endpoint catalog has SHA-256:

`07119ad5df107c791b2403de42c62f3770894ae7d283dfaff41b8dc03bd49ec5`

The hash identifies the snapshot, not a permanently current standard. Recheck it after the verification date.

## Official sources

- Architecture and motivation: <https://ascom-standards.org/About/Index.htm>
- Driver principles: <https://ascom-standards.org/AlpacaDeveloper/Principles.htm>
- Asynchronous operations: <https://ascom-standards.org/AlpacaDeveloper/Async.htm>
- Exception handling: <https://ascom-standards.org/AlpacaDeveloper/Exceptions.htm>
- API Reference V12: <https://ascom-standards.org/AlpacaDeveloper/ASCOMAlpacaAPIReference.html>
- Device and Management API definitions: <https://ascom-standards.org/api/>
- Platform-neutral Master Interfaces: <https://ascom-standards.org/newdocs/>
- Developer documentation index: <https://ascom-standards.org/Documentation/Index.htm#dev>

## Updating the baseline

When targeting a newer release:

1. Record its version, publication date, and retrieval date.
2. Diff the Device and Management schemas for paths, verbs, parameters, response types, and errors.
3. Review Master Interface release notes for behavioral changes invisible in OpenAPI.
4. Update every affected reference and regenerate or re-check the endpoint catalog.
5. Revalidate affected drivers and obtain new real-hardware ConformU evidence when behavior or `InterfaceVersion` changes.

Never call the skill “current” without a dated verification. Do not silently combine endpoint definitions from one revision with behavioral requirements from an incompatible Master Interface revision.

## Resolving apparent conflicts

- Master Interfaces govern what a member means.
- OpenAPI governs its exact wire schema.
- The API Reference governs shared mechanics.
- A later official erratum or clearly later dated definition may supersede older prose; document that decision.
- Emit canonical names and exact schemas even when tolerant parsing is allowed.
- Accept request variations only when doing so is unambiguous and does not hide an invalid required value.

One known wording difference concerns form-parameter casing: API Reference V12 tells clients to send canonical casing, while the verified Device API v1 introduction says parameter names are case-insensitive. For interoperability, clients emit canonical casing, devices accept parameter-name casing variations, and devices always emit canonical JSON key casing.

