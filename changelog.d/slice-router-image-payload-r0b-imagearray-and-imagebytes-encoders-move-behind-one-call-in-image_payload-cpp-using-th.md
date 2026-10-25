### Changed

- **Camera imagearray payload module**, AlpacaHTTP, issue #1628. The ImageBytes and JSON ImageArray encoders moved out of `router.cpp` into `AlpacaHTTP/src/http/image_payload.{h,cpp}` behind one call, `build(image, accept_header, call)`; the JSON envelope head comes from the new `DeviceCall::success_head()`. Responses are byte for byte unchanged; `AlpacaHTTP/tests/test_image_payload.cpp` asserts the exact JSON text, the ImageBytes header (both transaction ids) and the element-type packing, and `test_device_call.cpp` asserts the envelope head.
