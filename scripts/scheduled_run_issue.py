#!/usr/bin/env python3
"""Keep one open issue per scheduled workflow while its runs fail.

A failed scheduled run is otherwise visible only in the Actions tab: nothing
opens an issue when the nightly build, the weekly CodeQL scan or another
scheduled workflow goes red. The workflow .github/workflows/scheduled-failure.yml
(issue #1352) runs this script after each scheduled or manually dispatched run
of those workflows on main.

- A failed, timed-out or never-started run opens an issue for its workflow, or
  comments on the one already open, with a link to the run.
- A successful run closes that workflow's open issue, unless one of its jobs
  was skipped: the nightly skips its build and suite when nothing moved, and
  such a run does not show that the failure was fixed.
- Any other result (cancelled, skipped, neutral) changes nothing.

The issue is found by a marker in its body, not by its title, so a retitled
issue is still found. The run details come from the environment.

Usage:
  scheduled_run_issue.py [--dry-run]   (WORKFLOW_NAME, CONCLUSION, RUN_URL,
                                        HEAD_SHA, EVENT, RUN_ID; GH_TOKEN, GH_REPO)
  scheduled_run_issue.py --self-test
"""
import argparse
import json
import os
import re
import subprocess
import sys

MARKER_RE = re.compile(r"<!-- scheduled-run: (\{.*?\}) -->")
FAILED = ("failure", "timed_out", "startup_failure")
LABELS = ("bug", "P3")
LIST_LIMIT = 500


def marker(workflow):
    return "<!-- scheduled-run: %s -->" % json.dumps({"workflow": workflow}, sort_keys=True)


def marked_workflow(body):
    """The workflow name in a body's marker, or None."""
    m = MARKER_RE.search(body or "")
    if not m:
        return None
    try:
        d = json.loads(m.group(1))
    except json.JSONDecodeError:
        return None
    return d.get("workflow") if isinstance(d, dict) else None


def find_issue(issues, workflow):
    """The lowest-numbered open issue whose marker names `workflow`, or None."""
    found = [i for i in issues if marked_workflow(i.get("body")) == workflow]
    return min(found, key=lambda i: i["number"]) if found else None


def decide(conclusion, issue):
    """'open', 'comment', 'close' or 'none' for a run with `conclusion` and the workflow's open `issue`."""
    if conclusion in FAILED:
        return "comment" if issue else "open"
    if conclusion == "success" and issue:
        return "close"
    return "none"


def effective_conclusion(conclusion, job_conclusions):
    """The run's conclusion, except that a successful run with a skipped job counts as 'skipped'.

    Such a run did not redo its work (the nightly skips its build and suite when
    the tag is already at HEAD), so it cannot show that a failure was fixed.
    """
    if conclusion == "success" and "skipped" in job_conclusions:
        return "skipped"
    return conclusion


def job_conclusions(run_id):
    """The conclusion of each job of run `run_id` (GH_REPO from the environment)."""
    out = subprocess.run(["gh", "api", "--paginate", "repos/{owner}/{repo}/actions/runs/%s/jobs" % run_id,
                          "--jq", ".jobs[].conclusion"], check=True, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line]


def run_from_env(env, jobs_fn):
    """The run dict for `reconcile` from the workflow's environment.

    A success with a run id is checked through `jobs_fn(run_id)` (the job
    conclusions), so a run that skipped its work does not count as a recovery.
    """
    conclusion = env["CONCLUSION"]
    if conclusion == "success" and env["RUN_ID"]:
        conclusion = effective_conclusion(conclusion, jobs_fn(env["RUN_ID"]))
    return {"workflow": env["WORKFLOW_NAME"], "conclusion": conclusion, "url": env["RUN_URL"],
            "sha": env["HEAD_SHA"], "event": env["EVENT"]}


def run_line(run):
    return "[%s run](%s) on `%s` (%s): **%s**." % (run["event"], run["url"], run["sha"][:7], run["workflow"],
                                                   run["conclusion"])


def issue_body(run):
    return ("%s\nA run of `%s` on main did not succeed.\n\n- %s\n\n"
            "The next run of `%s` that succeeds closes this issue; each further failure adds a comment.\n\n"
            "Opened by `.github/workflows/scheduled-failure.yml`.\n"
            % (marker(run["workflow"]), run["workflow"], run_line(run), run["workflow"]))


