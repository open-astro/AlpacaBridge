#!/usr/bin/env python3
# Copyright (c) 2025-2026 Joey Troy and contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Manual raw-QHY USB recovery experiment; never imported by the daemon.

Run on a Linux arm64 rig only, after stopping all SDK consumers. JSON records
go to stdout (redirect to a new file); interactive prompts go to stderr.
Hardware-free check:
python3 -m unittest discover -s scripts/tests -p 'test_qhy_usb_recovery_probe.py'
"""

import argparse
import ctypes
import datetime
import json
import math
import os
import platform
from pathlib import Path
import sys
import time


WARNING = """WARNING: Stop AlpacaBridge and EVERY other QHY SDK consumer first,
including guiding software, camera tools, and other probes. This probe cannot
detect other processes or their handles. Release/init affects SDK-global state.
No exposures, wheel commands, guiding, or camera initialization are requested.
Wheel mode sets stream mode 0 and reads CFW metadata/status, but sends no move.
Vendor open/init code may itself affect hardware. SDK calls may hang or crash;
there is no safe in-process timeout. Ctrl-C cleanup is best-effort only.
"""
COMMANDS = """Commands:
  scan [N]       raw enumerate N times (default 3, maximum 100)
  unplug         prompt for physical unplug, then scan 3 times
  replug         prompt for physical replug/firmware settling, then scan 3 times
  reset          explicit ReleaseQHYCCDResource + InitQHYCCDResource (no handles)
  hold ID        open one handle and leave it open during scans/unplug/replug
  close          explicitly close the held handle (no automatic close/retry)
  cycle ID [N]   open/close the exact same camera ID N times (default 3, max 100)
  quit           release resources and exit
No reset is automatic. Use scan after reset to compare with the pre-reset IDs.
Close the held handle explicitly before reset or quit; quit while held fails.
With --wheel, scans include model lookup and opens perform the driver's CFW setup.
"""

CFWPORT = 17
CFWSLOTSNUM = 44


def thread_count():
    try:
        return len(list(Path("/proc/self/task").iterdir()))
    except OSError:
        return None


def emit(event, **fields):
    print(
        json.dumps(
            {
                "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "event": event,
                **fields,
            }
        ),
        flush=True,
    )


def ask(prompt):
    print(prompt, end="", file=sys.stderr, flush=True)
    line = sys.stdin.readline()
    if not line:
        raise EOFError
    return line.strip()


def load_sdk(library):
    """Only main's acknowledged, arm64-only path may call this function."""
    sdk = ctypes.CDLL(library)
    for name in ("InitQHYCCDResource", "ReleaseQHYCCDResource", "ScanQHYCCD"):
        function = getattr(sdk, name)
        function.argtypes = []
        function.restype = ctypes.c_uint32
    sdk.GetQHYCCDId.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_char)]
    sdk.GetQHYCCDId.restype = ctypes.c_uint32
    sdk.OpenQHYCCD.argtypes = [ctypes.c_char_p]
    sdk.OpenQHYCCD.restype = ctypes.c_void_p
    sdk.CloseQHYCCD.argtypes = [ctypes.c_void_p]
    sdk.CloseQHYCCD.restype = ctypes.c_uint32
    for name in ("EnableQHYCCDMessage", "EnableQHYCCDLogFile"):
        function = getattr(sdk, name)
        function.argtypes = [ctypes.c_bool]
        function.restype = None
    sdk.GetQHYCCDSDKVersion.argtypes = [ctypes.POINTER(ctypes.c_uint32)] * 4
    sdk.GetQHYCCDSDKVersion.restype = ctypes.c_uint32
    sdk.GetQHYCCDModel.argtypes = [
        ctypes.POINTER(ctypes.c_char),
        ctypes.POINTER(ctypes.c_char),
    ]
    sdk.GetQHYCCDModel.restype = ctypes.c_uint32
    sdk.SetQHYCCDStreamMode.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
    sdk.SetQHYCCDStreamMode.restype = ctypes.c_uint32
    sdk.IsQHYCCDControlAvailable.argtypes = [ctypes.c_void_p, ctypes.c_int]
    sdk.IsQHYCCDControlAvailable.restype = ctypes.c_uint32
    sdk.GetQHYCCDParam.argtypes = [ctypes.c_void_p, ctypes.c_int]
    sdk.GetQHYCCDParam.restype = ctypes.c_double
    sdk.GetQHYCCDCFWStatus.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char)]
    sdk.GetQHYCCDCFWStatus.restype = ctypes.c_uint32
    return sdk


