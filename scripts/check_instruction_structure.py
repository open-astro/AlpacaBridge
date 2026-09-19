#!/usr/bin/env python3
"""Check canonical instruction discovery and Claude adapters without an AI session."""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


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
    documents = canonical + [root / 'docs/agent-instructions.md']
    documents += list((root / 'docs/failures').glob('*.md'))
    documents += list((root / 'docs/decisions').glob('*.md'))
    documents += list((root / '.claude/skills').rglob('*.md'))
    for path in documents:
        if not path.is_file():
            continue
        for target in re.findall(r'\]\(([^\s)]+)\)', path.read_text(encoding="utf-8", errors="replace")):
            if target.startswith(('#', 'http:', 'https:', 'mailto:')):
                continue
            file = target.split('#', 1)[0]
            require((path.parent / file).exists(), '%s: broken relative link %s' % (path.name, target))
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
                          'AlpacaCore/include/alpacacore/vendor', 'AlpacaCore/tests'):
            shutil.copytree(ROOT / directory, root / directory)
        for file in ('CLAUDE.md', 'AGENTS.md'):
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
    print('Instruction structure: glob assertions and 4 negative fixtures passed')


if __name__ == '__main__':
    if '--self-test' in sys.argv:
        self_test()
    else:
        failures = check()
        print('\n'.join(failures) if failures else 'Instruction structure checks passed')
        sys.exit(bool(failures))
