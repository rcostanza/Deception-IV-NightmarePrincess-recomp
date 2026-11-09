# Deception PC runner

Native Windows and Linux runner for the PS4 version of Deception IV: The Nightmare Princess
(CUSA02859, update v1.01; KOEI TECMO's engine statically linked into eboot.bin, KTSL2 sound over
Ngs2, data in FILE_PS4 archives).
The game's x86-64 code runs directly; PS4 system calls are reimplemented in C and GNM rendering goes
through shadPS4's video core on Vulkan. See [docs/development.md](docs/development.md) for the
pipeline and [docs/building.md](docs/building.md) for the toolchain and packaging. No game files
are in the repository, and nothing assumes where the decrypted dump is: set `DECEPTION_GAME_DIR`
to it (or pass `--game DIR` to `run.py`), and `DECEPTION_UPDATE_DIR` to its update (`--update DIR`). Tests that need it skip without it.

## Loop
- Build: `bash build.sh` (Git Bash on Windows, where it puts MSYS2 CLANG64 on PATH itself; or Linux).
- Run: `python run.py --timeout 20`; read the first `STOP:` or `Fault:` line.
- Missing imports: `python run.py check` (full list in `out/game/missing-imports.txt`).
- Tests: `bash build.sh --test` (runtime C tests and every CMake `*-test`), then
  `cd tests && python -m unittest discover -p "test_*.py"`. `build_windows.ps1 -Test -Run` /
  `build_linux.sh --test --run` do the build, both suites and a boot check in one go.

## Conventions
- Names are Deception's: identifiers, files, env variables (`DECEPTION_*`), log prefixes and
  patch markers in vendored code (`// deception:`).
- C runtime imports resolve by symbol name: a function goes in a `src/runtime_*.c` export table
  (`{"name",function}`) and its NID in `src/import_names.inc`. Name NIDs with
  `python tools/name_imports.py out/game --write` (candidates in `tools/ps4_names.txt`).
- `libScePosix` imports are looked up under the `libkernel` key (runtime.c, `canonical_name`).
- POSIX variants return errno; `sce*` variants return SCE codes (0x8002xxxx, errno in the low
  16 bits). See `posix_result` in runtime_mutex.c.
- The loader builds with `-Wall -Wextra -Werror`. Match the surrounding terse C style and comment
  each function with the PS4 contract it follows.
- Guest-visible pointers must stay below 1 TiB (`runtime_low_alloc`).
- Keep only what this game needs: no extra menus or hacks for other games. The renderer
  is shadPS4's with added performance work (draw preparation, memos, async pipelines); behaviour
  changes there need a boot check (`python run.py --timeout 20`).
- Releases are built by `tools/package.py` and must work from the unpacked zip alone: no repo, no
  MSYS2, no Python of the player's own. Never package game files or anything built from a dump.
- Python edits on Windows: write files with LF line endings (`write_bytes`, or
  `open(path, 'w', newline='')`): text mode would turn the tree's LF into CRLF.
- DLC: 19 add-ons (`DLC000...` folders, each with `dlc/*.dat` and sce_sys); sceAppContent reports
  them installed and mounts each as /addcont<n> (src/runtime_content.c).
- The game is held at its native 30 FPS: its logic steps once per flip, so a 60 FPS mode runs it at
  double speed. It renders 1920x1080 only (no Pro mode) and 16:9 only: the window takes 16:9 sizes.
- Shader caches live in `shaders/` (repo, or next to the release exe); write nothing under
  %APPDATA%/%LOCALAPPDATA% or the XDG folders.
- `DECEPTION_POKE=<offset>=<hex>,...` patches the image at start for experiments (offsets as in
  `llvm-objdump -d out/game/eboot.elf`).
- Scripted test runs: use a copy of the save folder (`run.py --user <copy>`): stray presses on the
  title menu can start NEW GAME over the user's save.
- `skip_intro=1` (settings.ini, default): src/runtime_intro.c runs the vblank fast and taps Cross
  through the logos and the autosave notice, then one Cross on "Press ANY button", keyed on the
  archive entries the game reads; a start lands on the title menu (NEW GAME / CONTINUE).
