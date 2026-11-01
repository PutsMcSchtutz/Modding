ScrollSpeed for Crimson Desert  (Ctrl + Mouse Wheel = game speed)

BUILD (no PC setup needed):
 1. Create a free GitHub repo, upload this whole folder (keep .github/ intact).
 2. Open the Actions tab -> "build" -> download the ScrollSpeed artifact (contains ScrollSpeed.asi + .ini).
 Or locally (Visual Studio 2022):  cmake -B build -A x64 && cmake --build build --config Release

INSTALL:
 1. Put an x64 Ultimate ASI Loader (as winmm.dll or version.dll) in  Crimson Desert\bin64\
 2. Copy ScrollSpeed.asi and ScrollSpeed.ini into the same bin64 folder.
 3. In game: hold Ctrl and scroll. Speed shows in the window title. Ctrl+Middle click resets to 1x.

LIMITS: scales the whole game clock (enemies/animations too), not just Kliff's movement.
