"""tools/release_notes.py: a tag's section of CHANGELOG.md, the text of its GitHub release."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'tools'))
from release_notes import section  # noqa: E402

CHANGELOG = """# Changelog

Intro text.

## v0.2 (2026-11-09)

- Second.
  Wrapped line.

## v0.10

- Tenth.

## v0.1

- First.
"""


class SectionTests(unittest.TestCase):
    def test_section_runs_to_the_next_heading(self):
        self.assertEqual(section(CHANGELOG, 'v0.2'), '- Second.\n  Wrapped line.')

    def test_last_section_runs_to_the_end(self):
        self.assertEqual(section(CHANGELOG, 'v0.1'), '- First.')

    def test_tag_must_match_whole(self):
        self.assertEqual(section(CHANGELOG, 'v0.10'), '- Tenth.')
        self.assertIsNone(section(CHANGELOG, 'v0'))
        self.assertIsNone(section(CHANGELOG, 'v0.3'))

    def test_repository_changelog_has_a_section_for_each_tag(self):
        tags = subprocess.run(['git', 'tag', '-l', 'v*'], cwd=ROOT, capture_output=True, text=True)
        if tags.returncode != 0:
            self.skipTest('git is not available')
        text = (ROOT / 'CHANGELOG.md').read_text(encoding='utf-8')
        for tag in tags.stdout.split():
            self.assertTrue(section(text, tag), f'CHANGELOG.md has no section for {tag}')


class CommandTests(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(ROOT / 'tools' / 'release_notes.py'), *args],
                              capture_output=True, text=True, encoding='utf-8')

    def test_prints_the_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'CHANGELOG.md'
            path.write_text(CHANGELOG, encoding='utf-8')
            result = self.run_tool('v0.1', str(path))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '- First.\n')

    def test_missing_or_empty_section_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'CHANGELOG.md'
            path.write_text(CHANGELOG + '\n## v0.5\n\n', encoding='utf-8')
            self.assertEqual(self.run_tool('v0.4', str(path)).returncode, 1)
            self.assertEqual(self.run_tool('v0.5', str(path)).returncode, 1)


if __name__ == '__main__':
    unittest.main()
