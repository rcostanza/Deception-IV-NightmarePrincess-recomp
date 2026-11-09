# Development

How the runner works and how to work on it. Building, tests and packaging are in
[building.md](building.md).

## How it works

The PS4 and the PC are both x86-64, so nothing is recompiled. Deception's `eboot.bin` (KOEI TECMO's
engine, statically linked, with its KTSL2 sound library) is mapped into memory and run directly. What
differs is everything that code calls:

| The game calls | Replaced by |
|---|---|
| PS4 system libraries (kernel, POSIX, threads, files, pad, audio, save data, ...) | C reimplementations in `src/runtime_*.c` |
| GNM GPU command buffers | shadPS4's video core in `gpu/`, GCN shaders translated to Vulkan |

The game image is built from the dump by `run.py` (and by the setup in a release):

1. `scripts/prepare.py` strips the SELF wrapper from `eboot.bin` (the dump must be decrypted) and
   writes `out/game/boot.bin`, its report `analysis.json` and the AppContent profile `content.bin`.
2. `scripts/link.py` links the eboot with the two modules Deception ships in `sce_module`, `libc.prx`
   and `libSceFios2.prx`, into `boot-linked.bin`. Symbols match by NID and library identity, never by
   name; `mov rax, fs:[0]` thread-pointer loads are rewritten to GS (glibc owns FS on Linux).
3. On a CPU without an instruction set the game uses (MOVBE, LZCNT, BMI1), `scripts/isa_rewrite.py`
   lists each such instruction for `src/isa_rewrite.c`, which patches in a stub that emulates it.
4. `deception` maps the image, resolves every import against the C runtime (by symbol name, from
   `src/import_names.inc`) or the GPU core (by NID), runs each module's initializer and enters the
   game. An import with no implementation becomes a trap that prints
   `STOP: first unsupported PS4 import: <name>` and exits.

Every import Deception's modules make is implemented, and only those: a runtime function the
game does not import is not kept. `python run.py check` lists any that are missing (none today).

