#!/usr/bin/env python3
"""Check canonical instruction discovery and Claude adapters without an AI session."""
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
# Tripwire for a renamed documents directory, not a target count: the tree has
# ~44 linked Markdown documents, so a scan that reads a handful means a glob or
# a root regressed and the link check is passing over nothing.
MIN_LINKED_DOCUMENTS = 20
# The skills tree is the one source whose entire contribution (~10 files) fits
# inside the aggregate floor's margin, so a renamed `.claude/skills` would scan
# nothing and still clear MIN_LINKED_DOCUMENTS. Tripwire, not a target count.
MIN_SKILL_DOCUMENTS = 5
# Root documents scanned by name (issue #693), each with a floor on its DISTINCT
# relative link/src targets. Distinct, not occurrences: three links to one file
# are one target, so a regression that drops every link to that file cannot
# hide behind repeats. Every floor is a tripwire for a gutted or deleted file
# or a dead extractor, never a target count; a single broken link is its own
# finding regardless of the floor. The two small files (README.md: the logo,
# LICENSE, CHANGELOG.md, SUPPORTED-DRIVERS.md, docs/development.md;
# CHANGELOG.md: the logo and the component READMEs) sit one link below today's
# count so one deliberate removal is not misdiagnosed as a regression. The
# driver matrix's floor is deliberately far below its count: its links are one
# per validated model, rows come and go with validation work, and the floor
# only has to catch the file being emptied, so do not raise it row by row.
# Lower a floor only when a file legitimately loses links.
ROOT_DOCUMENT_LINK_FLOORS = {'README.md': 4, 'SUPPORTED-DRIVERS.md': 40, 'CHANGELOG.md': 2}
# A Markdown link target may contain one level of balanced parentheses
# (`AlpacaCore/conformu/ZWO/ASIair%20Plus%20(Pi%20CM4)/`); a plain `[^)]+`
# stops at the first `)` and reports a truncated path that does not exist.
MARKDOWN_LINK_TARGET_RE = re.compile(r'\]\(((?:[^\s()]|\([^\s()]*\))+)\)')
# HTML image/embed sources: the logo is `<img src="docs/image/ab.png">`.
HTML_SRC_TARGET_RE = re.compile(r'\bsrc="([^"\s]+)"')
# Fenced code blocks are removed before either scan (the same rule as check 7
# in check_docs_drift.py): a skill doc that illustrates `<img src="...">` or a
# link in a fence is showing an example, not naming a repo file.
FENCED_BLOCK_RE = re.compile(r'^[ \t]*(`{3,}|~{3,}).*?^[ \t]*\1[ \t]*$', re.S | re.M)


def matches(path, pattern):
    # Unlike fnmatch, a single star must not cross a directory boundary.
    expression = re.escape(pattern).replace(r'\*\*/', '(?:.*/)?')
    expression = expression.replace(r'\*\*', '.*').replace(r'\*', '[^/]*')
    return re.fullmatch(expression, path) is not None


def heading_anchors(text):
    """GitHub's anchors for every Markdown heading in `text`: lower case, every
    character that is not a letter, digit, space, hyphen or underscore dropped,
    spaces turned into hyphens, and -1, -2 ... appended to repeats."""
    anchors, seen = set(), {}
    for line in FENCED_BLOCK_RE.sub('', text).splitlines():
        m = re.match(r'^#{1,6} +(.+?) *#*$', line)
        if not m:
            continue
        slug = re.sub(r'[^\w\- ]', '', m.group(1).strip().lower()).replace(' ', '-')
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        anchors.add(slug if count == 0 else '%s-%d' % (slug, count))
    return anchors


