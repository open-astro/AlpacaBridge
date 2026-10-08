### Fixed

- **Gemini focuser and flat panel discard a late reply before the next command** (Gemini, no upstream issue): both wrappers now drop queued input before every command write, so an answer that arrived after its command's read timed out is no longer read as the next command's reply (wrong position, wrong cover state). Pinned by the "a late reply to the previous command is discarded" cases in `test_gemini_focuser.cpp` and `test_gemini_flatpanel_pro.cpp`.
