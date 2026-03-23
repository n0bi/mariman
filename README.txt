Merry Man for Flipper Zero
==========================

What it is
----------
A simple Game Boy-like 1-bit side-scrolling platformer for Flipper Zero.
You can move left and right, jump, collect objects, stomp enemies, and avoid pits and spikes.

Controls
--------
- LEFT  : move left
- RIGHT : move right
- UP    : jump
- OK    : jump / start / retry
- BACK  : exit

Folder layout
-------------
Place this folder under:
applications_user/merry_man/

Files:
- application.fam
- merry_man.c
- icon.png
- README.txt

Build commands
--------------
./fbt fap_merry_man
./fbt build APPSRC=applications_user/merry_man
./fbt launch APPSRC=applications_user/merry_man

Notes
-----
- This version was rewritten to be stricter and safer in plain C.
- The main game state is stored in a fixed-size struct, not malloc'd by the app.
- Allocated Flipper resources (viewport, queue, mutex) are cleaned up on exit.
- The source was checked locally with a strict C syntax build using -Wall -Wextra -Werror against Flipper-style stubs.
- You still need a local Flipper firmware tree and toolchain to produce the real .fap.
