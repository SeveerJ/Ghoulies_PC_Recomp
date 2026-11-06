# Known issues

- **Busy rooms:** rooms with many enemies (the mummy room, the fire imps) can drop below 60 fps on many PCs. The game keeps the right speed. The default 30 fps holds steady.
- **High settings:** Supersampling 2x or Maximum draws the game at up to 2880 lines and is very heavy. It can cause lag and pauses of a second or more, even on a fast graphics card. If the game stutters, turn Supersampling off first.
- **New rooms:** the first visit to a room can stutter for a short while as its shaders are built. Later visits are smooth.
- **Low-end PCs and laptops:** 4-core laptops are well below 60 fps at higher resolutions. Keep the defaults.
- **FXAA and supersampling:** while supersampling is on, the FXAA choices have no effect (supersampling already smooths the edges).
- **Speakers:** a change to Stereo / 5.1 takes effect the next time the game starts.
- **Death effect:** a box can show around enemies' death particles; up close it covers the screen.
- **Quit prompt:** the Yes/No text may be missing.
- **First load:** start-up can occasionally hang on the first loading screen. Close the game and start it again.
- **Bonus books:** the game has no list of which rooms' books you have found.
- **Rare crash:** one crash in a full playthrough could not be reproduced. A guard for its likely cause is in this build. If it happens, send `session_log.txt`.
- **SmartScreen:** Windows may warn that Setup is from an unknown publisher, because it is not code-signed. Choose "More info", then "Run anyway".
