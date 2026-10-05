# ASan allocator incompatible with the Pi 4's 39-bit VA kernel

## Summary

On the Raspberry Pi 4 used for local pre-flight, the ASan+UBSan build aborts before Catch2 starts. The same startup failure occurs in a trivial instrumented program, so it is a host-kernel limitation rather than a test or project failure.

## Root Cause

The running Debian Raspberry Pi kernel was built with `CONFIG_ARM64_VA_BITS=39` and `CONFIG_ARM64_VA_BITS_39=y`. The ASan runtime's 64-bit allocator reserves fixed address ranges outside that 39-bit virtual-address space. Its `mmap` fails with `ENOMEM` (`-12`), then the allocator terminates at `sanitizer_allocator_primary64.h:131` while checking the expected address. The failure occurs during sanitizer initialization, before test discovery; ASLR settings do not make the out-of-range mappings available.

## Prevention

- Keep the default ASan+UBSan check enabled. Do not count this startup abort as a sanitizer pass or hide it by weakening tests.
- Run the sanitized suite on an AArch64 host with a compatible 48-bit virtual-address layout, or boot a kernel configured with `CONFIG_ARM64_VA_BITS_48=y` and `CONFIG_ARM64_VA_BITS=48`.
- If the check fails with the allocator initialization assertion on a 39-bit-VA Pi kernel, identify it as an environment limitation and use the 48-bit-VA host/CI result for sanitizer coverage until the kernel is changed.

## Evidence

- Raspberry Pi 4, Debian 13, kernel `6.18.34+rpt-rpi-v8`, GCC 14.2: `/boot/config-6.18.34+rpt-rpi-v8` reports `CONFIG_ARM64_VA_BITS_39=y` and `CONFIG_ARM64_VA_BITS=39`.
- Both GCC 14.2 and Clang 19.1.7 trivial `-fsanitize=address,undefined` programs fail with `AddressSanitizer: CHECK failed: sanitizer_allocator_primary64.h:131` at `0x500000000000`, returned error `0xfffffffffffffff4` (`-12`), with an empty stack.
- The repository's ASan+UBSan CI job passed on its native arm64 runner in PR [#842](https://github.com/open-astro/AlpacaBridge/pull/842), consistent with the failure being specific to the Pi kernel VA layout.
- Upstream reports describe the same failure and identify the 39-bit versus 48-bit kernel VA configuration: [LLVM #65144](https://github.com/llvm/llvm-project/issues/65144) and [Google Sanitizers #1674](https://github.com/google/sanitizers/issues/1674).
- Not verified: rebuilding and booting a 48-bit-VA Pi kernel, then rerunning the full local pre-flight.
