"""Print a release's section of CHANGELOG.md, for the text of its GitHub release.

The section is the one headed `## <tag>` (anything after the tag on that line is allowed, such as a
date), up to the next `## ` heading. The CI publish job puts it above the download instructions.

Usage: python tools/release_notes.py TAG [CHANGELOG]
Exits 1 when the tag has no section or an empty one, so a release cannot go out without notes.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent


def section(text, tag):
    """The body of `## tag` in `text`, stripped; None when there is no such heading."""
    lines = None
    for line in text.splitlines():
        if line.startswith('## '):
            if lines is not None:
                break
            words = line[3:].split()
            if words and words[0] == tag:
                lines = []
        elif lines is not None:
            lines.append(line)
    return None if lines is None else '\n'.join(lines).strip()


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = Path(argv[2]) if len(argv) == 3 else ROOT / 'CHANGELOG.md'
    notes = section(path.read_text(encoding='utf-8'), argv[1])
    if not notes:
        print(f'{path.name} has no section for {argv[1]} (a "## {argv[1]}" heading with text under it)',
              file=sys.stderr)
        return 1
    sys.stdout.buffer.write((notes + '\n').encode('utf-8'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