def check(root=ROOT):
    failures = []
    def require(ok, message):
        if not ok:
            failures.append(message)
    def read(path):
        p = root / path
        return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ''
    index = read('docs/agent-instructions.md')
    require('@AGENTS.md' in read('CLAUDE.md') and '@docs/agent-instructions.md' in read('CLAUDE.md'),
            'CLAUDE.md must import the shared core and instruction index')
    require('docs/agent-instructions.md' in read('AGENTS.md')[:2048],
            'AGENTS.md must route to the index before a client truncates it')
    canonical = sorted((root / '.github/instructions').glob('*.instructions.md'))
    require(bool(canonical), 'No canonical instructions found')
    scopes = {}
    for path in canonical:
        name = path.name.removesuffix('.instructions.md')
        text = path.read_text(encoding="utf-8", errors="replace")
        metadata = re.match(r'---\napplyTo: ("[^\n]+")\n---\n', text)
        require(metadata is not None, '%s: missing applyTo metadata' % name)
        if not metadata:
            continue
        patterns = json.loads(metadata[1]).split(',')
        scopes[name] = patterns
        adapter = read('.claude/rules/%s.md' % name)
        native = re.match(r'---\npaths:\n((?:  - "[^\n]+"\n)+)---\n', adapter)
        actual = [json.loads(line[4:]) for line in native[1].splitlines()] if native else []
        require(actual == patterns, '%s: Claude paths differ from applyTo' % name)
        require('`.github/instructions/%s.instructions.md` in full' % name in adapter,
                '%s: Claude adapter must require reading the canonical file' % name)
        require('../.github/instructions/%s.instructions.md' % name in index,
                '%s: missing from instruction index' % name)
        require(bool(text[metadata.end():].strip()), '%s: instruction body is empty' % name)
    vendors = root / 'AlpacaCore/src/vendors'
    for directory in sorted(vendors.iterdir()):
        if not directory.is_dir():
            continue
        vendor = directory.name
        require(vendor in scopes, '%s: vendor has no instructions' % vendor)
        paths = list(directory.rglob('*'))
        paths += list((root / 'AlpacaCore/include/alpacacore/vendor' / vendor).rglob('*'))
        paths += list((root / 'AlpacaCore/tests').glob('*%s*' % vendor))
        for path in paths:
            if path.is_file():
                rel = path.relative_to(root).as_posix()
                require(any(matches(rel, p) for p in scopes.get(vendor, [])),
                        '%s: uncovered vendor file %s' % (vendor, rel))
    for adapter in (root / '.claude/rules').glob('*.md'):
        require(adapter.stem in scopes, '%s: orphan Claude adapter' % adapter.name)
    for name in ('alpaca-http-conformance', 'wifi-manager'):
        require(any(matches('AlpacaHTTP/src/main.cpp', p) for p in scopes.get(name, [])),
                '%s: HTTP startup is not covered' % name)
    # Resolve relocated Markdown links relative to their actual owning files.
    documents = canonical + [root / 'docs/agent-instructions.md', root / 'CONTEXT.md',
                             root / 'docs/architecture.md']
    documents += [root / name for name in ROOT_DOCUMENT_LINK_FLOORS]
    documents += list((root / 'docs/failures').glob('*.md'))
    documents += list((root / 'docs/decisions').glob('*.md'))
    skill_documents = [path for path in (root / '.claude/skills').rglob('*.md') if path.is_file()]
    require(len(skill_documents) >= MIN_SKILL_DOCUMENTS,
            'only %d Markdown document(s) found under .claude/skills/ (floor %d): the directory '
            'was renamed or the rglob regressed' % (len(skill_documents), MIN_SKILL_DOCUMENTS))
    documents += skill_documents
    documents = [path for path in documents if path.is_file()]
    require(len(documents) >= MIN_LINKED_DOCUMENTS,
            'only %d Markdown document(s) found for the link check (floor %d): a document '
            'directory was renamed or a glob regressed' % (len(documents), MIN_LINKED_DOCUMENTS))
    distinct_targets = {name: set() for name in ROOT_DOCUMENT_LINK_FLOORS}
    for path in documents:
        # Markdown links plus HTML src attributes: README.md, SUPPORTED-DRIVERS.md
        # and CHANGELOG.md all embed the logo as `<img src="docs/image/ab.png">`
        # (issue #693). Targets are percent-decoded before the existence check:
        # the driver matrix links its ConformU report directories with `%20`
        # for the spaces in model names.
        text = FENCED_BLOCK_RE.sub('', path.read_text(encoding="utf-8", errors="replace"))
        targets = MARKDOWN_LINK_TARGET_RE.findall(text) + HTML_SRC_TARGET_RE.findall(text)
        rel_path = path.relative_to(root).as_posix()
        for target in targets:
            if target.startswith(('http:', 'https:', 'mailto:')):
                continue
            fragment = unquote(target.split('#', 1)[1]) if '#' in target else ''
            if target.startswith('#'):
                # A same-file link must name one of the document's own headings.
                require(not fragment or fragment in heading_anchors(text),
                        '%s: link %s names a heading that does not exist' % (rel_path, target))
                continue
            file = unquote(target.split('#', 1)[0])
            if path.parent == root and path.name in distinct_targets:
                distinct_targets[path.name].add(file)
            require((path.parent / file).exists(), '%s: broken relative link %s'
                    % (path.relative_to(root).as_posix(), target))
            # A link to a Markdown file that names a heading must name one that
            # exists: renamed headings broke cross-file anchors silently (#1388;
            # #1412 first checked the decision records).
            target_path = path.parent / file
            if fragment and file.endswith('.md') and target_path.is_file():
                require(fragment in heading_anchors(target_path.read_text(encoding="utf-8", errors="replace")),
                        '%s: link %s names a heading that does not exist' % (rel_path, target))
    for name, floor in ROOT_DOCUMENT_LINK_FLOORS.items():
        require(len(distinct_targets[name]) >= floor,
                'only %d distinct relative link/src target(s) found in %s (floor %d): the file is missing, '
                'was gutted, or the link extractor regressed' % (len(distinct_targets[name]), name, floor))
    return failures