class Probe:
    def __init__(self, sdk, wheel=False):
        self.sdk = sdk
        self.wheel = wheel
        self.initialized = False
        self.handles = set()
        self.open_pending = False
        self.close_pending = False

    def call(self, name, *args, **context):
        before = thread_count()
        emit("call_start", call=name, threads_before=before, **context)
        start = time.monotonic()
        try:
            raw = getattr(self.sdk, name)(*args)
        except BaseException as error:
            emit(
                "call_exception",
                call=name,
                error=repr(error),
                elapsed_s=time.monotonic() - start,
                threads_after=thread_count(),
            )
            raise
        if isinstance(raw, float):
            result = {
                "raw": raw if math.isfinite(raw) else None,
                "raw_hex": raw.hex(),
                "raw_repr": repr(raw),
            }
        else:
            result = {"raw": raw, "raw_hex": None if raw is None else hex(raw)}
        emit(
            "call_result",
            call=name,
            **result,
            elapsed_s=time.monotonic() - start,
            threads_before=before,
            threads_after=thread_count(),
            **context
        )
        return raw

    def init(self):
        if self.wheel:
            self.call("EnableQHYCCDMessage", False)
            self.call("EnableQHYCCDLogFile", False)
        if self.call("InitQHYCCDResource") != 0:
            raise RuntimeError("SDK initialization failed; stopping (no scan/open).")
        self.initialized = True
        if self.wheel:
            parts = [ctypes.c_uint32() for _ in range(4)]
            status = self.call(
                "GetQHYCCDSDKVersion", *(ctypes.byref(part) for part in parts)
            )
            emit("sdk_version", status=status, parts=[part.value for part in parts])

    def release(self):
        if self.handles or self.open_pending or self.close_pending:
            raise RuntimeError(
                "Release/reset refused: probe-owned handle close unconfirmed."
            )
        if self.initialized:
            # A failed release leaves global state unknown: never retry or re-init.
            self.initialized = False
            if self.call("ReleaseQHYCCDResource") != 0:
                raise RuntimeError("SDK release failed; stopping without re-init.")

    def scan(self, count):
        for sample in range(count):
            total = self.call("ScanQHYCCD", sample=sample)
            if not 0 <= total <= 64:
                emit("scan_error", raw=total, sample=sample)
                continue
            ids = []
            failures = 0
            for index in range(total):
                buffer = ctypes.create_string_buffer(64)
                status = self.call("GetQHYCCDId", index, buffer, index=index)
                raw_id = buffer.raw.split(b"\0", 1)[0]
                if status != 0:
                    failures += 1
                    emit(
                        "id_lookup_error",
                        index=index,
                        raw=status,
                        id_bytes_hex=raw_id.hex(),
                    )
                elif not raw_id or b"\0" not in buffer.raw:
                    failures += 1
                    emit("invalid_id", index=index, id_bytes_hex=raw_id.hex())
                else:
                    ids.append(raw_id.decode("ascii", errors="backslashreplace"))
                    emit(
                        "camera_id", index=index, id=ids[-1], id_bytes_hex=raw_id.hex()
                    )
                    if self.wheel:
                        self.model(raw_id, index=index)
            emit("enumeration", sample=sample, count=total, ids=ids, id_errors=failures)
            if sample + 1 < count:
                time.sleep(1)

    def open(self, camera_id, **context):
        if self.handles or self.open_pending or self.close_pending:
            raise RuntimeError(
                "Open refused: a probe-owned handle is held or unconfirmed."
            )
        encoded = camera_id.encode("ascii")
        if not 1 <= len(encoded) <= 63 or b"\0" in encoded:
            raise ValueError("ID must be 1..63 ASCII bytes, without NUL.")
        # Logging or interruption can lose the handle before ownership is recorded.
        self.open_pending = True
        handle = self.call("OpenQHYCCD", encoded, id=camera_id, **context)
        if not handle:
            self.open_pending = False
            emit("open_error", id=camera_id, **context)
            return None
        self.handles.add(handle)
        self.open_pending = False
        if self.wheel:
            try:
                self.setup_wheel(handle, encoded)
            except BaseException:
                # Mirror connect rollback, but require a confirmed close.
                self.close(id=camera_id)
                raise
        return handle

    def model(self, camera_id, **context):
        encoded = ctypes.create_string_buffer(camera_id, 64)
        buffer = ctypes.create_string_buffer(64)
        status = self.call("GetQHYCCDModel", encoded, buffer, **context)
        raw_model = buffer.raw.split(b"\0", 1)[0]
        emit(
            "camera_model",
            status=status,
            model_bytes_hex=buffer.raw.hex(),
            model=raw_model.decode("ascii", errors="backslashreplace"),
            **context
        )

    def setup_wheel(self, handle, camera_id):
        if self.call("SetQHYCCDStreamMode", handle, 0, mode=0) != 0:
            raise RuntimeError("Failed to set QHY stream mode 0.")
        if self.call("IsQHYCCDControlAvailable", handle, CFWPORT, control=CFWPORT) != 0:
            raise RuntimeError("No CFW detected on this QHY camera.")
        slots = self.call("GetQHYCCDParam", handle, CFWSLOTSNUM, control=CFWSLOTSNUM)
        if (
            not math.isfinite(slots)
            or not -(2**31) <= slots <= 2**31 - 1
            or int(slots) <= 0
        ):
            raise RuntimeError("QHY CFW reported an invalid slot count.")
        self.model(camera_id)
        buffer = ctypes.create_string_buffer(64)
        status = self.call("GetQHYCCDCFWStatus", handle, buffer)
        first = buffer.raw[0]
        position = (
            first - ord("0") if status == 0 and ord("0") <= first <= ord("9") else -1
        )
        emit(
            "wheel_ready",
            slots=int(slots),
            position=position,
            status=status,
            status_bytes_hex=buffer.raw.hex(),
        )

    def close(self, **context):
        if self.open_pending or self.close_pending or not self.handles:
            raise RuntimeError(
                "Close refused: no confirmed handle or prior close unconfirmed."
            )
        handle = next(iter(self.handles))
        self.close_pending = True
        status = self.call("CloseQHYCCD", handle, **context)
        if status != 0:
            raise RuntimeError("Close failed; stopping and refusing SDK release/reset.")
        self.handles.remove(handle)
        self.close_pending = False

    def cycle(self, camera_id, count):
        for sample in range(count):
            if self.open(camera_id, sample=sample):
                self.close(id=camera_id, sample=sample)