def reconcile(run, client):
    """Apply the decision for `run` through `client`; return a one-line summary."""
    issue = find_issue(client.list_open(), run["workflow"])
    action = decide(run["conclusion"], issue)
    if action == "open":
        client.create("Scheduled run failing: %s" % run["workflow"], issue_body(run), LABELS)
        return "opened an issue for %s" % run["workflow"]
    if action == "comment":
        client.comment(issue["number"], "Failed again: " + run_line(run))
        return "commented on #%d" % issue["number"]
    if action == "close":
        client.close(issue["number"], "Recovered: " + run_line(run))
        return "closed #%d" % issue["number"]
    return "nothing to do (%s, %s)" % (run["conclusion"], "issue #%d open" % issue["number"] if issue else "no issue")


class GhIssues:
    """Issue operations through the gh CLI (GH_TOKEN and GH_REPO from the environment)."""

    def __init__(self, dry_run):
        self.dry_run = dry_run

    def _gh(self, *args, stdin=None):
        if self.dry_run and args[1] != "list":
            print("DRY RUN: gh " + " ".join(args) + ("\n" + stdin if stdin else ""))
            return ""
        return subprocess.run(["gh", *args], input=stdin, check=True, capture_output=True, text=True).stdout

    def list_open(self):
        # No label filter: the marker identifies the issue, and triage may relabel it.
        out = self._gh("issue", "list", "--state", "open", "--limit", str(LIST_LIMIT), "--json", "number,body")
        return json.loads(out or "[]")

    def create(self, title, body, labels):
        args = ["issue", "create", "--title", title, "--body-file", "-"]
        for label in labels:
            args += ["--label", label]
        self._gh(*args, stdin=body)

    def comment(self, number, text):
        self._gh("issue", "comment", str(number), "--body-file", "-", stdin=text)

    def close(self, number, text):
        self._gh("issue", "close", str(number), "--comment", text)


