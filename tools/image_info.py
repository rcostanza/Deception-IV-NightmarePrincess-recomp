"""Look inside the game image when a Fault: or STOP: line points at guest code.

    python tools/image_info.py got OFFSET...
        Names the import bound to each GOT slot (eboot offsets, hex) in out/game/boot.bin.
    python tools/image_info.py flatten MODULE OUT.bin [NAME...]
        Writes a module (SELF .prx/.elf, or eboot.bin) as a flat image of its PT_LOAD and SCE_RELRO
        segments at their vaddrs, prints its program headers, then every relocation whose symbol
        contains one of NAME (NIDs). A guest offset minus the module's base (out/game/link.json) is an
        offset into OUT.bin: disassemble it with llvm-objcopy -I binary and llvm-objdump, or load it
        into Ghidra as raw x86-64 at base 0.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from link import module  # noqa: E402


def got(offsets):
    data = (ROOT / 'out/game/boot.bin').read_bytes()
    size, entry, ns, nr, ni, caps = struct.unpack_from('<QQQQQQ', data, 8)
    pos = 56 + ns * 24
    names = [data[pos + i * 128:pos + (i + 1) * 128].split(b'\0')[0].decode() for i in range(ni)]
    pos += ni * 128
    symbols = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', (ROOT / 'src/import_names.inc').read_text()))
    slots = {}
    for i in range(nr):
        target, kind, value, addend = struct.unpack_from('<QQqq', data, pos + i * 32)
        if kind:
            slots[target] = names[value]
    for offset in offsets:
        nid = slots.get(int(offset, 16))
        print(offset, nid, symbols.get(nid, '') if nid else '')


def flatten(path, out, names):
    m = module(path)
    loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
    flat = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        flat[p['vaddr']:p['vaddr'] + p['filesz']] = m['elf'][p['offset']:p['offset'] + p['filesz']]
        print(f"load vaddr={p['vaddr']:#x} filesz={p['filesz']:#x} memsz={p['memsz']:#x} flags={p['flags']}")
    Path(out).write_bytes(flat)
    for target, kind, sym, addend in m['relocs']:
        name = m['symbols'][sym]['name'] if sym < len(m['symbols']) else ''
        if any(k in name for k in names):
            print(f'reloc {target:#x} type={kind} {name}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    g = sub.add_parser('got', help='name the imports bound to GOT slots')
    g.add_argument('offsets', nargs='+')
    f = sub.add_parser('flatten', help='write a module as a flat image')
    f.add_argument('module')
    f.add_argument('out')
    f.add_argument('names', nargs='*')
    a = p.parse_args()
    if a.command == 'got':
        got(a.offsets)
    else:
        flatten(a.module, a.out, a.names)


if __name__ == '__main__':
    main()