How the picture and the sound are made is below ([Render resolution and aspect ratio](#render-resolution-and-aspect-ratio), [Audio](#audio)).

## Running from the source tree

Set `DECEPTION_GAME_DIR` to your decrypted dump (or pass `--game DIR` each time):

```bash
python run.py --timeout 20
```

- Without a command, `run.py` prepares `out/game/` if the dump or `scripts/` changed and starts the
  game. The first `STOP:` or `Fault:` line it prints is the next thing to fix. `--timeout` ends the
  run after that many seconds.
- `python run.py check` lists the imports the runtime cannot resolve, by library (full list in
  `out/game/missing-imports.txt`). `python run.py prepare` rebuilds the image only.
- `--update DIR` (`DECEPTION_UPDATE_DIR`) lays the update dump over the game; `--dlc DIR`
  (`DECEPTION_DLC_DIR`) adds a DLC folder.
- `DECEPTION_POKE=<image offset>=<hex bytes>,...` writes bytes into the game image at start, for
  experiments (`src/runtime_game.c`).
- Saves go to `user/` (`--user DIR`), settings to `user/settings.ini`. Shader caches go to
  `shaders/` here, as a release keeps them next to its exe (`DECEPTION_GPU_USER_DIR` moves them).
- Every `settings.ini` key has an environment variable that overrides it, named in
  `settings.example.ini`. A variable left set in a shell keeps overriding the panel:
  `DECEPTION_VBLANK_HZ` prints a warning at start for that reason.
- `DECEPTION_VERBOSE=1` logs every routine event (threads, semaphores, memory maps, module loads).
  `run.py` and the debug package's `diagnose/start.bat` turn it on.
- Hang dump: once a frame has been shown, a picture held for `DECEPTION_HANG_DUMP` seconds (default 20,
  `0` turns it off) logs every thread's name, CPU time and frames (`Hang:` lines, innermost first).
  The `--timeout` watchdog prints the same list before it ends a run.

## Development loop

1. `python run.py --timeout 20` and read the first `STOP:` or `Fault:` line.
2. Implement the function in the matching `src/runtime_*.c` export table (`{"name",function}`),
   following the PS4 contract, with a comment that states it. Return "not available" for services a
   PC does not have (PSN, the camera, the companion app).
3. `bash build.sh`, run again.

When an import shows up as a bare NID, add candidate names to `tools/ps4_names.txt` and run
`python tools/name_imports.py out/game --write`: it hashes every candidate (and every name the
runtime already implements) and appends the matches to `src/import_names.inc`. For a fault in guest
code, `python tools/image_info.py flatten <module> out.bin` writes a module as a flat image to
disassemble at the offset the log names, and `python tools/image_info.py got <offset>` names the
import bound to a GOT slot.

## Frame rate

The game runs at 30 FPS, as on PS4: its logic advances one step per flip, and it flips every second
vblank of a 60 Hz video out. The engine also holds a dormant 60 FPS mode
(`DECEPTION_POKE=1f561=7f,1f903=7f,1f589=9090` turns it on: the frame interval it asks for becomes 1
and the branch that keeps 2 is skipped), but the logic does not scale its step with it: a two-second
walk covers twice the distance and the menus repeat keys twice as fast. So no frame rate setting is
offered; the vblank stays at 60 Hz (`GetVblankFrequency`), apart from the skip-intro boost below.

## Render resolution and aspect ratio

The game renders at 1920x1080 and has no PS4 Pro mode: it reads only the refresh rate from
`sceVideoOutGetResolutionStatus` and gives its display buffers and render targets fixed sizes
(the targets through 0x39ad10, most with sizes computed per pass). The runner reports a base PS4
(`sceKernelIsNeoMode` 0) and a 1920x1080 video out; `post_process.frag` scales the picture to the
window, and with `sharpening=1` (the default) FSR 1 RCAS (`rcas.frag`, `host_passes/rcas_pass.cpp`)
sharpens it at the window's size. A native 4K mode would need every render target and the UI's
1920-pixel layout scaled; that has not been done.

The picture is 16:9 only: the window's size list holds only 16:9 sizes, and the fullscreen modes
fit 16:9 into the screen (`DeceptionSettings::PickFullscreen`) with black bars on other shapes. An
`aspect_ratio` line from an older settings.ini is ignored. For future work: the 3D projection takes its aspect from one of ten 16:9 float constants in the image
(0x73b1b0, 0x73b1c4, 0x73b1c8, 0x876ab4, 0x891174, 0x893780, 0x8937b4, 0x894530, 0x895f60, 0x895f8c);
patched to 21:9 the scene shows more to the sides (squeezed into the 16:9 buffer) while the HUD
keeps its layout, so a real ultrawide mode would also need wider render targets.

The only overlay is the PS4's system UI (save list, text entry, trophy notices), the Display
Settings panel and the exit prompt (`gpu/shim/deception_overlay.cpp`). The last two are drawn like the
game's dialogs: a gold-framed black panel, the chosen row on an amber band between gold ornaments,
headings in Cinzel Bold and text in Josefin Slab SemiBold (free stand-ins for the game's Roman
capitals and slab serif; `gpu/third_party/fonts`). Scripted runs open them with the pad-file tokens
`display_settings` and `exit_prompt`.

## Audio

The game's sound is KOEI TECMO's KTSL2 mixer on top of Ngs2 (`src/runtime_ngs2.c`). Its racks are
"Sfx" (256 sampler voices playing ATRAC9 banks), "Stream" (12 voices of 16-bit 6-channel PCM),
"Movie", the effect racks ("Reverb", "DuckingTrig", "DuckingComp", "MultiFx") and "Mastering".
The runner mixes the sampler voices (pitch, envelope, port matrices) in the voices' patch order and
renders the mastering voices' input as 8-channel float; reverb, ducking and filters pass their
input through. Music (`BGM/BGMLink.g1l`) is ATRAC9 that the game decodes itself through
`sceAudiodec` (`src/runtime_audiodec.c`, one padded superframe per call) and streams in 512-frame
blocks to a Stream voice it starts before the first block arrives. `DECEPTION_NGS2_TRACE=1` (2 adds
state queries) and `DECEPTION_AUDIODEC_TRACE=1` log what the game asks.

## Starting, the title screen and leaving

With `skip_intro=1` (the default) `src/runtime_intro.c` plays the start as a hurried player would:
the vblank runs at 600 Hz through the KOEI TECMO and TECMO logos, Cross is tapped on the autosave
notice, and "Press ANY button" gets one Cross 3 s after its data loads, which opens the title menu
(NEW GAME / CONTINUE) for the player. Each screen is recognised by the archive entry the game reads
(`COMMON_WINDOW_G1T`, `TITLEMENU_G1T` in `FILE_PS4/archive00.lnk`, found by name in its `.h`). The
tap is single on purpose: a second one could choose a menu entry.

F4 asks "Exit Deception?" anywhere. The prompt is drawn by the overlay and takes the input while
open; Yes pushes the same quit event as closing the window. Holding Select (SDL's Back button) for
two seconds opens the Display Settings panel (F10 too): the button's own press is held back while it
is down, and a release before two seconds sends it to the game as a short tap.

## Update, save data, DLC and trophies

The update dump (`--update`, `DECEPTION_UPDATE_DIR`, `update=` in paths.ini) is laid over `/app0`
as the PS4 does: a file in the update replaces the game's, and directory listings merge both.

Saves are folders under `<save>/savedata/<user>/CUSA02859/` (`src/runtime_savedata.c`): the game
keeps `system0000` and `game-auto`. The save list dialog (`runtime_services.c`) lists them newest
first.

Add-ons are folders named after their label (`DLC0000000000001`, ...) in `<save>/addcont/` or in the
extra DLC folder (`--dlc`, `DECEPTION_DLC_DIR`; shadPS4's `dlc/CUSA02859` works as it is).
`sceAppContentGetAddcontInfo` reports them installed and `sceAppContentAddcontMount` serves each one's
files (`dlc/*.dat`) as `/addcont<n>` (`src/runtime_content.c`).

Trophies come from the dump's `sce_sys/trophy/trophy00.trp` (35); unlocks are kept in
`<save>/trophies.dat` and shown as a notice.

## Release folder and setup

`deception-setup.exe` (`setup/`) runs `prepare/run.py prepare` into `game/` beside it, rewrites the
game's instructions for this CPU if needed, writes `settings.ini` from the template when there is
none, sets its `skip_intro` line from the window's checkbox, and writes `paths.ini` with the game and
save folders and, when chosen, `update=` and `dlc=`. Nothing is written outside the release folder
and the save folder. The command line does the same as the window:
`deception-setup.exe --help`.

Started with no arguments, `deception.exe` reads `paths.ini` and builds the command line `run.py`
would pass (`src/launcher.c`); without a console its output goes to `log/deception.log` in the save
folder. It also watches the game: it starts the same command line as a child (on Windows in a job
that ends it if the launcher is killed, on Linux by `fork` with `PR_SET_PDEATHSIG`), and when the
game ends with an exit code other than 0 it notes the code in the log, copies the log to
`log/crash-<date>-<time>.log` and says so in a message box. `DECEPTION_CRASH_WATCH=0` turns that off.

Preparing the game needs Python 3.10 or newer with its standard library only; releases bundle
python.org's embeddable Python in `prepare/python`. The CPU rewrite also needs `iced-x86`, which only
CPUs missing an instruction set ever use. The icon of both exes is `res/deception.ico`
(`tools/make_app_icon.py` builds it from artwork).

## Input

The pad and keyboard layouts are fixed tables in `src/runtime_pad.c` (README "Controls"): gamepads map
by button position, keys by physical position. `deception --gamepad` prints the connected pad and its
labels.

Scripted input for automated runs: `DECEPTION_PAD_FILE` (held buttons and stick values, re-read when
the file changes), `DECEPTION_PAD_RECORD` (F9 records the pad) and `DECEPTION_PAD_REPLAY`.

## Layout

| Path | What it is |
|---|---|
| `src/` | Loader (`loader.c`), launcher (`launcher.c`), the runtime (`runtime_*.c`) and the CPU rewrite (`isa_*.c`) |
| `gpu/` | shadPS4's video core (`shadps4/`, changes marked `// deception:`), the shim and overlay around it (`shim/`), CMake build |
| `build.sh`, `build_windows.ps1`, `build_linux.sh` | The build (`build.sh`) and the local build, test and boot-check wrappers ([building.md](building.md)) |
| `scripts/` | Building the game image: `prepare.py`, `link.py`, `isa_rewrite.py` |
| `setup/` | `deception-setup`, which sets up a release folder from a player's dump |
| `res/` | The exes' icon, Windows resource script and manifests (`libs.manifest`: the DLL folder) |
| `packaging/` | `diagnose/`, the glitch kit the debug package ships |
| `.github/` | CI (`workflows/build.yml`) and its scripts: `ci-deps.sh` (Linux build dependencies), `smoke-test.sh` (a release on fresh distros) |
| `tools/` | `package.py` (the release), `name_imports.py` (names NIDs), `image_info.py` (modules and GOT slots), `make_app_icon.py`, `screenshot.ps1` |
| `tests/` | Python tests (`cd tests; python -m unittest`) and C/C++ tests (`build.sh --test`: its own list and every `*-test` in `gpu/CMakeLists.txt`) |
| `docs/` | This guide and [building.md](building.md) |
