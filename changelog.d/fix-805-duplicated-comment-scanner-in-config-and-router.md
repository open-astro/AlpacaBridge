### Changed

- **Config comment scanner shared between loader and rewriter** (AlpacaHTTP): the quote and colon state machine that finds a YAML comment was copied in `config.cpp` and `router.cpp`; both now call `alpacahttp::util::strip_yaml_comment`, so a quoting fix lands in one place. No behaviour change.

### Added (tests)

- **`test_yaml_comment`** (AlpacaHTTP): calls the shared scanner directly for single and double quotes, `#` inside quotes, an apostrophe in a plain value and trailing comments.
