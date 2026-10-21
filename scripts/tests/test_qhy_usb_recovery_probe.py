# Copyright (c) 2025-2026 Joey Troy and contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Hardware-free control-path checks; never load the vendor library."""

import contextlib
import ctypes
import importlib.util
import io
import json
from pathlib import Path
import runpy
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "qhy_usb_recovery_probe.py"
with patch("ctypes.CDLL", side_effect=AssertionError("must not load SDK on import")):
    spec = importlib.util.spec_from_file_location("qhy_probe", SCRIPT)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)


class FakeSDK:
    # Models statuses and ownership only, not QHY hotplug, threads, or USB recovery.
    def __init__(self):
        self.calls = []
        self.scan_results = iter([1] * 100)
        self.init_results = iter([0] * 100)
        self.id_status = 0
        self.id_value = b"QHY-test"
        self.close_status = 0
        self.release_status = 0
        self.open_result = 1234
        self.id_exception = None
        self.version_status = 0
        self.model_status = 0
        self.model_value = b"miniCam8M\0raw"
        self.stream_status = 0
        self.available_status = 0
        self.slots = 7.0
        self.status_status = 0
        self.status_value = b"3\0raw"

    def EnableQHYCCDMessage(self, enabled):
        self.calls.append(("message", enabled))

    def EnableQHYCCDLogFile(self, enabled):
        self.calls.append(("logfile", enabled))

    def GetQHYCCDSDKVersion(self, *parts):
        self.calls.append(("version",))
        for pointer, value in zip(parts, (26, 7, 28, 0xFFFFFFFF)):
            ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint32))[0] = value
        return self.version_status

    def GetQHYCCDModel(self, camera_id, buffer):
        self.calls.append(("model", camera_id.value))
        buffer.raw = self.model_value.ljust(64, b"\0")
        return self.model_status

    def SetQHYCCDStreamMode(self, handle, mode):
        self.calls.append(("stream", handle, mode))
        return self.stream_status

    def IsQHYCCDControlAvailable(self, handle, control):
        self.calls.append(("available", handle, control))
        return self.available_status

    def GetQHYCCDParam(self, handle, control):
        self.calls.append(("param", handle, control))
        return self.slots

    def GetQHYCCDCFWStatus(self, handle, buffer):
        self.calls.append(("status", handle))
        buffer.raw = self.status_value.ljust(64, b"\0")
        return self.status_status

    def InitQHYCCDResource(self):
        self.calls.append(("init",))
        return next(self.init_results)

    def ReleaseQHYCCDResource(self):
        self.calls.append(("release",))
        return self.release_status

    def ScanQHYCCD(self):
        self.calls.append(("scan",))
        return next(self.scan_results)

    def GetQHYCCDId(self, index, buffer):
        self.calls.append(("id", index))
        if self.id_exception:
            raise self.id_exception
        buffer.value = self.id_value
        return self.id_status

    def OpenQHYCCD(self, camera_id):
        self.calls.append(("open", camera_id))
        return self.open_result

    def CloseQHYCCD(self, handle):
        self.calls.append(("close", handle))
        return self.close_status


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.sdk = FakeSDK()
        self.output = io.StringIO()
        self.errors = io.StringIO()
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(contextlib.redirect_stdout(self.output))
        self.stack.enter_context(contextlib.redirect_stderr(self.errors))
        self.stack.enter_context(
            patch("ctypes.CDLL", side_effect=AssertionError("unexpected dlopen"))
        )
        self.stack.enter_context(patch.object(probe.time, "sleep"))
        self.stack.enter_context(
            patch.object(probe.platform, "system", return_value="Linux")
        )
        self.stack.enter_context(
            patch.object(probe.platform, "machine", return_value="aarch64")
        )
        self.loader = self.stack.enter_context(
            patch.object(probe, "load_sdk", return_value=self.sdk)
        )

    def run_commands(self, *commands, argv=None):
        with patch.object(probe, "ask", side_effect=["STOPPED", *commands]):
            return probe.main([] if argv is None else argv)

    def events(self, event):
        return [
            item
            for line in self.output.getvalue().splitlines()
            if (item := json.loads(line))["event"] == event
        ]

    def test_import_help_and_refusal_never_load_sdk(self):
        with patch("ctypes.CDLL", side_effect=AssertionError("unexpected dlopen")):
            runpy.run_path(str(SCRIPT), run_name="import_check")
            with self.assertRaises(SystemExit) as result:
                probe.main(["--help"])
            self.assertEqual(result.exception.code, 0)
        with patch.object(probe, "ask", return_value="no"):
            self.assertEqual(probe.main([]), 1)
        self.loader.assert_not_called()
        self.assertFalse(self.sdk.calls)

    def test_wrong_host_never_loads_sdk(self):
        with patch.object(probe.platform, "machine", return_value="x86_64"):
            self.assertEqual(probe.main([]), 1)
        self.loader.assert_not_called()

    def test_repeated_scan_preserves_raw_errors_and_lookup_failures(self):
        self.sdk.scan_results = iter([0, 0xFFFFFFFF, 65, 1])
        self.sdk.id_status = 42
        self.assertEqual(self.run_commands("scan 4", "quit"), 0)
        self.assertEqual(
            [event["raw"] for event in self.events("scan_error")], [0xFFFFFFFF, 65]
        )
        self.assertEqual(self.events("id_lookup_error")[0]["raw"], 42)
        self.assertEqual(self.events("enumeration")[-1]["id_errors"], 1)
        self.assertEqual(self.sdk.calls.count(("id", 0)), 1)
        self.assertEqual(self.sdk.calls[-1], ("release",))
        for event in self.events("call_result"):
            self.assertGreaterEqual(event["elapsed_s"], 0)
            self.assertIn("threads_before", event)
            self.assertIn("threads_after", event)

    def test_explicit_physical_steps_reset_and_same_id_cycles(self):
        self.assertEqual(
            self.run_commands(
                "scan 1",
                "unplug",
                "DONE",
                "replug",
                "DONE",
                "reset",
                "cycle QHY-test 2",
                "quit",
            ),
            0,
        )
        self.assertEqual(self.sdk.calls.count(("scan",)), 7)
        self.assertEqual(self.sdk.calls.count(("init",)), 2)
        self.assertEqual(self.sdk.calls.count(("release",)), 2)
        self.assertEqual(
            self.sdk.calls[-5:],
            [
                ("open", b"QHY-test"),
                ("close", 1234),
                ("open", b"QHY-test"),
                ("close", 1234),
                ("release",),
            ],
        )
        self.assertEqual(self.events("enumeration")[0]["ids"], ["QHY-test"])
        self.assertEqual(
            [event["action"] for event in self.events("physical_step_acknowledged")],
            ["unplug", "replug"],
        )

    def test_physical_steps_require_separate_acknowledgement(self):
        self.assertEqual(self.run_commands("unplug", "no", "replug", "no", "quit"), 0)
        self.assertEqual(self.sdk.calls, [("init",), ("release",)])

    def test_init_failure_stops_before_scan_or_open(self):
        self.sdk.init_results = iter([17])
        self.assertEqual(self.run_commands("scan", "quit"), 1)
        self.assertEqual(self.sdk.calls, [("init",)])

    def test_reset_failures_never_continue(self):
        self.sdk.release_status = 18
        self.assertEqual(self.run_commands("reset", "scan", "quit"), 1)
        self.assertEqual(self.sdk.calls, [("init",), ("release",)])
        self.sdk = FakeSDK()
        self.loader.return_value = self.sdk
        self.sdk.init_results = iter([0, 19])
        self.assertEqual(self.run_commands("reset", "scan", "quit"), 1)
        self.assertEqual(self.sdk.calls, [("init",), ("release",), ("init",)])

    def test_unconfirmed_close_blocks_release_and_reset(self):
        self.sdk.close_status = 20
        self.assertEqual(self.run_commands("cycle QHY-test 2", "reset", "quit"), 1)
        self.assertEqual(
            self.sdk.calls, [("init",), ("open", b"QHY-test"), ("close", 1234)]
        )
        owned = probe.Probe(self.sdk)
        owned.initialized = True
        owned.handles.add(1234)
        with self.assertRaisesRegex(RuntimeError, "close unconfirmed"):
            owned.release()
        self.assertNotIn(("release",), self.sdk.calls)

    def test_unconfirmed_open_blocks_release_on_interrupt_or_logging_failure(self):
        original_emit = probe.emit
        for stage in ("sdk", "result_log"):
            for error in (KeyboardInterrupt(), OSError("fake log failure")):
                with self.subTest(stage=stage, error=type(error).__name__):
                    self.sdk.calls.clear()

                    def open_camera(camera_id):
                        self.sdk.calls.append(("open", camera_id))
                        if stage == "sdk":
                            raise error
                        return 1234

                    def log(event, **fields):
                        if (
                            stage == "result_log"
                            and event == "call_result"
                            and fields.get("call") == "OpenQHYCCD"
                        ):
                            raise error
                        original_emit(event, **fields)

                    with patch.object(
                        self.sdk, "OpenQHYCCD", side_effect=open_camera
                    ), patch.object(probe, "emit", side_effect=log):
                        self.assertEqual(self.run_commands("cycle QHY-test 1"), 1)
                    self.assertEqual(self.sdk.calls, [("init",), ("open", b"QHY-test")])
                    self.assertNotIn(("release",), self.sdk.calls)

    def test_null_open_is_logged_without_close(self):
        self.sdk.open_result = None
        self.assertEqual(self.run_commands("cycle QHY-test 2", "quit"), 0)
        self.assertEqual(len(self.events("open_error")), 2)
        self.assertNotIn(("close", 1234), self.sdk.calls)
        self.assertEqual(self.sdk.calls[-1], ("release",))

    def test_hold_scan_close_reset(self):
        self.assertEqual(
            self.run_commands("hold QHY-test", "scan 1", "close", "reset", "quit"), 0
        )
        self.assertEqual(
            self.sdk.calls,
            [
                ("init",),
                ("open", b"QHY-test"),
                ("scan",),
                ("id", 0),
                ("close", 1234),
                ("release",),
                ("init",),
                ("release",),
            ],
        )

    def test_held_handle_refuses_reset_second_hold_and_cycle_without_exiting(self):
        self.assertEqual(
            self.run_commands(
                "hold QHY-test",
                "reset",
                "hold another-ID",
                "cycle another-ID 2",
                "scan 1",
                "close",
                "quit",
            ),
            0,
        )
        self.assertEqual(
            self.sdk.calls,
            [
                ("init",),
                ("open", b"QHY-test"),
                ("scan",),
                ("id", 0),
                ("close", 1234),
                ("release",),
            ],
        )
        self.assertEqual(self.errors.getvalue().count("Command refused:"), 3)
        for pending in (False, True):
            owned = probe.Probe(self.sdk)
            owned.open_pending = pending
            if not pending:
                owned.handles.add(1234)
            with self.assertRaisesRegex(RuntimeError, "Open refused"):
                owned.open("another-ID")

    def test_held_close_failure_or_uncertainty_never_retries_or_releases(self):
        original_emit = probe.emit
        for stage in ("status", "sdk", "result_log"):
            with self.subTest(stage=stage):
                self.sdk.calls.clear()

                def close_camera(handle):
                    self.sdk.calls.append(("close", handle))
                    if stage == "sdk":
                        raise KeyboardInterrupt
                    return 42 if stage == "status" else 0

                def log(event, **fields):
                    if (
                        stage == "result_log"
                        and event == "call_result"
                        and fields.get("call") == "CloseQHYCCD"
                    ):
                        raise KeyboardInterrupt
                    original_emit(event, **fields)

                with patch.object(
                    self.sdk, "CloseQHYCCD", side_effect=close_camera
                ), patch.object(probe, "emit", side_effect=log):
                    self.assertEqual(
                        self.run_commands("hold QHY-test", "close", "close", "reset"), 1
                    )
                self.assertEqual(
                    self.sdk.calls, [("init",), ("open", b"QHY-test"), ("close", 1234)]
                )

    def test_quit_with_held_handle_refuses_release(self):
        self.assertEqual(self.run_commands("hold QHY-test", "quit"), 1)
        self.assertEqual(self.sdk.calls, [("init",), ("open", b"QHY-test")])

    def test_bad_counts_and_ids_never_reach_sdk(self):
        self.assertEqual(
            self.run_commands(
                "scan 0",
                "scan 101",
                "scan nope",
                "cycle ID 0",
                "cycle ID 101",
                "cycle ID nope",
                "cycle é",
                "cycle " + "x" * 64,
                "scan 1 2",
                "quit",
            ),
            0,
        )
        self.assertEqual(self.sdk.calls, [("init",), ("release",)])

    def test_exception_eof_and_interrupt_release_initialized_resource(self):
        for error, expected in [
            (OSError("fake failure"), 1),
            (EOFError(), 0),
            (KeyboardInterrupt(), 130),
        ]:
            with self.subTest(error=type(error).__name__):
                self.sdk.calls.clear()
                self.sdk.id_exception = error
                self.assertEqual(self.run_commands("scan 1"), expected)
                self.assertEqual(self.sdk.calls[-1], ("release",))
        self.sdk.id_exception = None
        with patch.object(probe, "ask", side_effect=["STOPPED", EOFError()]):
            self.assertEqual(probe.main([]), 0)
        self.assertEqual(self.sdk.calls[-1], ("release",))

    def test_invalid_lookup_id_is_not_reported_as_camera(self):
        for value in (b"", b"x" * 64):
            self.sdk.id_value = value
            self.assertEqual(self.run_commands("scan 1", "quit"), 0)
        self.assertEqual(len(self.events("invalid_id")), 2)
        self.assertFalse(self.events("camera_id"))

    def test_ctypes_binding_uses_pointer_and_unsigned_status_types(self):
        class Function:
            pass

        class Library:
            pass

        library = Library()
        for name in (
            "InitQHYCCDResource",
            "ReleaseQHYCCDResource",
            "ScanQHYCCD",
            "GetQHYCCDId",
            "OpenQHYCCD",
            "CloseQHYCCD",
            "EnableQHYCCDMessage",
            "EnableQHYCCDLogFile",
            "GetQHYCCDSDKVersion",
            "GetQHYCCDModel",
            "SetQHYCCDStreamMode",
            "IsQHYCCDControlAvailable",
            "GetQHYCCDParam",
            "GetQHYCCDCFWStatus",
        ):
            setattr(library, name, Function())
        # Exercise the actual loader while replacing dlopen, never a real SDK.
        # Use a fresh import to get the real function without changing main's seam.
        namespace = runpy.run_path(str(SCRIPT), run_name="binding_check")
        with patch("ctypes.CDLL", return_value=library) as dlopen:
            result = namespace["load_sdk"]("fake.so")
        dlopen.assert_called_once_with("fake.so")
        self.assertIs(result, library)
        self.assertIs(library.OpenQHYCCD.restype, ctypes.c_void_p)
        self.assertEqual(library.CloseQHYCCD.argtypes, [ctypes.c_void_p])
        self.assertIs(library.ScanQHYCCD.restype, ctypes.c_uint32)
        self.assertEqual(
            library.GetQHYCCDId.argtypes,
            [ctypes.c_uint32, ctypes.POINTER(ctypes.c_char)],
        )
        for name in ("EnableQHYCCDMessage", "EnableQHYCCDLogFile"):
            self.assertEqual(getattr(library, name).argtypes, [ctypes.c_bool])
            self.assertIsNone(getattr(library, name).restype)
        self.assertEqual(
            library.GetQHYCCDSDKVersion.argtypes, [ctypes.POINTER(ctypes.c_uint32)] * 4
        )
        self.assertEqual(
            library.GetQHYCCDModel.argtypes, [ctypes.POINTER(ctypes.c_char)] * 2
        )
        self.assertEqual(
            library.SetQHYCCDStreamMode.argtypes, [ctypes.c_void_p, ctypes.c_uint8]
        )
        for name in ("IsQHYCCDControlAvailable", "GetQHYCCDParam"):
            self.assertEqual(
                getattr(library, name).argtypes, [ctypes.c_void_p, ctypes.c_int]
            )
        self.assertIs(library.GetQHYCCDParam.restype, ctypes.c_double)
        self.assertEqual(
            library.GetQHYCCDCFWStatus.argtypes,
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char)],
        )
        for name in (
            "InitQHYCCDResource",
            "ReleaseQHYCCDResource",
            "GetQHYCCDId",
            "CloseQHYCCD",
            "GetQHYCCDSDKVersion",
            "GetQHYCCDModel",
            "SetQHYCCDStreamMode",
            "IsQHYCCDControlAvailable",
            "GetQHYCCDCFWStatus",
        ):
            self.assertIs(getattr(library, name).restype, ctypes.c_uint32)

    def test_wheel_scan_hold_and_cycle_match_driver_order_without_init_or_move(self):
        self.assertEqual(
            self.run_commands(
                "scan 1",
                "hold QHY-test",
                "scan 1",
                "close",
                "cycle QHY-test 1",
                "quit",
                argv=["--wheel"],
            ),
            0,
        )
        setup = [
            ("open", b"QHY-test"),
            ("stream", 1234, 0),
            ("available", 1234, 17),
            ("param", 1234, 44),
            ("model", b"QHY-test"),
            ("status", 1234),
        ]
        scan = [("scan",), ("id", 0), ("model", b"QHY-test")]
        self.assertEqual(
            self.sdk.calls,
            [
                ("message", False),
                ("logfile", False),
                ("init",),
                ("version",),
                *scan,
                *setup,
                *scan,
                ("close", 1234),
                *setup,
                ("close", 1234),
                ("release",),
            ],
        )
        self.assertEqual(self.events("sdk_version")[0]["status"], 0)
        self.assertEqual(
            self.events("sdk_version")[0]["parts"], [26, 7, 28, 0xFFFFFFFF]
        )
        model = self.events("camera_model")[0]
        self.assertEqual(model["model"], "miniCam8M")
        self.assertEqual(
            model["model_bytes_hex"], self.sdk.model_value.ljust(64, b"\0").hex()
        )
        ready = self.events("wheel_ready")[0]
        self.assertEqual(
            (ready["slots"], ready["position"], ready["status"]), (7, 3, 0)
        )
        self.assertEqual(
            ready["status_bytes_hex"], self.sdk.status_value.ljust(64, b"\0").hex()
        )
        void = next(
            event
            for event in self.events("call_result")
            if event["call"] == "EnableQHYCCDMessage"
        )
        self.assertIsNone(void["raw"])
        self.assertIsNone(void["raw_hex"])

    def test_wheel_success_retains_ownership_until_explicit_close(self):
        owned = probe.Probe(self.sdk, wheel=True)
        owned.init()
        self.assertEqual(owned.open("QHY-test"), 1234)
        self.assertEqual(owned.handles, {1234})
        self.assertFalse(owned.open_pending)
        with self.assertRaisesRegex(RuntimeError, "close unconfirmed"):
            owned.release()
        self.assertNotIn(("close", 1234), self.sdk.calls)
        owned.close()
        self.assertEqual(owned.handles, set())
        owned.release()
        self.assertEqual(self.sdk.calls[-2:], [("close", 1234), ("release",)])

    def test_wheel_startup_init_failure_stops_before_version_scan_or_open(self):
        self.sdk.init_results = iter([42])
        self.assertEqual(self.run_commands("scan 1", argv=["--wheel"]), 1)
        self.assertEqual(
            self.sdk.calls, [("message", False), ("logfile", False), ("init",)]
        )

    def test_wheel_sdk_version_failure_is_recorded_but_nonfatal(self):
        self.sdk.version_status = 0xFFFFFFFF
        self.assertEqual(self.run_commands("scan 1", "quit", argv=["--wheel"]), 0)
        self.assertEqual(self.events("sdk_version")[0]["status"], 0xFFFFFFFF)
        self.assertEqual(
            self.events("sdk_version")[0]["parts"], [26, 7, 28, 0xFFFFFFFF]
        )
        self.assertIn(("scan",), self.sdk.calls)
        self.assertEqual(self.sdk.calls[-1], ("release",))

    def test_wheel_startup_exceptions_release_only_after_confirmed_init(self):
        for name in (
            "EnableQHYCCDMessage",
            "EnableQHYCCDLogFile",
            "InitQHYCCDResource",
            "GetQHYCCDSDKVersion",
        ):
            with self.subTest(call=name):
                self.sdk.calls.clear()
                with patch.object(
                    self.sdk, name, side_effect=OSError("startup failure")
                ):
                    self.assertEqual(self.run_commands("quit", argv=["--wheel"]), 1)
                self.assertEqual(
                    ("release",) in self.sdk.calls, name == "GetQHYCCDSDKVersion"
                )
                self.assertNotIn(("scan",), self.sdk.calls)
                self.assertEqual(self.events("call_exception")[-1]["call"], name)

    def test_wheel_required_setup_failures_stop_early_and_close_once(self):
        setup = [("stream", 1234, 0), ("available", 1234, 17), ("param", 1234, 44)]
        for index, field, value in (
            (0, "stream_status", 42),
            (1, "available_status", 0xFFFFFFFF),
            (2, "slots", 0.0),
        ):
            with self.subTest(field=field):
                self.sdk = FakeSDK()
                self.loader.return_value = self.sdk
                setattr(self.sdk, field, value)
                self.assertEqual(
                    self.run_commands("cycle QHY-test 2", "reset", argv=["--wheel"]), 1
                )
                self.assertEqual(
                    self.sdk.calls,
                    [
                        ("message", False),
                        ("logfile", False),
                        ("init",),
                        ("version",),
                        ("open", b"QHY-test"),
                        *setup[: index + 1],
                        ("close", 1234),
                        ("release",),
                    ],
                )

    def test_wheel_slot_validation_preserves_nonfinite_and_double_results(self):
        for value in (
            float("nan"),
            float("inf"),
            -float("inf"),
            float(0xFFFFFFFF),
            float(2**31),
            float(-(2**31) - 1),
            -1.0,
            0.0,
            0.9,
        ):
            with self.subTest(slots=repr(value)):
                self.output.seek(0)
                self.output.truncate()
                self.sdk.calls.clear()
                self.sdk.slots = value
                self.assertEqual(
                    self.run_commands("hold QHY-test", argv=["--wheel"]), 1
                )
                self.assertEqual(self.sdk.calls[-2:], [("close", 1234), ("release",)])
                self.assertEqual(self.sdk.calls.count(("close", 1234)), 1)
                self.assertFalse(self.events("wheel_ready"))
                raw = next(
                    event
                    for event in self.events("call_result")
                    if event["call"] == "GetQHYCCDParam"
                )
                self.assertEqual(raw["raw_repr"], repr(value))
                self.assertEqual(raw["raw_hex"], value.hex())
                self.assertEqual(
                    raw["raw"], value if probe.math.isfinite(value) else None
                )
                self.assertNotIn(("model", b"QHY-test"), self.sdk.calls)

    def test_wheel_positive_fractional_slots_truncate_like_driver(self):
        for value, expected in ((7.9, 7), (1.0, 1), (float(2**31 - 1), 2**31 - 1)):
            with self.subTest(slots=value):
                self.sdk.slots = value
                self.assertEqual(
                    self.run_commands("cycle QHY-test 1", "quit", argv=["--wheel"]), 0
                )
                self.assertEqual(self.events("wheel_ready")[-1]["slots"], expected)
                raw = [
                    event
                    for event in self.events("call_result")
                    if event["call"] == "GetQHYCCDParam"
                ][-1]
                self.assertEqual(raw["raw"], value)
                self.assertEqual(raw["raw_hex"], value.hex())

    def test_wheel_model_and_status_failures_are_nonfatal_and_keep_raw_buffers(self):
        for status, value, position in (
            (42, b"3\0raw", -1),
            (0, b"x\0raw", -1),
            (0, b"\xff" * 64, -1),
            (0, b"0", 0),
            (0, b"9", 9),
        ):
            with self.subTest(status=status, value=value):
                self.sdk.model_status = 0xFFFFFFFF
                self.sdk.model_value = b"\xff" * 64
                self.sdk.status_status = status
                self.sdk.status_value = value
                self.assertEqual(
                    self.run_commands(
                        "scan 1", "cycle QHY-test 1", "quit", argv=["--wheel"]
                    ),
                    0,
                )
                ready = self.events("wheel_ready")[-1]
                self.assertEqual(
                    (ready["position"], ready["status"]), (position, status)
                )
                self.assertEqual(
                    ready["status_bytes_hex"], value.ljust(64, b"\0").hex()
                )
                model = self.events("camera_model")[-1]
                self.assertEqual(model["status"], 0xFFFFFFFF)
                self.assertEqual(model["model_bytes_hex"], (b"\xff" * 64).hex())
                self.assertEqual(model["model"], "\\xff" * 64)

    def test_wheel_postopen_exceptions_rollback_once_and_release(self):
        for name in (
            "SetQHYCCDStreamMode",
            "IsQHYCCDControlAvailable",
            "GetQHYCCDParam",
            "GetQHYCCDModel",
            "GetQHYCCDCFWStatus",
        ):
            for error, expected in (
                (OSError("fake SDK failure"), 1),
                (KeyboardInterrupt(), 130),
            ):
                with self.subTest(call=name, error=type(error).__name__):
                    self.sdk.calls.clear()
                    with patch.object(self.sdk, name, side_effect=error):
                        self.assertEqual(
                            self.run_commands("hold QHY-test", argv=["--wheel"]),
                            expected,
                        )
                    self.assertEqual(self.sdk.calls.count(("close", 1234)), 1)
                    self.assertEqual(
                        self.sdk.calls[-2:], [("close", 1234), ("release",)]
                    )
                    self.assertEqual(self.events("call_exception")[-1]["call"], name)

    def test_wheel_rollback_close_failure_or_uncertainty_blocks_retry_release_reset(
        self,
    ):
        original_emit = probe.emit
        for stage in ("status", "sdk", "result_log"):
            with self.subTest(stage=stage):
                self.sdk.calls.clear()
                self.sdk.stream_status = 42

                def close_camera(handle):
                    self.sdk.calls.append(("close", handle))
                    if stage == "sdk":
                        raise KeyboardInterrupt
                    return 42 if stage == "status" else 0

                def log(event, **fields):
                    if (
                        stage == "result_log"
                        and event == "call_result"
                        and fields.get("call") == "CloseQHYCCD"
                    ):
                        raise KeyboardInterrupt
                    original_emit(event, **fields)

                with patch.object(
                    self.sdk, "CloseQHYCCD", side_effect=close_camera
                ), patch.object(probe, "emit", side_effect=log):
                    self.assertEqual(
                        self.run_commands(
                            "cycle QHY-test 2", "close", "reset", argv=["--wheel"]
                        ),
                        1,
                    )
                self.assertEqual(
                    self.sdk.calls,
                    [
                        ("message", False),
                        ("logfile", False),
                        ("init",),
                        ("version",),
                        ("open", b"QHY-test"),
                        ("stream", 1234, 0),
                        ("close", 1234),
                    ],
                )

    def test_wheel_help_and_refusal_do_not_load_sdk(self):
        with self.assertRaises(SystemExit) as result:
            probe.main(["--wheel", "--help"])
        self.assertEqual(result.exception.code, 0)
        self.assertIn("--wheel", self.output.getvalue())
        with patch.object(probe, "ask", return_value="no"):
            self.assertEqual(probe.main(["--wheel"]), 1)
        self.loader.assert_not_called()
        self.assertFalse(self.sdk.calls)

    def test_session_records_effective_identity_groups_cwd_and_wheel_mode(self):
        values = {
            "getuid": 1001,
            "geteuid": 1002,
            "getgid": 1003,
            "getegid": 1004,
            "getgroups": [1003, 999],
            "getcwd": "/fake/probe-cwd",
        }
        with contextlib.ExitStack() as stack:
            stack.enter_context(
                patch.dict(
                    probe.os.environ,
                    {
                        "HOME": "/fake/probe-home",
                        "LD_LIBRARY_PATH": "/fake/vendor-libs",
                    },
                )
            )
            for name, value in values.items():
                stack.enter_context(patch.object(probe.os, name, return_value=value))
            for argv, wheel in (([], False), (["--wheel"], True)):
                self.assertEqual(self.run_commands("quit", argv=argv), 0)
                session = self.events("session")[-1]
                for key, name in (
                    ("uid", "getuid"),
                    ("euid", "geteuid"),
                    ("gid", "getgid"),
                    ("egid", "getegid"),
                    ("groups", "getgroups"),
                    ("cwd", "getcwd"),
                ):
                    self.assertEqual(session[key], values[name])
                self.assertIs(session["wheel"], wheel)
                self.assertEqual(session["home"], "/fake/probe-home")
                self.assertEqual(session["ld_library_path"], "/fake/vendor-libs")


if __name__ == "__main__":
    unittest.main()
