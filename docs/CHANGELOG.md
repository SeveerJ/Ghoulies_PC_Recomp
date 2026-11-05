# Changelog

## Playtest build 2 (3 Nov)

- Fixed: random crashes in some areas (the shower among them). The game's main stack ran into the scratch stack the sound and timer interrupts use; they now have their own memory.
- Fixed: the steamy bathroom doorway was a black box. Two texture modes it uses (dot product / dot ST) are now emulated.
- Fixed: stretched shadows smeared across the floor. Textures that use "border" edges now get their border colour.
- Fixed: the flickering ice and spikes in the freezer room.
- New: a 30 fps option (as on the original Xbox) in the launcher and the F1 menu. The game keeps the right speed at either rate.
- New: more anti-aliasing choices: FXAA quality, high and ultra, as well as the fast FXAA. They follow edges further and smooth thin details more.
- New: a supersampling setting (1.5x, 2x or maximum): the game is drawn above your screen's resolution and shrunk to fit, for the cleanest edges.
- New: 5.1 surround sound (Speakers setting), as in Orta PC.
- Fixed: a rare crash after long sessions (a damaged contact list is now skipped instead of read).
- PCs with 4 cores or fewer use one background shader worker, which avoids long freezes on laptops.
- The launcher shrinks a little to fit smaller or high-DPI screens.
- Fast shaders for each new room are built in the background on spare cores, so new rooms slow down for a shorter time.

## First playtest build

- Recompiled Grabbed by the Ghoulies (US) runs on PC, with the Orta PC toolkit changes.
- Real-time frame timing, so the game runs at the right speed at 60 fps.
- Flips are presented straight away instead of waiting for the next vblank, which helps hold 60 fps.
- Anti-aliasing: smooth scaling and FXAA (F1 menu and settings.ini).
- Fixed: black screen after a zombie kill (the depth-replace texture mode is now emulated).
- Fixed: crash when a challenge is failed and the Reaper appears.
- Fixed: blocky Bink videos on loading screens and in cutscenes (a 16-bit `movsx` from `bp`/`sp` was translated wrongly).
- Setup.exe builds the game from your own disc on your own PC.
