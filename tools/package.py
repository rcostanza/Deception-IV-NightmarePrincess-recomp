#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assembles the release folder (dist/) from the build, and optionally zips it.

    python tools/package.py [--out dist] [--python EMBEDDABLE] [--zip] [--debug]

On Linux the same folder holds deception and deception-setup (rpath $ORIGIN/lib), the
shared libraries they need in lib/ (ldd, less the host's: glibc, libstdc++, Vulkan, the display
and audio stacks, which must match its drivers), deception.png for the desktop entry and
prepare/ without Python: the setup runs the system's python3. --zip writes <release>-linux.tar.gz
(and -symbols, -debug), which keeps the files' exec bits and symlinks. Needs ldd, patchelf and strip.

The folder is what a player unpacks: they need neither this repo nor MSYS2. They run
deception-setup.exe, which prepares the game image from their own dump (prepare/run.py), and then
start deception.exe or the desktop shortcut it offers.

    deception.exe, deception-setup.exe (debug info stripped)
    libs/           the DLLs the game needs (from MSYS2 CLANG64) and libs.manifest, which names them:
                    a private assembly, which deception.exe's manifest depends on (res/libs.manifest)
    release.txt     this release; the setup stamps the image it prepares with it (src/launcher.c)
    README.txt, LICENSE.txt
    licenses/       the licenses of the fonts embedded in deception (DejaVu Sans, Cinzel, Josefin Slab)
    prepare/        run.py, scripts/ and settings.example.ini, run by the setup;
                    prepare/python/ holds Python for it when --python names the python.org
                    "embeddable package" (folder or .zip); otherwise the player needs Python 3.10+.

--debug packages for chasing a bug on someone's PC instead: the exes keep their debug info and
diagnose/ holds the glitch kit (packaging/diagnose: start.bat arms the frame capture,
optimization toggles and reload trigger). Its zip is <release>-debug.zip. A plain --zip also writes
<release>-symbols.zip with the unstripped exes, which resolve the exe+0x... offsets of a player's
deception.log (llvm-addr2line -fe deception.exe 0x140000000+offset).

What the setup adds later (game/, paths.ini, settings.ini) and what the kit writes (diagnose/
captures, its log) is left alone, so dist/ also works as the developer's own install. Never ship
game/: it is built from a player's own dump.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

PORT = Path(__file__).resolve().parent.parent
LINUX = sys.platform.startswith('linux')
EXES = ('deception', 'deception-setup') if LINUX else ('deception.exe', 'deception-setup.exe')
# Linux libraries the host provides: bundled, they break its GPU drivers, display connection or audio
# (libstdc++ too: Mesa needs the one it was built with, at least as new as this build's).
HOST_LIBS = re.compile(r'^(linux-vdso|ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl'
                       r'|libstdc\+\+|libgcc_s|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*'
                       r'|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor'
                       r'|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype'
                       r'|libnvidia-.*|libcuda|libz)\.so')
ICON = PORT / 'res' / 'deception.ico'
# What prepare/run.py needs: the scripts and the template of settings.ini.
PREPARE_FILES = ['run.py', 'settings.example.ini', *sorted(f'scripts/{p.name}' for p in (PORT / 'scripts').glob('*.py'))]
# The glitch kit that --debug ships in diagnose/.
DIAGNOSE = PORT / 'packaging' / 'diagnose'
# libs/libs.manifest in a release: res/libs.manifest's assembly, with the DLLs beside it.
LIBS_MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="libs" version="1.0.0.0" processorArchitecture="amd64"/>
{files}</assembly>
"""

README = """Deception IV: The Nightmare Princess on PC
Release {release}

You need a decrypted dump of your own copy of the PS4 game (CUSA02859): the folder with
eboot.bin, sce_module, sce_sys and FILE_PS4. Its update (version 1.01) and DLC dumps are optional;
shadPS4's <game>-patch and dlc/CUSA02859 folders work as they are. No game files are included.

