#!/usr/bin/env python3
"""Check canonical instruction discovery and Claude adapters without an AI session."""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
# Tripwire for a renamed documents directory, not a target count: the tree has
# ~44 linked Markdown documents, so a scan that reads a handful means a glob or
# a root regressed and the link check is passing over nothing.
MIN_LINKED_DOCUMENTS = 20
# The skills tree is the one source whose entire contribution (~10 files) fits
# inside the aggregate floor's margin, so a renamed `.claude/skills` would scan
# nothing and still clear MIN_LINKED_DOCUMENTS. Tripwire, not a target count.
MIN_SKILL_DOCUMENTS = 5


def matches(path, pattern):
    # Unlike fnmatch, a single star must not cross a directory boundary.
    expression = re.escape(pattern).replace(r'\*\*/', '(?:.*/)?')
    expression = expression.replace(r'\*\*', '.*').replace(r'\*', '[^/]*')
    return re.fullmatch(expression, path) is not None


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
    for path in documents:
        for target in re.findall(r'\]\(([^\s)]+)\)', path.read_text(encoding="utf-8", errors="replace")):
            if target.startswith(('#', 'http:', 'https:', 'mailto:')):
                continue
            file = target.split('#', 1)[0]
            require((path.parent / file).exists(), '%s: broken relative link %s'
                    % (path.relative_to(root).as_posix(), target))
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
        for file in ('CLAUDE.md', 'AGENTS.md', 'CONTEXT.md'):
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
    print('Instruction structure: glob assertions, 4 negative fixtures, link path and 2 floors passed')


if __name__ == '__main__':
    if '--self-test' in sys.argv:
        self_test()
    else:
        failures = check()
        print('\n'.join(failures) if failures else 'Instruction structure checks passed')
        sys.exit(bool(failures))
