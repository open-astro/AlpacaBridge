#!/usr/bin/env python3
"""Require the PR body to follow .github/PULL_REQUEST_TEMPLATE.md.

GitHub fills the template in only when a PR is opened from the web compare
page with an empty body. A body passed up front (`gh pr create --body`, the
API, an agent) skips it, so the template's sections went missing on PRs that
were otherwise complete.

The rules are read from the template itself, so editing the template changes
the check with no edit here:

  * Every `## <Heading>` of the template appears in the body (case and
    surrounding spaces ignored; order not checked).
  * A section whose template holds `- [ ]` items (the checklist) keeps at least
    one `- [ ]` or `- [x]` item.
  * Every other section holds at least one line of its own: not blank, not a
    bare `-` or `>` marker, and not a line copied unchanged from the template's
    placeholder text in that section.

HTML comments are ignored on both sides, and `## ` lines inside fenced code
blocks are not headings. The script checks shape, not quality; review reads
the content.

The PR body is read from the PR_BODY environment variable (or --body-file),
never from a command line, because it is untrusted text.

Usage:
    check_pr_template.py [--body-file FILE] [--template FILE]
    check_pr_template.py --self-test
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, ".github", "PULL_REQUEST_TEMPLATE.md")


COMMENT_RE = re.compile(r"<!--.*?-->", re.S)
HEADING_RE = re.compile(r"^##\s+(.+?)\s*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
CHECKBOX_RE = re.compile(r"^\s*[-*]\s+\[[ xX]\]\s+\S")
FILLER_RE = re.compile(r"^[->*\s]*$")


def sections(text):
    """Return [(normalized heading, display heading, lines)] for each `## ` section."""
    text = COMMENT_RE.sub("", text.replace("\r\n", "\n"))
    out = []
    in_fence = False
    for line in text.split("\n"):
        if FENCE_RE.match(line):
            in_fence = not in_fence
        m = None if in_fence else HEADING_RE.match(line)
        if m:
            title = m.group(1)
            out.append((" ".join(title.lower().split()), title, []))
        elif out:
            out[-1][2].append(line.strip())
    return out


def check(body, template):
    """Return the template violations of one PR body."""
    if not body.strip():
        return ["the PR body is empty"]
    found = {}
    for key, _, lines in sections(body):
        found.setdefault(key, lines)
    errors = []
    for key, title, tlines in sections(template):
        # "A or B" offers a choice, so the body may name either path on its own
        names = [key] + key.split(" or ") if " or " in key else [key]
        hit = next((n for n in names if n in found), None)
        if hit is None:
            errors.append("missing section '## %s'" % title)
            continue
        lines = found[hit]
        if any(CHECKBOX_RE.match(t) for t in tlines):
            if not any(CHECKBOX_RE.match(line) for line in lines):
                errors.append("section '## %s' has no '- [ ]' or '- [x]' items" % title)
            continue
        placeholders = {t for t in tlines if t}
        own = [line for line in lines if not FILLER_RE.match(line) and line not in placeholders]
        if not own:
            errors.append("section '## %s' is empty or holds only the template's placeholder text" % title)
    return errors


def main(argv):
    body_file = None
    template = TEMPLATE
    args = iter(argv)
    for arg in args:
        if arg == "--body-file":
            body_file = next(args, None)
        elif arg == "--template":
            template = next(args, None)
        else:
            template = None
        if template is None:
            print("usage: check_pr_template.py [--body-file FILE] [--template FILE] | --self-test", file=sys.stderr)
            return 2
    if body_file:
        with open(body_file, encoding="utf-8") as f:
            body = f.read()
    else:
        body = os.environ.get("PR_BODY", "")
    with open(template, encoding="utf-8") as f:
        template_text = f.read()
    errors = check(body, template_text)
    for e in errors:
        print("pr-template: " + e)
    if errors:
        print("Fill in the PR body from .github/PULL_REQUEST_TEMPLATE.md (%d problem(s))." % len(errors))
        return 1
    print("PR body follows the template.")
    return 0


def self_test():
    failures = []

    def expect(cond, msg):
        if not cond:
            failures.append(msg)

    with open(TEMPLATE, encoding="utf-8") as f:
        real = f.read()
    tpl = (
        "<!-- top note -->\n## Thinking Path\n<!-- say why -->\n> - Project line\n> - [placeholder]\n\n"
        "## What Changed\n\n-\n\n## Checklist\n\n- [ ] item one\n- [ ] item two\n"
    )
    good = (
        "## Thinking Path\n> - Project line\n> - The real reason\n\n"
        "## What Changed\n- `a.py`: did a thing\n\n## Checklist\n- [x] item one\n- [ ] item two\n"
    )
    expect(check(good, tpl) == [], "filled body rejected: %r" % check(good, tpl))
    expect(check(good.replace("\n", "\r\n"), tpl) == [], "CRLF body rejected")
    expect(check(good.replace("## What Changed", "##  what changed "), tpl) == [], "heading case or spacing rejected")
    expect(len(check(tpl, tpl)) == 2, "unfilled template: expected 2 problems, got %r" % check(tpl, tpl))
    expect(any("Thinking Path" in e for e in check(good.replace("> - The real reason\n", ""), tpl)),
           "section holding only template lines accepted")
    expect(any("What Changed" in e for e in check(good.replace("- `a.py`: did a thing", "-"), tpl)),
           "section holding only a bare '-' accepted")
    expect(any("What Changed" in e for e in check(good.replace("- `a.py`: did a thing", "<!-- x -->"), tpl)),
           "section holding only a comment accepted")
    expect(any("missing" in e and "What Changed" in e for e in check(good.replace("## What Changed", "## Summary"), tpl)),
           "missing section accepted")
    expect(any("Checklist" in e for e in check(good.replace("- [x] item one\n- [ ] item two\n", "done\n"), tpl)),
           "checklist without items accepted")
    fenced = good.replace("## What Changed\n- `a.py`: did a thing\n", "## Other\n```\n## What Changed\n```\n")
    expect(any("missing" in e and "What Changed" in e for e in check(fenced, tpl)), "heading inside a fence counted")
    expect(any("empty" in e for e in check("", tpl)), "empty body accepted")
    choice_tpl = "## Linked Issues or Issue Description\n\n-\n"
    for heading in ("Linked Issues or Issue Description", "Issue Description", "Linked Issues"):
        expect(check("## %s\nCloses #1\n" % heading, choice_tpl) == [], "choice heading '%s' rejected" % heading)
    expect(any("missing" in e for e in check("## Issues\nCloses #1\n", choice_tpl)), "other heading accepted for a choice")
    # the real template: unfilled fails, the #857-style Summary/Validation body fails
    expect(len(check(real, real)) >= 6, "unfilled real template: %r" % check(real, real))
    old_style = "Closes #1.\n\n## Summary\n- x\n\n## Validation\n- y\n\n## Falsified by:\n- z\n"
    expect(len(check(old_style, real)) >= 6, "Summary/Validation body accepted: %r" % check(old_style, real))

    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("check_pr_template self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    sys.exit(main(sys.argv[1:]))
