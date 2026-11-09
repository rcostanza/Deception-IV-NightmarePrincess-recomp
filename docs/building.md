# Building from source

Everything builds on Windows 10/11 x64 with MSYS2's CLANG64 toolchain, and on Linux x86-64 with
clang. The same steps run in CI (`.github/workflows/build.yml`), so a green run there is a working
recipe.

## 1. Toolchain

Install [MSYS2](https://www.msys2.org) to `C:\msys64` (`winget install MSYS2.MSYS2`; another place
works with `MSYS2_ROOT` set to it), open the **MSYS2 CLANG64** shell and install the toolchain and
libraries:

```bash
pacman -S --needed git patch make \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,zlib,zydis,nasm} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

You also need Python 3.10 or newer on Windows itself (python.org) for `run.py`, the tests and the
packaging, and [Git for Windows](https://git-scm.com) for Git Bash.

## 2. Source

Clone the repository with its submodules (imgui, magic_enum, miniz and xbyak, under
`gpu/third_party`):

```bash
git clone --recurse-submodules <repository URL>
```

`build.sh` fetches the submodules itself if the clone was made without them. The first build also
downloads FFmpeg's source (9.0.2, checked against its SHA-256) and builds the small static FFmpeg
the runner links into `out/ffmpeg` (`tools/build_ffmpeg.sh`): only the H.264, AAC and PNG decoders,
the MP4 demuxer and the file protocol, which is all Deception's movies and the save icons need. That
takes a few minutes once; later builds reuse it, and it is rebuilt when its version or options
change. `nasm` gives it its x86 assembly (without it, decoding is slower).

## 3. Build

From Git Bash (or the MSYS2 shell) at the repository root:

```bash
bash build.sh
```

It writes `out/deception.exe` (loader, runtime and GPU library in one exe) and `out/deception-setup.exe`.
Both are RelWithDebInfo builds and carry their debug info (about 120 MB of the game exe) for
debuggers and `llvm-addr2line`. A failed build prints its last lines; the full log is
`out/gpu-build.log`.

Tests:

```bash
bash build.sh --test
```

```bash
cd tests && python -m unittest discover -p "test_*.py"
```

`--test` builds and runs the runtime's C tests and every `*-test` target of `gpu/CMakeLists.txt`
(the renderer's C++ tests and the rest of the runtime's), from the repository root; a failed one
prints the end of its log, `out/<name>-test.log`. Tests that need the game dump, a Vulkan device or
`iced-x86` skip themselves when those are missing.

### Convenience scripts

For local work, `build_windows.ps1` (PowerShell) and `build_linux.sh` wrap the steps above:

```powershell
.\build_windows.ps1 -Test -Run -Game <dump>
```

```bash
./build_linux.sh --test --run --game <dump>
```

Without options they build. `-Test`/`--test` also runs the Python tests, `-Run`/`--run` starts the
game for `-Timeout`/`--timeout` seconds (20 by default; `DECEPTION_GAME_DIR` names the dump when
`-Game`/`--game` is left out) and prints its first `STOP:` or `Fault:` line, with the full log in
`out/boot.log`. `-Package`/`--package` writes the release (section 5), and `-Clean`/`--clean`
removes `out/` first. On Windows the script finds Git Bash (or MSYS2's bash) itself; on Linux it
picks `clang-18`-style compiler names when there is no plain `clang`.

### Linux

The same commands build `out/deception` (linked without PIE: host pointers handed to the game must stay
below 1 TiB) and `out/deception-setup`, and run the same tests. On Ubuntu 24.04 (also in WSL),
`.github/scripts/ci-deps.sh` installs the dependencies as root, building Vulkan-Headers, VMA and SDL3,
which 24.04 lacks or ships too old. Then:

```bash
CC=clang-18 CXX=clang++-18 bash build.sh --test
```

In WSL the game runs on Mesa's software Vulkan (llvmpipe): slow, but enough for a boot check.

## 4. Run from the source tree

Point `run.py` at your decrypted dump (CUSA02859: `eboot.bin`, `sce_module`, `sce_sys`,
`FILE_PS4`) with `--game DIR` or `DECEPTION_GAME_DIR`, and at its update (version 1.01) with
`--update DIR` or `DECEPTION_UPDATE_DIR`:

```bash
python run.py --game <dump> --update <dump>-patch --timeout 20
```

`run.py` prepares `out/game/` from the dump when needed and starts the game with the MSYS2 DLLs on
`PATH`. [development.md](development.md) covers its other commands.

## 5. Package

There are two kinds of package. Both hold what a player needs without this repo or MSYS2: the game
and setup programs, the libraries they load (on Windows found by walking their imports in
`C:\msys64\clang64\bin`), `prepare/` for the setup, a README and the license. Neither ever contains
game files.

On Windows the DLLs sit in `libs\`, not beside the exes. Windows only looks there because
`deception.exe`'s manifest depends on a private assembly named `libs`, and `libs\libs.manifest`
(written by the packager) lists the DLLs. The build puts a copy of `res/libs.manifest`, which lists
none, in `out\libs\`, so a development build still finds the MSYS2 DLLs on `PATH`. Without some
`libs\libs.manifest` beside it, `deception.exe` does not start ("side-by-side configuration is
incorrect").

| | Release (for players) | Debug (for chasing a bug on someone's PC) |
|---|---|---|
| Command | `python tools/package.py --python <embed.zip> --zip` | `python tools/package.py --python <embed.zip> --zip --debug --out dist-debug` |
| Exes | debug info stripped (game exe ~7 MB) | as built, with debug info |
| `diagnose/` | not included | glitch kit from `packaging/diagnose` |
| Zip | `out/<release>-windows-x64.zip` | `out/<release>-debug.zip` |

`<embed.zip>` is the python.org "Windows embeddable package (64-bit)", for example
`https://www.python.org/ftp/python/3.15.1/python-3.15.1-embed-amd64.zip`; check its SHA-256 against
the python.org release page. It becomes `prepare/python`, so players need no Python of their own.
Without `--python` the setup uses the player's Python 3.10+.

