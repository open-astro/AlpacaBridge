### Fixed (tooling)
- **ConformU pin parsing** (`scripts/dependency_snapshot.py`): `parse_conformu` reports an error when `.claude/commands/conformu.md` pins more than one ConformU build in the `` `X.Y.Z.N, Build time:`` form, instead of reporting whichever comes first. Self-test samples now use the 4.5.2-rc.1 build.
- **Upstream document notes** (`scripts/dependency_updates.py`, `scripts/dependency_sdk_versions.json`): a `documents` entry may carry a `note` that is printed in the issue the weekly check opens. The ConformU beta entry's note says to adopt and mirror a new beta through `/conformu` before moving its baseline, and to keep its URL on the ASCOM beta download.

### Changed
- **ConformU version records** (`SUPPORTED-DRIVERS.md`, OnStep instructions): 4.5.2-rc.1 joins the list of ConformU versions used. The OnStep notes cite the TeSeek HM-17 4.5.2-rc.1 report as the arm64 validation.