1. Run deception-setup.exe. Pick the game folder (and its update and DLC folders, if you have
   them) and where your saves go, then press Install.
   It prepares the game from your dump (a few seconds) and offers a desktop shortcut.
2. Press Play Game, or later start deception.exe or the shortcut.

Run deception-setup.exe again to change the game or save folder. After unpacking a newer release
over this folder, run it once more: it keeps your folders and settings.

In the game: F10, or Select (Share/View) held for two seconds, opens the display settings (window,
resolution, VSync, sharpening); F4 asks to close the game.

settings.ini (made by the setup) holds the window, audio and control options; one in your save
folder is read instead, and the display settings save there. If the game stops, the reason is in
deception.log, in the log folder of your save folder. Shader caches are kept in the shaders
folder here.

Needs Windows 10/11 x64, a CPU with AVX and a GPU with Vulkan 1.3.{python_note}
Source and license (GPL-2.0-or-later): see LICENSE.txt; the fonts' licenses are in licenses.
{debug_note}"""
LINUX_README = """Deception IV: The Nightmare Princess on PC (Linux)
Release {release}

You need a decrypted dump of your own copy of the PS4 game (CUSA02859): the folder with
eboot.bin, sce_module, sce_sys and FILE_PS4. Its update (version 1.01) and DLC dumps are optional;
shadPS4's <game>-patch and dlc/CUSA02859 folders work as they are. No game files are included.