On Linux, `python3 tools/package.py --zip` writes `out/<release>-linux-x64.tar.gz` (also `-linux-symbols`, and
`-linux-debug` with `--debug`): the game, the setup, the libraries they need in `lib/` less the host's
(glibc, libstdc++, Vulkan, X11/Wayland, audio) and `prepare/`; the setup runs the system's python3.
`.github/scripts/smoke-test.sh <unpacked release>` checks it as root on a fresh distro (libraries,
the setup on non-dumps, the start from `paths.ini`); the `linux-smoke` CI job runs it on Ubuntu
24.04, Debian 13, Fedora 42 and Arch.

The folder (`dist/` by default, `--out DIR` otherwise) is also a working install: run its setup once
and it plays. Packaging into it again keeps what the setup and the diagnose kit wrote there
(`game/`, `paths.ini`, `settings.ini`, captures, logs).

### Symbols for release crashes

A release `--zip` also writes `out/<release>-symbols.zip` with the unstripped programs of that build;
it is not for players. A crash in a player's `deception.log` reads like `deception.exe+0x1234ab`; resolve
it with the symbols of the same release (named in the player's `release.txt`), adding the image base
0x140000000:

```bash
llvm-addr2line -fCe deception.exe 0x1401234ab
```

### The debug kit

`diagnose/start.bat` starts the game with the frame capture, the renderer optimization toggles and
the cache reload armed (idle until used) and logs to `diagnose/deception.log`. While a glitch is on
screen the player runs `1-capture.bat`, `2-optimizations-off.bat` and `3-reload.bat`, then sends the
`diagnose` folder. The debug package's README.txt says so.

## 6. CI and releases

Pushing a tag `v*` runs `.github/workflows/build.yml` (so do pull requests and a manual run from the
Actions tab; pushes to branches do not): on `windows-latest`, the build, both test suites and both
packages (with the pinned embeddable Python); the `linux` and `linux-smoke` jobs do the same for Linux
on `ubuntu-24.04`. The packages are the run's artifacts (Actions tab, kept 90 days). A tag run that
passes also publishes the Windows zip and the Linux tarball as a GitHub release named after the tag.
The release's text is the tag's section of `CHANGELOG.md` (`## v0.4` up to the next `## `), then
the install instructions. Write the section first: a tag without one fails the `notes` job at once
and does not publish. `python tools/release_notes.py v0.4` prints what the release will say.

```bash
python tools/release_notes.py v0.1.0   # the section is there
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```
