ScrollSpeed v5 - Ctrl + mouse wheel = stick limit (controller required)

The mod scales your left-stick values before the game reads them, like pushing the stick less far.
It can slow you down from full range; it cannot make you faster than a fully pushed stick.

INSTALL: copy ScrollSpeed.asi + ScrollSpeed.ini into bin64 (one ASI loader only: winmm.dll OR version.dll).
Delete older ScrollSpeed.* files first.

USE: connect your controller, walk with the stick pushed fully, hold Ctrl and scroll down.
     Speed should drop smoothly. Ctrl+middle click resets. Or: hold Back/View + D-Pad Up/Down.

LOG (bin64\ScrollSpeed.log) - send it if something is off:
  "hooked XInputGetState in ..."   -> hook installed
  "[10s] ... calls=N, scaled=M"     -> calls>0 means the game polls the controller through the hook;
                                       scaled>0 means stick values were reduced.