1. Run deception-setup (from a file manager, or ./deception-setup in a terminal). Pick the
   game folder and where your saves go. It prepares the game from your dump (a few seconds) and
   offers an entry in your application menu (Steam's "Add a Non-Steam Game" lists it too).
   From a terminal it also takes: ./deception-setup --game <dump> [--update <folder>]
   [--dlc <folder>] [--save <folder>]
2. Start the game from the menu entry, or run ./deception.

Run deception-setup again to change the game or save folder. After unpacking a newer release
over this folder, run it once more: it keeps your folders and settings.

In the game: F10, or Select (Share/View) held for two seconds, opens the display settings (window,
resolution, VSync, sharpening); F4 asks to close the game.

settings.ini (made by the setup) holds the window, audio and control options; one in your save
folder is read instead, and the display settings save there. If the game stops, the reason is in
deception.log, in the log folder of your save folder. Shader caches are kept in the shaders
folder here.

Needs Linux x86-64 with glibc 2.39 or newer (SteamOS 3.7, Ubuntu 24.04, Debian 13, Fedora 40 or
newer), python3 3.10+ for the setup, a CPU with AVX and a GPU with a Vulkan 1.3 driver.
Source and license (GPL-2.0-or-later): see LICENSE.txt; the fonts' licenses are in licenses.
"""
PYTHON_NOTE = '\nThe setup needs Python 3.10 or newer (python.org) to prepare the game.'
DEBUG_NOTE = """
This is a debugging build. To catch a rendering glitch, start the game with diagnose\\start.bat
(its log goes to diagnose\\deception.log). While the glitch shows, run 1-capture.bat, then
2-optimizations-off.bat, then 3-reload.bat, noting after each whether the glitch changed. Restart
the game afterwards, and send the diagnose folder (log and captures).
"""


def pe_imports(path):
    """The DLLs a PE file imports, normal and delay-loaded."""
    data = path.read_bytes()
    fh = struct.unpack_from('<I', data, 0x3C)[0] + 4
    sections, opt_size = struct.unpack_from('<H', data, fh + 2)[0], struct.unpack_from('<H', data, fh + 16)[0]
    opt = fh + 20
    if struct.unpack_from('<H', data, opt)[0] != 0x20B:
        raise SystemExit(f'{path}: not a 64-bit PE file')
    table = opt + opt_size

    def offset(rva):
        for i in range(sections):
            vsize, va, raw, ptr = struct.unpack_from('<4I', data, table + i * 40 + 8)
            if va <= rva < va + max(vsize, raw):
                return ptr + rva - va
        raise SystemExit(f'{path}: bad import table')

    names = []
    # Import descriptors are 20 bytes with the name at +12; delay-load descriptors 32 with it at +4.
    for directory, size, name_at in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from('<I', data, opt + 112 + directory * 8)[0]
        if not rva:
            continue
        at = offset(rva)
        while (name := struct.unpack_from('<I', data, at + name_at)[0]):
            start = offset(name)
            names.append(data[start:data.index(b'\0', start)].decode('ascii'))
            at += size
    return names


def dll_closure(exe, bin_dir):
    """The DLLs `exe` needs from MSYS2, and theirs. What Windows has (System32, API sets) stays
    Windows': vulkan-1.dll is the driver's, as when run.py starts the game, also on a build machine
    without a GPU driver (CI), where System32 has none."""
    system = Path(os.environ.get('SYSTEMROOT', r'C:\Windows')) / 'System32'
    queue, seen, found, missing = list(pe_imports(exe)), set(), [], []
    while queue:
        name = queue.pop(0)
        key = name.lower()
        if (key in seen or key.startswith(('api-ms-', 'ext-ms-')) or key == 'vulkan-1.dll'
                or (system / name).exists()):
            continue
        seen.add(key)
        source = bin_dir / name
        if not source.is_file():
            missing.append(name)
            continue
        found.append(source)
        queue += pe_imports(source)
    if missing:
        raise SystemExit(f'Not in {bin_dir}: {", ".join(missing)} (packages in docs/building.md)')
    return found


def linux_tool(*names):
    for name in names:
        if (path := shutil.which(name)):
            return path
    raise SystemExit(f'Need {names[0]} (.github/scripts/ci-deps.sh installs it).')


def so_closure(exes):
    """The shared libraries `exes` load, theirs included (ldd lists them all), less the host's."""
    found, missing = {}, []
    for exe in exes:
        listing = subprocess.run(['ldd', str(exe)], capture_output=True, text=True, check=True).stdout
        for name, path in re.findall(r'^\s*(\S+)\s+=>\s+(\S+)', listing, re.MULTILINE):
            if HOST_LIBS.match(name):
                continue
            if path == 'not':
                missing.append(name)
            else:
                found[name] = Path(path)
    if missing:
        raise SystemExit(f'ldd cannot find {", ".join(sorted(set(missing)))}')
    return [found[name] for name in sorted(found)]


def set_rpath(path, rpath):
    """Bundled libraries find each other through their own RUNPATH: the exe's applies to its direct
    dependencies only."""
    subprocess.run([linux_tool('patchelf'), '--set-rpath', rpath, str(path)], check=True)


def min_glibc(paths):
    """The newest GLIBC_ symbol version the files use: the oldest glibc they start on."""
    objdump = shutil.which('objdump') or shutil.which('llvm-objdump-18')
    if not objdump:
        return None
    versions = set()
    for path in paths:
        dump = subprocess.run([objdump, '-T', str(path)], capture_output=True, text=True).stdout
        versions |= set(re.findall(r'GLIBC_(2\.[0-9.]+)', dump))
    return max(versions, key=lambda v: tuple(map(int, v.split('.'))), default=None)


def ico_png(ico):
    """The largest PNG frame in a .ico (the desktop entry's icon)."""
    data = ico.read_bytes()
    frames = []
    for i in range(struct.unpack_from('<H', data, 4)[0]):
        width, _, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        if data[offset:offset + 8] == b'\x89PNG\r\n\x1a\n':
            frames.append((width or 256, data[offset:offset + length]))
    if not frames:
        raise SystemExit(f'{ico}: no PNG frame')
    return max(frames, key=lambda frame: frame[0])[1]


def release_name(product='deception-pc'):
    """<product>-<commit>-<date>, or -<commit>-dirty-<date>-<time> for uncommitted work:
    what release.txt and the zip are named after. Two releases from one dirty tree differ by the
    packaging time, so the launcher asks for the setup again after either is unpacked."""
    def git(*args):
        return subprocess.run(['git', *args], cwd=PORT, capture_output=True, text=True).stdout.strip()
    described = git('describe', '--tags', '--always', '--dirty') or 'unknown'
    when = time.strftime('%Y-%m-%d-%H%M') if described.endswith('-dirty') else git('log', '-1', '--format=%cs')
    return f'{product}-{described}-{when}'


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def strip(source, target, bin_dir, mode='--strip-all'):
    """Copies an exe without its debug info (RelWithDebInfo DWARF is ~95% of deception.exe).
    Crash logs give exe+offset, which the unstripped build resolves."""
    target.parent.mkdir(parents=True, exist_ok=True)
    tool = linux_tool('llvm-strip-18', 'llvm-strip', 'strip') if LINUX else str(bin_dir / 'llvm-strip.exe')
    subprocess.run([tool, mode, '-o', str(target), str(source)], check=True)


def embeddable_python(source, target):
    """Unpacks or copies the python.org embeddable package to target (python.exe at its root), the
    setup's."""
    shutil.rmtree(target, ignore_errors=True)
    if source.suffix.lower() == '.zip':
        with zipfile.ZipFile(source) as z:
            z.extractall(target)
    else:
        shutil.copytree(source, target)
    if not (target / 'python.exe').is_file():
        raise SystemExit(f'{source}: no python.exe (expected the embeddable package from python.org)')
    # Its ._pth file fixes sys.path, without the running script's folder: the setup's scripts import
    # each other (from prepare import ...), so prepare/scripts goes on it.
    for pth in target.glob('python*._pth'):
        lines = pth.read_text().splitlines()
        extra = [line for line in ('../scripts',) if line not in lines]
        pth.write_bytes(('\r\n'.join([*extra, *lines]) + '\r\n').encode())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, help='the folder to fill (default dist)')
    p.add_argument('--python', type=Path, help='Python to ship with the setup: the python.org embeddable package (folder or .zip), Windows')
    p.add_argument('--zip', action='store_true', help='also write out/<release>.zip (and <release>-symbols.zip)')
    p.add_argument('--debug', action='store_true', help='keep debug info in the exes and ship the diagnose/ kit')
    a = p.parse_args()
    bin_dir = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
    if LINUX and a.python:
        raise SystemExit('--python is the Windows embeddable package; on Linux the setup runs the system python3.')
    for exe in EXES:
        if not (PORT / 'out' / exe).is_file():
            raise SystemExit(f'out/{exe} not found: build first (bash build.sh).')
    out = (a.out or PORT / 'dist').resolve()
    release = release_name()
    shipped = []

    def ship(source, name):
        copy(source, out / name)
        shipped.append(name)

    programs = {exe: PORT / 'out' / exe for exe in EXES}
    for name, source in programs.items():
        if a.debug:
            ship(source, name)
        else:
            strip(source, out / name, bin_dir)
            shipped.append(name)
        if LINUX:
            set_rpath(out / name, '$ORIGIN/lib')
    if LINUX:
        libs = so_closure(list(programs.values()))
        wanted = {lib.name for lib in libs}
        for old in (out / 'lib').glob('*') if (out / 'lib').is_dir() else ():
            if old.name not in wanted:
                old.unlink()
        for lib in libs:
            target = out / 'lib' / lib.name
            if a.debug:
                copy(lib, target)
            else:
                strip(lib, target, bin_dir, '--strip-unneeded')
            set_rpath(target, '$ORIGIN')
            shipped.append(f'lib/{lib.name}')
        (out / 'deception.png').write_bytes(ico_png(ICON))
        shipped.append('deception.png')
        dlls = libs
    else:
        # The kit's own files come and go with --debug; its captures and log stay.
        for bat in sorted(DIAGNOSE.glob('*.bat')):
            if a.debug:
                ship(bat, f'diagnose/{bat.name}')
            else:
                (out / 'diagnose' / bat.name).unlink(missing_ok=True)
        # The DLLs go in libs/ (older releases had them beside the exes).
        dlls = dll_closure(PORT / 'out' / EXES[0], bin_dir)
        wanted = {dll.name.lower() for dll in dlls}
        for old in out.glob('*.dll'):
            old.unlink()
        for old in (out / 'libs').glob('*.dll'):
            if old.name.lower() not in wanted:
                old.unlink()
        for dll in dlls:
            ship(dll, f'libs/{dll.name}')
        files = ''.join(f'  <file name="{dll.name}"/>\n' for dll in dlls)
        (out / 'libs' / 'libs.manifest').write_bytes(LIBS_MANIFEST.format(files=files).replace('\n', '\r\n').encode())
        shipped.append('libs/libs.manifest')

    prepare = out / 'prepare'
    python = prepare / 'python'
    for item in prepare.iterdir() if prepare.is_dir() else ():
        if item != python:
            shutil.rmtree(item) if item.is_dir() else item.unlink()
    for name in PREPARE_FILES:
        ship(PORT / name, f'prepare/{name}')
    if a.python:
        embeddable_python(a.python, python)
    if python.is_dir():
        shipped += [str(f.relative_to(out)).replace('\\', '/') for f in sorted(python.rglob('*')) if f.is_file()]

    (out / 'release.txt').write_bytes(f'{release}\n'.encode())
    if LINUX:
        (out / 'README.txt').write_bytes(LINUX_README.format(release=release).encode())
    else:
        (out / 'README.txt').write_bytes(README.format(release=release, python_note='' if python.is_dir() else PYTHON_NOTE,
                                                       debug_note=DEBUG_NOTE if a.debug else '')
                                         .replace('\n', '\r\n').encode())
    ship(PORT / 'LICENSE', 'LICENSE.txt')
    # The overlay's embedded fonts: their licenses go with them.
    for font_license in sorted((PORT / 'gpu' / 'third_party' / 'fonts').glob('LICENSE-*.txt')):
        ship(font_license, f'licenses/{font_license.name}')
    shipped += ['release.txt', 'README.txt']
    size = sum((out / name).stat().st_size for name in shipped)
    if LINUX:
        glibc = min_glibc([out / name for name in shipped if name in programs or name.startswith('lib/')])
        print(f'{out}: {release}{" (debug)" if a.debug else ""}, {len(dlls)} libraries, {len(shipped)} files, '
              f'{size / 2**20:.0f} MiB, needs glibc {glibc or "?"}')
    else:
        print(f'{out}: {release}{" (debug)" if a.debug else ""}, {len(dlls)} DLLs, {len(shipped)} files, {size / 2**20:.0f} MiB'
              + ('' if python.is_dir() else ' (no Python: the player needs Python 3.10+)'))

    if a.zip and LINUX:
        # tar keeps the exec bits a zip would lose.
        archive = PORT / 'out' / f'{release}-linux{"-debug" if a.debug else ""}.tar.gz'
        with tarfile.open(archive, 'w:gz', compresslevel=9) as t:
            for name in sorted(shipped):
                t.add(out / name, f'{release}/{name}')
        print(f'{archive}: {archive.stat().st_size / 2**20:.0f} MiB')
        if not a.debug:
            symbols = PORT / 'out' / f'{release}-linux-symbols.tar.gz'
            with tarfile.open(symbols, 'w:gz', compresslevel=9) as t:
                for exe in EXES:
                    t.add(PORT / 'out' / exe, exe)
            print(f'{symbols}: {symbols.stat().st_size / 2**20:.0f} MiB (not for players)')
    elif a.zip:
        archive = PORT / 'out' / f'{release}{"-debug" if a.debug else ""}.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name in sorted(shipped):
                z.write(out / name, f'{release}/{name}')
        print(f'{archive}: {archive.stat().st_size / 2**20:.0f} MiB')
        if not a.debug:
            symbols = PORT / 'out' / f'{release}-symbols.zip'
            with zipfile.ZipFile(symbols, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for exe in EXES:
                    z.write(PORT / 'out' / exe, exe)
            print(f'{symbols}: {symbols.stat().st_size / 2**20:.0f} MiB (not for players)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