def self_test():
    bad = []

    def check(name, got, want):
        if got != want:
            bad.append("FAIL %s: got %r, want %r" % (name, got, want))

    issue = {"number": 7, "body": marker("Nightly Build")}
    for conclusion, open_issue, want in [
        ("failure", None, "open"), ("timed_out", None, "open"), ("startup_failure", None, "open"),
        ("failure", issue, "comment"), ("timed_out", issue, "comment"),
        ("success", issue, "close"), ("success", None, "none"),
        ("cancelled", None, "none"), ("cancelled", issue, "none"),
        ("skipped", issue, "none"), ("neutral", None, "none"), ("", None, "none"),
    ]:
        check("decide(%r, %s)" % (conclusion, "issue" if open_issue else "None"), decide(conclusion, open_issue), want)

    issues = [{"number": 9, "body": "text\n" + marker("CodeQL")},
              {"number": 4, "body": marker("Nightly Build") + "\nretitled"},
              {"number": 3, "body": "<!-- scheduled-run: {not json} -->"},
              {"number": 2, "body": None},
              {"number": 8, "body": marker("Nightly Build")}]
    check("find by marker", (find_issue(issues, "CodeQL") or {}).get("number"), 9)
    check("lowest of two", (find_issue(issues, "Nightly Build") or {}).get("number"), 4)
    check("no marker for workflow", find_issue(issues, "OSV scan"), None)
    check("marker round trip", marked_workflow(issue_body({"workflow": "OSV scan", "event": "schedule",
                                                           "url": "u", "sha": "abcdef0123", "conclusion": "failure"})),
          "OSV scan")

    class Fake:
        def __init__(self):
            self.issues = []
            self.calls = []

        def list_open(self):
            return [i for i in self.issues if i["state"] == "open"]

        def create(self, title, body, labels):
            self.issues.append({"number": len(self.issues) + 1, "title": title, "body": body, "state": "open"})
            self.calls.append(("create", title, labels))

        def comment(self, number, text):
            self.calls.append(("comment", number))

        def close(self, number, text):
            self.issues[number - 1]["state"] = "closed"
            self.calls.append(("close", number))

    def run(workflow, conclusion):
        return {"workflow": workflow, "conclusion": conclusion, "event": "schedule",
                "url": "https://example.invalid/run", "sha": "0123456789abcdef"}

    f = Fake()
    for workflow, conclusion in [("Nightly Build", "failure"), ("CodeQL", "failure"), ("Nightly Build", "failure"),
                                 ("Nightly Build", "cancelled"), ("Nightly Build", "success"),
                                 ("Nightly Build", "success"), ("Nightly Build", "failure")]:
        reconcile(run(workflow, conclusion), f)
    check("reconcile sequence", f.calls, [
        ("create", "Scheduled run failing: Nightly Build", LABELS),
        ("create", "Scheduled run failing: CodeQL", LABELS),
        ("comment", 1),
        ("close", 1),
        ("create", "Scheduled run failing: Nightly Build", LABELS),
    ])
    check("CodeQL issue still open", [i["number"] for i in f.list_open()], [2, 3])

    # A green run that skipped its work proves nothing: the nightly's `check` job
    # skips `build` and `test-optimized` when the tag is already at HEAD, and the
    # run still concludes success (review of #1353).
    for conclusion, jobs, want in [
        ("success", ["success", "success", "success"], "success"),
        ("success", ["success", "skipped", "skipped"], "skipped"),
        ("success", ["success", "success", "skipped"], "skipped"),
        ("success", [], "success"),
        ("failure", ["success", "failure", "skipped"], "failure"),
    ]:
        check("effective_conclusion(%r, %r)" % (conclusion, jobs), effective_conclusion(conclusion, jobs), want)
    g = Fake()
    reconcile(run("Nightly Build", "failure"), g)
    reconcile(run("Nightly Build", effective_conclusion("success", ["success", "skipped", "skipped"])), g)
    check("a skip-night success leaves the failure issue open", [i["number"] for i in g.list_open()], [1])

    # main() applies the rule: the run built from the environment reads the jobs
    # only for a success with a run id, and downgrades a skip-night success.
    env = {"WORKFLOW_NAME": "Nightly Build", "CONCLUSION": "success", "RUN_URL": "u", "HEAD_SHA": "abc1234",
           "EVENT": "schedule", "RUN_ID": "42"}
    asked = []

    def jobs(run_id):
        asked.append(run_id)
        return ["success", "skipped", "skipped"]

    check("run_from_env downgrades a skip-night success", run_from_env(env, jobs)["conclusion"], "skipped")
    check("run_from_env reads the jobs of that run", asked, ["42"])
    asked.clear()
    check("run_from_env keeps a failure as is", run_from_env(dict(env, CONCLUSION="failure"), jobs)["conclusion"],
          "failure")
    check("run_from_env reads no jobs for a failure", asked, [])
    check("run_from_env without a run id keeps success", run_from_env(dict(env, RUN_ID=""), jobs)["conclusion"],
          "success")

    # The open issue is found by its marker whatever its labels: a triaged issue
    # that lost `bug` must still be found, or a second one opens (PR #1353 review).
    class Recording(GhIssues):
        def __init__(self):
            super().__init__(dry_run=False)
            self.calls = []

        def _gh(self, *args, stdin=None):
            self.calls.append(args)
            return json.dumps([{"number": 5, "body": marker("CodeQL")}])

    r = Recording()
    check("list_open returns every listed issue", [i["number"] for i in r.list_open()], [5])
    check("list_open is not filtered by label", "--label" in r.calls[0], False)
    check("list_open asks for open issues only", r.calls[0][r.calls[0].index("--state") + 1], "open")

    for line in bad:
        print(line)
    if bad:
        return 1
    print("scheduled_run_issue self-test: all cases passed")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    env = {k: os.environ.get(k, "") for k in ("WORKFLOW_NAME", "CONCLUSION", "RUN_URL", "HEAD_SHA", "EVENT", "RUN_ID")}
    missing = [k for k in ("WORKFLOW_NAME", "RUN_URL") if not env[k]]
    if missing:
        p.error("missing environment: " + ", ".join(missing))
    print(reconcile(run_from_env(env, job_conclusions), GhIssues(args.dry_run)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
