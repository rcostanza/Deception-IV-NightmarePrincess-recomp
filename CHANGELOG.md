# Changelog

Each release's section becomes the text of its GitHub release (`tools/release_notes.py`, run by
`.github/workflows/build.yml` when a `vN.N` tag such as `v0.1` is pushed). Add the section before
tagging: a tag without one does not publish. Keep each entry on one line: GitHub shows a line break
in release text as a break.

## v0.1

- First release: Deception IV: The Nightmare Princess (PS4, CUSA02859 v1.01) running natively on Windows and Linux.
- The game's update and DLC from your own dumps; the setup takes the game, update, DLC and save folders, in its window or on the command line.
- Sound effects and music.
- Skip intro: a start lands on the title menu (NEW GAME / CONTINUE) in about 6 seconds.
- Windowed, borderless and fullscreen; 720p to 4K window sizes; FSR 1 RCAS sharpening.
- Display settings (F10, or hold Select) and the exit prompt (F4) drawn like the game's own dialogs.
- Shader caches are kept in `shaders` next to the game, nothing in `%APPDATA%`.
