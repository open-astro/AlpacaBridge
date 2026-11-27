### Fixed

- **README Authored code badge shows again on GitHub**, CI and docs, issue #2739. GitHub's image proxy returned 504 for the shields.io endpoint badge, so the README showed a broken image. `.github/workflows/authored-lines-badge.yml` now also saves the badge SVG (drawn by the shields.io static badge from the published count) as `authored-lines.svg` on the `badges` branch, and the README shows that file. `scripts/check_docs_drift.py --badge-svg-url` gives the static badge URL, and docs-drift check 25 holds the README line to the SVG.
