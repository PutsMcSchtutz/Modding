ScrollSpeed v3 EXPERIMENT for Crimson Desert

Goal: change the game's own MoveSpeedRate data table live (what the "Movement Speed Gains" /
"Direct Movement Speed" data mods change on disk), controlled by Ctrl + mouse wheel.
UNVERIFIED: whether the game re-reads that table while running. The log tells us.

TEST
 1. Copy ScrollSpeed.asi + ScrollSpeed.ini into bin64 (with your ASI loader). Remove older ScrollSpeed.asi.
 2. Start the game, LOAD A SAVE, wait for the auto-scan (~45 s) or press F6.
 3. Open ScrollSpeed.log (bin64) and read the lines starting "anchor at" and "scan done".
 4. Hold Ctrl and scroll up. Does your walk/run speed change? Try re-equipping an item or reloading
    the save if not. F8 / Ctrl+middle-click restore the original values.
 5. Send me the whole log and what you saw.
