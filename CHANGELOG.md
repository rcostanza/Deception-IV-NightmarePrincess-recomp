# Changelog

Each release's section becomes the text of its GitHub release (`tools/release_notes.py`, run by
`.github/workflows/build.yml` when a `v*` tag is pushed). Add the section before tagging: a tag
without one does not publish. Keep each entry on one line: GitHub shows a line break in release
text as a break.

## v0.4

- Sharpening: AMD FSR 1 RCAS over the picture, for crisper edges without halos. On by default; a new Sharpening row in Display Settings (F10), and **F9** switches it on or off during play to compare. The game's own HUD and menus are sharpened along with the scene.
- 144 FPS frame rate, between 120 and 180. A 144 or 165 Hz display now runs at 144 instead of 120.

## v0.3

- Aspect ratios: 21:9 and 32:9 show more of the level to the sides, 4:3 more above and below, nothing stretched. The menus stay 16:9.
- 30 FPS cap for PCs that cannot hold 60, at normal game speed.

## v0.2

- HDR setting: the game's own HDR10 output on an HDR display.
- 90 FPS frame rate (smooth on a 120 Hz display with VRR).
- Faster rendering where the game writes next to data the GPU has written.

## v0.1

- First release: Deception (PS4, CUSA00038 v1.10) running natively on Windows and Linux.
