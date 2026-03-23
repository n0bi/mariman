Merry Man for Flipper Zero

Files
- application.fam
- merry_man.c
- icon.png

Controls
- LEFT / RIGHT: Move
- UP or OK: Jump
- BACK: Exit

Build
1. Place this folder in flipperzero-firmware/applications_user/merry_man/
2. Build with one of:
   ./fbt fap_merry_man
   ./fbt build APPSRC=applications_user/merry_man

Notes
- Simplified renderer for better performance
- Removed scrolling background decorations
- Smaller top HUD with distance and object count only
- Removed bottom range/goal text
