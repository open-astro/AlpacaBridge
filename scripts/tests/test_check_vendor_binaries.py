# Copyright (c) 2025-2026 Joey Troy and contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""check_vendor_binaries.py over a throwaway git repository (issue #2633)."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "check_vendor_binaries.py"
spec = importlib.util.spec_from_file_location("check_vendor_binaries", SCRIPT)
cvb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cvb)


class CheckVendorBinaries(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.ext = self.root / cvb.EXTERNAL
        (self.ext / "V/lib").mkdir(parents=True)
        (self.ext / "V/lib/libv.so.1").write_bytes(b"\x7fELF\0\0vendor")
        (self.ext / "V/My Manual.pdf").write_bytes(b"%PDF\0x")
        (self.ext / "V/README.md").write_text("text, not listed\n")
        (self.ext / "V/lib/libv.so").symlink_to("libv.so.1")
        self.git("init", "-q")
        self.git("add", "-A")
        self.assertEqual(cvb.write(self.root), 2)
        self.git("add", "-A")

    def tearDown(self):
        self._tmp.cleanup()

    def git(self, *args):
        subprocess.run(["git", *args], cwd=self.root, check=True)

    def test_clean_tree_passes(self):
        self.assertEqual(cvb.problems(self.root), [])
        self.assertIn("  V/My Manual.pdf\n", (self.ext / cvb.MANIFEST).read_text())

    def test_changed_byte_fails(self):
        (self.ext / "V/lib/libv.so.1").write_bytes(b"\x7fELF\0\0vendoR")
        self.assertEqual(cvb.problems(self.root), ["V/lib/libv.so.1: sha256 differs from SHA256SUMS"])

    def test_unlisted_binary_fails(self):
        (self.ext / "V/lib/libnew.a").write_bytes(b"!<arch>\0")
        self.git("add", "-A")
        self.assertEqual(cvb.problems(self.root), ["V/lib/libnew.a: tracked binary missing from SHA256SUMS"])

    def test_listed_but_removed_fails(self):
        self.git("rm", "-q", "-f", f"{cvb.EXTERNAL}/V/My Manual.pdf")
        self.assertEqual(cvb.problems(self.root), ["V/My Manual.pdf: listed in SHA256SUMS but not a tracked binary"])

    def test_malformed_or_missing_manifest_fails(self):
        (self.ext / cvb.MANIFEST).write_text("deadbeef V/lib/libv.so.1\n")
        self.assertEqual(len(cvb.problems(self.root)), 1)
        (self.ext / cvb.MANIFEST).unlink()
        self.assertEqual(len(cvb.problems(self.root)), 1)


if __name__ == "__main__":
    unittest.main()
