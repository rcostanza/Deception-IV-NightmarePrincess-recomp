# Development

How the runner works and how to work on it. Building, tests and packaging are in
[building.md](building.md).

## How it works

The PS4 and the PC are both x86-64, so nothing is recompiled. Deception's `eboot.bin` (Housemarque's
engine, statically linked, with Wwise for audio) is mapped into memory and run directly. What
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

Every import Deception's three modules make is implemented, and only those: a runtime function the
game does not import is not kept. `python run.py check` lists any that are missing (none today).

The renderer draws at the game's own resolution (`render_resolution=4k`, the default: PS4 Pro mode,
`sceKernelIsNeoMode` returns 1 and the video out is 3840x2160; or 1080p) and scales the picture to the
window in `post_process.frag`. With `sharpening=1` (the default) that pass draws into an image of the
window's size and FSR 1 RCAS (`rcas.frag`, `host_passes/rcas_pass.cpp`) sharpens it into the frame.
The only overlay is the PS4's system UI (save list, text entry, trophy notices), the Display Settings
panel and the exit prompt (`gpu/shim/deception_overlay.cpp`).

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
- `--guest-arg SWITCH` passes one of the engine's own command-line switches (strings in the eboot:
  `-nosplash`, `-no_video`, `-no_level_intro`, ...).
- Saves go to `user/` (`--user DIR`), settings to `user/settings.ini`. Shader caches go to one folder
  per PC, `%LOCALAPPDATA%\Deception` (`~/.cache/deception`), shared with installed copies
  (`DECEPTION_GPU_USER_DIR` moves it).
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

Deception's game logic advances once per frame, by a step its frame clock fixes: the boot passes the
constant 59.94 and allows no frame skip, so the game's speed is the rate at which it flips. Raising
the PS4 vblank alone (shadPS4's approach) runs the game faster, not smoother.

`frame_rate` (30, 60, 90, 120, 144 or 180) sets both. `DeceptionSettings::EffectiveFrameRate()` lowers it to what the
display refreshes at, unless VSync is off with tearing allowed; the video out's vblank follows it
(`GetVblankFrequency`, live), and at each flip `runtime_game_frame_rate()` (`src/runtime_game.c`)
calls the engine's own `set_fps(rate x 0.999)` through the frame clock's vtable (the global at image
+0x152cd28, vtable image+0xc6c2c8, slot 3), after checking the vtable is the one it expects. A change
from the panel applies on the next flip. Above about 200 FPS a guard in the spawner (the 0.005 at
0x94283c) stops enemies spawning, so higher rates are not offered.
At 30 the vblank stays at 60: with an engine target of 30 the game calls `sceVideoOutSetFlipRate(1)`
(a flip every second vblank, as the PS4's 30 FPS games do), and back to 0 when the target rises
again, live switches included; a 30 Hz vblank under that ran it at 15.

After a rate change the present thread paces by its timer instead of following the display: with
G-Sync, following the display's own timing settled at 97 FPS.

### CPU writes next to GPU-written bytes

In combat the game's CPU writes records (eboot 0x1c90a7, a copy loop at 0x6c4bf8, CRT copies) into
pages it shares with small GPU-written buffers: the 5 KiB destination of a compute copy (shader
0x069f1335) and a 64 KiB storage buffer (0xf003dbab). Page tracking is per 4 KiB page, so each
such write fault used to download the GPU's bytes first, a `scheduler.Finish()` that waits for the
whole queue: about 160 a second, 4 ms of GPU idle per frame on a Radeon 890M (73 to 79 FPS at
1080p). The writes almost never touch a byte the GPU wrote, so `BufferCache::TryMergeWrite` lets
them go ahead without downloading and keeps a copy of the page's GPU-written bytes; the page's next
upload (`EmitUploadRange`) leaves out those that the guest has not changed since. Relaxed readback
mode only (the default), where pages are never read-protected. Toggle `WriteMerge` (bit 50) turns
it off; `DECEPTION_FRAME_STATS=1` prints the merged writes per second, `DECEPTION_PERF_DIAG=1` the
downloads that remain with the guest instruction and the GPU writer behind each.

### Render resolution

The engine's output size table (1920x1080 on PS4, 3840x2160 on PS4 Pro; eboot 0xa5d120) sets only
the display buffers and the final pass: patched to 1280x720, the scene's passes still render at
1920x1080 for the same GPU time, so there is no 720p mode. The scene's sizes are fixed in the
render scripts in the game data: each target is created by name with its size (`mrt_albedo`
1920x1080, or 1920x2160 checkerboard in PS4 Pro mode; `ssao_*` at half; `menu_overlay` for the pause
menu). `DECEPTION_VERBOSE=1` lists the ones a wide picture widens.

### Aspect ratios (4:3, 21:9, 32:9)

The `aspect_ratio` setting picks the picture's shape: in windowed mode the window has it (its size
list holds only sizes of that ratio), and the fullscreen modes fit it into the screen
(`DeceptionSettings::PickFullscreen`: borderless the desktop, exclusive the largest mode of the ratio,
bars otherwise). A shape other than 16:9 is made at start: `DeceptionSettings::WideWidth4k` turns it
into a width at 2160 lines (2880 for 4:3, 5120 for 2560x1080, 7680 for 32:9; 3840 for 16:9). These
things make the engine use it:

- the video out is that wide (half in 1080p mode; `emulator_settings.h`). The engine takes its
  aspect ratio from the video out's size (with the camera correction below, a wider picture keeps
  the PS4's vertical view and shows more to the sides, Hor+; 4:3 keeps its horizontal view and
  shows more above and below, Vert+);
