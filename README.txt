ScrollSpeed v2 for Crimson Desert  (Ctrl + Mouse Wheel = PLAYER movement speed)

Only the controlled character's horizontal movement is scaled. Enemies, animations and
the world clock stay at normal speed.

INSTALL
 1. Needs an x64 ASI loader (winmm.dll or version.dll) in  Crimson Desert\bin64\
 2. Copy ScrollSpeed.asi and ScrollSpeed.ini into the same bin64 folder.
 3. In game: hold Ctrl and scroll. Multiplier shows in the window title. Ctrl+Middle click = reset.

TROUBLESHOOTING
 Set DebugLog=1 in the ini, relaunch, and read ScrollSpeed.log in bin64:
   "hook installed"            -> working
   "signature ... 0 match"     -> game updated; signature must be refreshed (send me the log + game version)
 Single-player only. Back up saves. Very high multipliers can clip through walls.