def repetitions(text="3"):
    count = int(text)
    if not 1 <= count <= 100:
        raise ValueError("Repeat count must be 1..100.")
    return count


def run(probe):
    try:
        probe.init()
        print(COMMANDS, file=sys.stderr)
        while True:
            words = ask("qhy> ").split()
            if not words:
                continue
            command, *args = words
            emit("command", command=command, args=args)
            if command == "quit" and not args:
                break
            if command in ("reset", "hold", "cycle") and (
                probe.handles or probe.open_pending
            ):
                print(
                    "Command refused: close the held handle first; "
                    "no second open/reset.",
                    file=sys.stderr,
                )
                continue
            if command == "reset" and not args:
                probe.release()
                probe.init()
            elif command == "close" and not args:
                if probe.handles:
                    probe.close()
                else:
                    print("No held handle to close.", file=sys.stderr)
            elif command in ("unplug", "replug") and not args:
                action = (
                    "Unplug the camera USB cable now"
                    if command == "unplug"
                    else "Replug the SAME camera; "
                    "wait for firmware/USB enumeration to settle"
                )
                if (
                    ask(action + "; type DONE to scan (anything else cancels): ")
                    == "DONE"
                ):
                    emit("physical_step_acknowledged", action=command)
                    probe.scan(3)
            elif command in ("scan", "cycle", "hold"):
                try:
                    if command == "scan" and len(args) <= 1:
                        count = repetitions(*args)
                    elif (command == "cycle" and 1 <= len(args) <= 2) or (
                        command == "hold" and len(args) == 1
                    ):
                        count = repetitions(*args[1:])
                        args[0].encode("ascii")
                        if not 1 <= len(args[0]) <= 63 or "\0" in args[0]:
                            raise ValueError(
                                "ID must be 1..63 ASCII bytes, without NUL."
                            )
                    else:
                        raise ValueError("Wrong command arguments.")
                except ValueError as error:
                    print(str(error), file=sys.stderr)
                    continue
                if command == "scan":
                    probe.scan(count)
                elif command == "hold":
                    probe.open(args[0])
                else:
                    probe.cycle(args[0], count)
            else:
                print(COMMANDS, file=sys.stderr)
    finally:
        probe.release()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=WARNING + "\n" + COMMANDS,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--library", default="libqhyccd.so", help="SDK shared library path"
    )
    parser.add_argument(
        "--wheel",
        action="store_true",
        help="mirror integrated wheel SDK setup (no movement)",
    )
    options = parser.parse_args(argv)
    print(WARNING, file=sys.stderr)
    try:
        if platform.system() != "Linux" or platform.machine() not in (
            "aarch64",
            "arm64",
        ):
            raise RuntimeError(
                "Real SDK probe requires Linux arm64; help/tests load no SDK."
            )
        if (
            ask(
                "Type STOPPED to confirm ALL SDK consumers are stopped before loading: "
            )
            != "STOPPED"
        ):
            raise RuntimeError("Acknowledgement refused; SDK not loaded.")
        emit(
            "session",
            library=options.library,
            pid=os.getpid(),
            machine=platform.machine(),
            threads=thread_count(),
            wheel=options.wheel,
            uid=os.getuid(),
            euid=os.geteuid(),
            gid=os.getgid(),
            egid=os.getegid(),
            groups=os.getgroups(),
            cwd=os.getcwd(),
            home=os.environ.get("HOME"),
            ld_library_path=os.environ.get("LD_LIBRARY_PATH"),
        )
        run(Probe(load_sdk(options.library), wheel=options.wheel))
    except EOFError:
        emit("input_closed")
        return 0
    except KeyboardInterrupt:
        emit("interrupted")
        return 130
    except (OSError, RuntimeError, ValueError, AttributeError) as error:
        emit("fatal", error=str(error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