- `runtime_game_widen` (src/runtime_game.c) writes it into the size table, so the display buffers
  match;
- the same function puts a jmp to a stub at the start of the engine's three create functions
  (0x476aa0 render targets, 0x478840 and 0x478910 depth buffers). The stub widens a 16:9 or 8:9
  (checkerboard) size by width/3840 (narrows it at 4:3) and runs the function's own 17-byte prologue. The main menu's
  cards (`menu_slide_*`, `dummy_depth*`) keep their size;
- the engine's perspective has a vertical half-angle of tan(fov/2)/sqrt(1+aspect) (fitted to the
  main menu's and gameplay camera's matrices at 16:9, 21:9 and 32:9), so on its own a wider picture
  also sees less above and below (10% at 21:9, 28% at 32:9), pushing the HUD and the menus, drawn
  in 3D in front of the camera, past the top and bottom. Every camera passes its projection
  through 0x420bd0 (this, view, projection, fov, near, far, aspect); for perspective cameras of the
  picture's aspect a stub passes a copy with 16:9's vertical scale (4:3: its horizontal scale,
  the smaller zoom of the two) and the fov that gives it;
- at 4:3 the screen's orthographic cameras (x within +-aspect, y +-1) zoom out to 16:9's width:
  the pause menu is laid out for 16:9 and would lose its sides. All twelve are set once at boot
  from the same call (0x4226b2), so the main menu's flat layer and backdrop zoom out with it.

The main menu's backdrop only covers the middle 16:9, so the menus show dark sides (dark bands
above and below at 4:3), and the pause menu darkens only the middle 16:9; the HUD stays within
16:9's frame too. The shape is set for the run: a changed aspect ratio shows "(after restart)" on
the Display Settings page.

## Starting, the title screen and leaving

With `skip_intro=1` (the default) the loader passes `-nosplash`, then presses Cross once when the
title screen appears: the AvPlayer opening `title.mp4` marks it (`runtime_pad_title_movie`,
`src/runtime_pad.c`). A start lands in the main menu.

On the title screen, Back (Circle, B, Escape) asks "Exit Deception?" (`title_back_exits=1`); F4 asks
anywhere. The prompt is drawn by the overlay and takes the input while open; Yes pushes the same
quit event as closing the window. Holding Select (SDL's Back button) for two seconds opens the
Display Settings panel (F10 too): the button's own press is held back while it is down, and a release
before two seconds sends it to the game as a short tap.

## Save data, DLC and trophies

Saves are folders under `<save>/savedata/<user>/CUSA00038/` (`src/runtime_savedata.c`); Deception keeps
one, `savedata`. The save list dialog (`runtime_services.c`) lists them newest first.

Deception's 22 add-ons (ships and skins) are entitlement-only: the game asks
`sceAppContentGetAddcontInfo` about each label it knows, and an add-on is installed when a folder of
that name exists in `<save>/addcont/` or in the extra DLC folder (`--dlc`, `DECEPTION_DLC_DIR`;
shadPS4's `addcont` with its `CUSA00038` folder works as it is). Nothing is read from those folders.

Trophies come from the dump's `sce_sys/trophy/trophy00.trp` (65, TROP000-064); unlocks are kept in
`<save>/trophies.dat` and shown as a notice.

## Release folder and setup

`deception-setup.exe` (`setup/`) runs `prepare/run.py prepare` into `game/` beside it, rewrites the
game's instructions for this CPU if needed, writes `settings.ini` from the template when there is
none, sets its `skip_intro` line from the window's checkbox, and writes `paths.ini` with the game and
save folders and, when one is chosen, `dlc=`. The command line does the same as the window:
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