def self_test():
    import tempfile
    import shutil
    assert matches('a/b/c.cpp', 'a/**')
    assert not matches('a/b/c.cpp', 'a/*')
    assert matches('AlpacaCore/conformu/Player One/model/Linux-arm64.txt', 'AlpacaCore/conformu/Player One/**')
    assert not matches('AlpacaCore/src/vendors/celestron/x.cpp', 'AlpacaCore/src/vendors/gemini/**')
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for directory in ('.github/instructions', '.claude/rules', 'docs', 'AlpacaCore/src/vendors',
                          'AlpacaCore/include/alpacacore/vendor', 'AlpacaCore/tests',
                          '.claude/skills'):
            shutil.copytree(ROOT / directory, root / directory)
        for file in ('CLAUDE.md', 'AGENTS.md', 'CONTEXT.md', 'README.md', 'SUPPORTED-DRIVERS.md',
                     'CHANGELOG.md'):
            shutil.copy(ROOT / file, root / file)
        # Link targets outside the small fixture are intentionally absent; compare
        # new diagnostics against the fixture baseline instead of hiding failures.
        baseline = set(check(root))
        mutations = [
            ('CLAUDE.md', lambda s: ''),
            ('.claude/rules/gemini.md', lambda s: s.replace('gemini/**', 'celestron/**')),
            ('.github/instructions/gemini.instructions.md', lambda s: s.split('---', 2)[0]),
            ('docs/agent-instructions.md', lambda s: s.replace('../.github/instructions/gemini.instructions.md', 'missing.md')),
        ]
        for file, mutate in mutations:
            path = root / file
            original = path.read_text(encoding="utf-8", errors="replace")
            path.write_text(mutate(original))
            assert set(check(root)) - baseline, 'Mutation escaped detection: ' + file
            path.write_text(original)
        # A broken link is reported with its repo-relative path: two files
        # named README.md (or two records with one name) must be told apart.
        assert 'hardware-free-driver-tests-via-the-sdk-seam--extend' in heading_anchors(
            '### Hardware-free driver tests via the SDK seam — extend\n'), 'heading_anchors slug rule regressed'
        assert 'a-1' in heading_anchors('## A\n## A\n'), 'heading_anchors does not number repeats'
        anchor_probe = root / 'docs/decisions/zz-anchor.md'
        anchor_probe.write_text('[x](../../.github/instructions/gemini.instructions.md#no-such-heading)\n')
        new_findings = set(check(root)) - baseline
        anchor_probe.unlink()
        assert any('zz-anchor.md' in f and 'no-such-heading' in f for f in new_findings), \
            'A decision-record link to a missing heading escaped the anchor check'
        # Any document's link fragment is checked (#1388), not only the decision
        # records': rename a heading a cross-file link targets, and break a
        # same-file (#anchor) link.
        probe = root / 'docs/failures/zz-anchor-probe.md'
        probe.write_text('[x](../../.github/instructions/gemini.instructions.md#no-such-heading)\n')
        new_findings = set(check(root)) - baseline
        assert any('zz-anchor-probe.md' in f and 'no-such-heading' in f for f in new_findings), \
            'A cross-file link to a missing heading outside docs/decisions escaped the anchor check'
        probe.write_text('# Real\n[ok](#real) [bad](#no-such-heading)\n')
        new_findings = set(check(root)) - baseline
        probe.write_text('# Real\n[ok](#real)\n')
        assert not (set(check(root)) - baseline), 'A same-file link to an existing heading was reported'
        probe.unlink()
        assert any('zz-anchor-probe.md' in f and '#no-such-heading' in f for f in new_findings), \
            'A same-file (#anchor) link to a missing heading escaped the anchor check'
        index = root / 'docs/architecture.md'
        original = index.read_text(encoding="utf-8", errors="replace")
        link = re.search(r'\]\(([^)#\s]+\.md)#([^)\s]+)\)', original)
        assert link, 'docs/architecture.md has no cross-file anchor link for the rename mutation'
        target = (index.parent / link[1]).resolve()
        assert target.is_file() and root in target.parents, 'rename mutation target is not in the fixture'
        target_text = target.read_text(encoding="utf-8", errors="replace")
        target.write_text(re.sub(r'^(#{1,6} )', r'\1Renamed ', target_text, flags=re.M))
        renamed = set(check(root)) - baseline
        target.write_text(target_text)
        assert any('docs/architecture.md' in f and 'names a heading that does not exist' in f
                   for f in renamed), \
            'Renaming the headings a cross-file link targets escaped the anchor check'
        broken = root / 'docs/decisions/zz-broken-link.md'
        broken.write_text('[gone](missing-target.md)\n')
        new_findings = set(check(root)) - baseline
        broken.unlink()
        assert any('docs/decisions/zz-broken-link.md' in f and 'missing-target.md' in f
                   for f in new_findings), 'Broken link is not reported with its repo-relative path'
        # The root glossary and the architecture overview carry links too, and
        # neither sits in a scanned directory: each must be read by name.
        for name in ('CONTEXT.md', 'docs/architecture.md'):
            page = root / name
            original = page.read_text(encoding="utf-8", errors="replace")
            page.write_text(original + '\n[gone](missing-target.md)\n')
            new_findings = set(check(root)) - baseline
            page.write_text(original)
            assert any(name in f and 'missing-target.md' in f for f in new_findings), \
                'A broken link in %s escaped the link check' % name
        # README.md (issue #693): a broken Markdown link and a broken <img src>
        # are each reported, and a README with no relative targets trips its
        # own floor instead of scanning nothing.
        page = root / 'README.md'
        original = page.read_text(encoding="utf-8", errors="replace")
        page.write_text(original + '\n[gone](missing-target.md)\n<img src="docs/image/missing.png" alt="x">\n')
        new_findings = set(check(root)) - baseline
        assert any('README.md' in f and 'missing-target.md' in f for f in new_findings), \
            'A broken Markdown link in README.md escaped the link check'
        assert any('README.md' in f and 'docs/image/missing.png' in f for f in new_findings), \
            'A broken <img src> in README.md escaped the link check'
        # Inside a fenced code block the same two targets are examples, not links.
        page.write_text(original + '\n```html\n[gone](missing-target.md)\n<img src="docs/image/missing.png">\n```\n')
        new_findings = set(check(root)) - baseline
        page.write_text(original)
        assert not any('missing-target.md' in f or 'docs/image/missing.png' in f for f in new_findings), \
            'A link or src inside a fenced code block was validated as a repo path'
        page.write_text('# gutted\n')
        new_findings = set(check(root)) - baseline
        page.write_text(original)
        assert any('README.md' in f and 'floor' in f for f in new_findings), \
            'A README.md with no relative links escaped its ROOT_DOCUMENT_LINK_FLOORS entry'
        # The floor counts DISTINCT targets: a README that repeats one link five
        # times has one target and must trip the floor.
        page.write_text('# repeats\n' + '[a](CONTEXT.md) ' * 5 + '\n')
        new_findings = set(check(root)) - baseline
        page.write_text(original)
        assert any('README.md' in f and 'only 1 distinct' in f for f in new_findings), \
            'Repeated links to one target were counted as distinct'
        # Percent-encoded targets and one level of parentheses resolve (the
        # driver matrix links `AlpacaCore/conformu/ZWO/ASIair%20Plus%20(Pi%20CM4)/`);
        # an encoded link to a missing directory is still reported, decoded or not.
        (root / 'docs/fixture dir (x)').mkdir()
        (root / 'docs/fixture dir (x)/r.txt').write_text('r\n')
        page.write_text(original + '\n[ok](docs/fixture%20dir%20(x)/) [gone](docs/fixture%20dir%20(y)/)\n')
        new_findings = set(check(root)) - baseline
        page.write_text(original)
        assert not any('fixture%20dir%20(x)' in f for f in new_findings), \
            'A percent-encoded link with parentheses to an existing directory was reported as broken'
        assert any('README.md' in f and 'fixture%20dir%20(y)' in f for f in new_findings), \
            'A percent-encoded link to a missing directory escaped the link check'
        # SUPPORTED-DRIVERS.md is scanned by name with its own floor.
        matrix = root / 'SUPPORTED-DRIVERS.md'
        original_matrix = matrix.read_text(encoding="utf-8", errors="replace")
        matrix.write_text(original_matrix + '\n[gone](AlpacaCore/conformu/Nope/Model%20X/)\n')
        new_findings = set(check(root)) - baseline
        matrix.write_text('# gutted\n')
        floor_findings = set(check(root)) - baseline
        matrix.write_text(original_matrix)
        assert any('SUPPORTED-DRIVERS.md' in f and 'Nope/Model%20X' in f for f in new_findings), \
            'A broken link in SUPPORTED-DRIVERS.md escaped the link check'
        assert any('SUPPORTED-DRIVERS.md' in f and 'floor' in f for f in floor_findings), \
            'A gutted SUPPORTED-DRIVERS.md escaped its ROOT_DOCUMENT_LINK_FLOORS entry'
        # CHANGELOG.md has a floor too, so a gutted changelog is a finding.
        changelog = root / 'CHANGELOG.md'
        original_changelog = changelog.read_text(encoding="utf-8", errors="replace")
        changelog.write_text('# gutted\n')
        floor_findings = set(check(root)) - baseline
        changelog.write_text(original_changelog)
        assert any('CHANGELOG.md' in f and 'floor' in f for f in floor_findings), \
            'A gutted CHANGELOG.md escaped its ROOT_DOCUMENT_LINK_FLOORS entry'
        # A renamed skills tree loses ~10 documents, which fits inside the
        # aggregate floor's margin: only the per-source floor catches it.
        skills = root / '.claude/skills'
        skills.rename(root / '.claude/skillz')
        new_findings = set(check(root)) - baseline
        (root / '.claude/skillz').rename(skills)
        assert any('.claude/skills/' in f and 'floor' in f for f in new_findings), \
            'A renamed .claude/skills escaped the per-source floor'
    # A root with no Markdown documents to scan must trip the floor rather than
    # report a clean pass having read nothing.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / 'AlpacaCore/src/vendors').mkdir(parents=True)
        assert any('floor' in f for f in check(root)), 'An empty documents set escaped the floor'
    print('Instruction structure: glob assertions, negative fixtures, link path, root document links and floors passed')


if __name__ == '__main__':
    if '--self-test' in sys.argv:
        self_test()
    else:
        failures = check()
        print('\n'.join(failures) if failures else 'Instruction structure checks passed')
        sys.exit(bool(failures))
