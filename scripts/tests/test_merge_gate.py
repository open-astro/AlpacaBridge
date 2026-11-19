#!/usr/bin/env python3
"""The merge gate (.github/scripts/merge-gate.sh) against a fake gh.

Run from the repo root:

    python3 -I -m unittest discover -s scripts/tests -v

The gate reads the queued PR, its head's check runs and commit statuses, and the
bot's comments through gh, and selects with jq. These tests put a gh stub first
on PATH that answers each call from a fixture, and check the gate's exit code and
output. The real jq does the selecting.
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
GATE = REPO_ROOT / ".github" / "scripts" / "merge-gate.sh"
REF = "refs/heads/gh-readonly-queue/main/pr-7-0123abcd"
SHA = "a" * 40
REVIEW_START = "2026-11-12T10:00:00Z"

# Answers each gh call from a file in $FAKE_GH; the gate pipes the output to jq.
GH_STUB = r"""#!/usr/bin/env python3
import os, sys
d = os.environ["FAKE_GH"]
a = sys.argv[1:]
if a[:2] == ["pr", "view"]:
    name = "pr.json"
elif a[:2] == ["pr", "diff"]:
    name = "diff.txt"
elif a[0] == "api":
    path = next(x for x in a[1:] if not x.startswith("-"))
    name = ("runs.json" if "/check-runs" in path else
            "status.json" if path.endswith("/status") else
            "comments.json" if "/comments" in path else None)
    if name is None:
        sys.exit("unexpected gh api path: " + path)
else:
    sys.exit("unexpected gh call: " + " ".join(a))
sys.stdout.write(open(os.path.join(d, name), encoding="utf-8").read())
"""


def run(name, conclusion: str | None = "success", status="completed", id_=1):
    return {"id": id_, "name": name, "status": status, "conclusion": conclusion,
            "started_at": REVIEW_START}


def review(id_=100, conclusion="success"):
    return run("review", conclusion, id_=id_)


def comment(body, at="2026-11-12T10:30:00Z", login="github-actions[bot]"):
    return {"user": {"login": login}, "updated_at": at, "body": body}


APPROVED = comment("## Defects\n\nNone.\n\n## Notes\n\n- a note\n\n✅ Approved\n")
ISSUES = comment("## Defects\n\n- a defect\n\n⚠️ Issues found\n")


class MergeGateTest(unittest.TestCase):
    def gate(self, runs, comments=(), statuses=(), author="someone", diff="", ref=REF):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            stub = root / "bin" / "gh"
            stub.parent.mkdir()
            stub.write_text(GH_STUB, encoding="utf-8")
            stub.chmod(stub.stat().st_mode | stat.S_IEXEC)
            files = {
                "pr.json": json.dumps({"headRefOid": SHA, "author": {"login": author}}),
                "runs.json": json.dumps({"check_runs": list(runs)}),
                "status.json": json.dumps({"statuses": list(statuses)}),
                "comments.json": json.dumps(list(comments)),
                "diff.txt": diff,
            }
            for name, text in files.items():
                (root / name).write_text(text, encoding="utf-8")
            env = dict(os.environ, FAKE_GH=d, PATH=f"{stub.parent}{os.pathsep}{os.environ['PATH']}",
                       MERGE_GATE_WAIT_MINUTES="0")
            p = subprocess.run(["bash", str(GATE), "open-astro/x", ref], env=env,
                               capture_output=True, text=True, timeout=60)
        return p.returncode, p.stdout + p.stderr

    def assertPasses(self, result):
        code, out = result
        self.assertEqual(code, 0, out)

    def assertFails(self, result, needle):
        code, out = result
        self.assertNotEqual(code, 0, out)
        self.assertIn(needle, out)

    def test_the_newest_run_of_each_name_decides(self):
        # A PR-body edit starts a new suite: the old red run of the same name no longer counts.
        self.assertPasses(self.gate([run("falsified-by", "failure", id_=1), run("falsified-by", id_=2), review()],
                                    [APPROVED]))
        self.assertFails(self.gate([run("falsified-by", id_=1), run("falsified-by", "failure", id_=2), review()],
                                   [APPROVED]), "falsified-by: failure")

    def test_neutral_is_green_and_any_other_conclusion_is_red(self):
        self.assertPasses(self.gate([run("CodeQL", "neutral"), review()], [APPROVED]))
        for conclusion in ("failure", "cancelled", "timed_out", "action_required"):
            self.assertFails(self.gate([run("CodeQL", conclusion), review()], [APPROVED]), f"CodeQL: {conclusion}")

    def test_skipped_runs_do_not_count(self):
        self.assertPasses(self.gate([run("TSan", "skipped"), run("review-fork", "skipped", id_=200), review()],
                                    [APPROVED]))

    def test_a_check_still_running_fails_after_the_wait(self):
        self.assertFails(self.gate([run("Bench", None, "in_progress"), review()], [APPROVED]),
                         "did not finish")
        self.assertFails(self.gate([review()], [APPROVED], statuses=[{"context": "adr", "state": "pending"}]),
                         "did not finish")

    def test_a_failed_commit_status_fails(self):
        self.assertPasses(self.gate([review()], [APPROVED], statuses=[{"context": "adr", "state": "success"}]))
        self.assertFails(self.gate([review()], [APPROVED], statuses=[{"context": "adr", "state": "failure"}]),
                         "adr: failure")

    def test_only_an_approved_verdict_from_the_latest_review_passes(self):
        self.assertFails(self.gate([review()], [ISSUES]), 'is not "✅ Approved"')
        # An approval written before the latest review started is from an earlier head.
        stale = comment(APPROVED["body"], at="2026-11-12T09:59:59Z")
        self.assertFails(self.gate([review()], [stale]), "posted no verdict")
        self.assertFails(self.gate([review()], [stale, ISSUES]), 'is not "✅ Approved"')
        # The newest verdict decides.
        self.assertPasses(self.gate([review()], [ISSUES, comment(APPROVED["body"], at="2026-11-12T10:45:00Z")]))
        self.assertFails(self.gate([run("CodeQL")], [APPROVED]), "No review ran")

    def test_only_the_bots_sign_off_line_is_a_verdict(self):
        human = comment("✅ Approved", login="someone")
        quoted = comment("The review will sign off with ✅ Approved or ⚠️ Issues found when it ends.")
        self.assertFails(self.gate([review()], [human]), "posted no verdict")
        self.assertFails(self.gate([review()], [quoted]), "posted no verdict")
        self.assertPasses(self.gate([review()], [comment("✅ Approved", login="claude[bot]")]))

    def test_dependabot_and_a_review_workflow_edit_skip_the_verdict(self):
        self.assertPasses(self.gate([run("CI")], [], author="app/dependabot"))
        self.assertPasses(self.gate([review()], [], diff=".github/workflows/claude-review.yml\n"))
        self.assertFails(self.gate([review()], [], diff="README.md\n"), "posted no verdict")
        # The exemption is for the verdict only: a red check still fails.
        self.assertFails(self.gate([run("CI", "failure")], [], author="app/dependabot"), "CI: failure")

    def test_a_ref_without_a_pull_request_number_fails(self):
        self.assertFails(self.gate([review()], [APPROVED], ref="refs/heads/main"), "No pull request number")


if __name__ == "__main__":
    unittest.main()
