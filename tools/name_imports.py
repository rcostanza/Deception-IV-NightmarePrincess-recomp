"""Name the game's unresolved imports and add them to src/import_names.inc.

The C runtime resolves imports by symbol name (runtime_lookup), and the name of an NID comes only
from src/import_names.inc. A NID is a hash of the name alone, so hashing candidate names finds them:
every name a runtime table already implements (`{"name",function}` in src/*.c) and the extra names
in tools/ps4_names.txt. A wrong candidate never matches, so the list may hold guesses.

Usage: python tools/name_imports.py OUT_DIR [--write]
OUT_DIR holds link.json from scripts/link_modules.py. Without --write it only prints the report.
"""
import argparse
import collections
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare import nid  # noqa: E402

INC = ROOT / 'src' / 'import_names.inc'
PAIR = re.compile(r'\{"([^"]+)","([^"]+)"\}')
TABLE_NAME = re.compile(r'\{"([A-Za-z_][A-Za-z0-9_]*)",\s*[A-Za-z_(]')


def implemented_names():
    names = set()
    for source in (ROOT / 'src').glob('*.c'):
        names.update(TABLE_NAME.findall(source.read_text(encoding='utf-8', errors='replace')))
    return names


def listed_names():
    path = ROOT / 'tools' / 'ps4_names.txt'
    lines = path.read_text(encoding='utf-8').splitlines() if path.is_file() else []
    return {line.split('#', 1)[0].strip() for line in lines} - {''}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out', type=Path)
    p.add_argument('--write', action='store_true', help='append the new names to src/import_names.inc')
    a = p.parse_args()

    known = dict(PAIR.findall(INC.read_text(encoding='utf-8')))
    implemented = implemented_names()
    by_nid = {nid(name): name for name in implemented | listed_names()}
    unresolved = __import__('json').loads((a.out / 'link.json').read_text())['unresolved_imports']

    added, unnamed = {}, collections.Counter()
    status = collections.defaultdict(collections.Counter)
    for key in unresolved:
        n, _, library = key.partition('#')
        name = known.get(key) or known.get(f'{n}#libkernel') or by_nid.get(n)
        if not name:
            unnamed[library] += 1
            status[library]['unnamed'] += 1
            continue
        # The runtime looks libScePosix names up under libkernel (runtime.c, canonical_name).
        inc_key = f'{n}#libkernel' if library == 'libScePosix' else key
        if inc_key not in known:
            added[inc_key] = name
        status[library]['implemented' if name in implemented else 'named'] += 1

    print(f'{len(unresolved)} imports left to the host runtime; {len(added)} newly named')
    print(f'{"library":32} {"implemented":>11} {"named only":>10} {"unnamed":>8}')
    for library, c in sorted(status.items(), key=lambda kv: -sum(kv[1].values())):
        print(f'{library:32} {c["implemented"]:11} {c["named"]:10} {c["unnamed"]:8}')
    if a.write and added:
        with INC.open('a', encoding='utf-8', newline='\n') as f:
            for key, name in sorted(added.items(), key=lambda kv: (kv[0].split('#')[1], kv[1])):
                f.write(f'{{"{key}","{name}"}},\n')
        print(f'Appended {len(added)} names to {INC.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
