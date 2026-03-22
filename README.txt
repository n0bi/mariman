Merry Man - Flipper Zero external app

A Game Boy-style original side-scrolling platformer for Flipper Zero.

Controls
- Left: move left
- Right: move right
- Up or OK: jump
- Back: exit
- OK on title/game over: start or retry

Gameplay
- Traverse as far right as possible.
- Avoid pits, spikes, and roaming enemies.
- Stomp enemies from above to defeat them.
- Collect objects for bonus points.
- Rank goals increase with distance.

Folder layout
applications_user/
  merry_man/
    application.fam
    merry_man.c
    icon.png
    README.txt

Build examples
./fbt fap_merry_man
./fbt build APPSRC=applications_user/merry_man
./fbt launch APPSRC=applications_user/merry_man

Notes
- The icon is a 10x10 1-bit PNG as required for embedded FAP icons.
- The game uses original 1-bit art, not Nintendo sprites.
