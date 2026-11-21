# Playing

How to build the game's Mac app: [README](../README.md#how-to-play).

- **Settings:** a window before the game starts sets frame rate, console mode (docked or handheld), language, rumble and more. Click **Play**; your choices are remembered. What each one does: [Settings](settings.md).
- **Saves and logs:** the save is in `~/Library/Application Support/nxnative/<title ID>/save/`, the log is `~/Library/Logs/nxnative/<title ID>.log` (the previous run is `<title ID>.prev.log`). When something goes wrong, send the log.

## Controllers

A controller that Apple's controller support lists (an Xbox or PlayStation controller, a Switch Pro Controller) is a player as soon as it connects.

A controller macOS pairs but Apple's controller support does not list, such as an 8BitDo in its own mode (Ultimate 2C Wireless over Bluetooth), is read as a raw HID gamepad, with full analog sticks: it becomes a player at its first button press (a device that only declares a gamepad, such as a keyboard receiver, never presses one and so takes no player), and the log says `controller '<name>' (HID) connected: press a button to play`, then `a button pressed: player N`. The press that joins also reaches the game. A pad Apple's support says it will list gets two seconds to appear there first, so it is never read twice (an Xbox, PlayStation or Nintendo controller is always left to Apple's support); if it never appears, it is read as HID after that wait, and the log says so. Its buttons follow 8BitDo's numbering by default; another maker's pad may number them differently, and the log then says so. `DM_HID_MAP=A=1,B=2,X=4,Y=5` in `env.txt` moves them (here: the printed letters instead of the positions), `DM_HID_LOG=1` logs the number of each button pressed, and `DM_HID_PADS=0` turns this off.

Two identical controllers that Apple's controller support lists as one, such as two Pro Controllers over USB, are each a player of their own: after 3 seconds the log says `GameController lists 1 of 2`, and each takes a player, by USB port. They have no vibration, player lights or Home button to quit. `DM_PAD_IDENTICAL=0` in `env.txt` turns this off.

With [Press a button to join](settings.md#press-a-button-to-join) on in the settings (`DM_PAD_PRESS_TO_JOIN=1` in `env.txt`), a controller becomes a player only when one of its buttons is pressed, as the lowest player free: the players choose their order and how many play, over Bluetooth or USB, identical pads included, and a controller connected while the game runs joins at its first press. The log says `press a button to play`, then `a button pressed: player N`. The press that joins also reaches the game. When a game asks for more players than it sees, a prompt over the game says how many are wanted and joined and waits for the others: a controller that presses a button (or connects, with the setting off) joins, A finishes once enough are in, and B or Escape closes it, as on a Switch.

To quit with a controller, hold its Home button (the logo button on an Xbox or PlayStation controller) for 2 seconds, then press A to quit or B to go back to the game. Quitting this way is the same as ⌘Q: progress the game has not saved is lost. If holding Home opens the Games app instead, macOS keeps the Home button for itself: its controller settings for the Home button decide whether apps may use it. `DM_HOME_QUIT=0` in `env.txt` turns the hold off.

The mouse pointer hides when you press a key or a controller button, or move a stick, while it is over the game picture. Moving the mouse shows it again, as does switching to another window or app.

## Switch 2 controllers

Joy-Con 2 and the Pro Controller 2 do not appear in macOS's Bluetooth settings: they speak Nintendo's own Bluetooth LE protocol. Turn on **Switch 2 controllers** in the game's settings window ([Switch 2 controllers](settings.md#switch-2-controllers)), then:

1. **Pair a controller once, in the game.** While the game runs, hold the controller's small Sync button until its player lights sweep. The game pairs it and it plays; the log says `paired with this Mac`. A controller paired this way forgets the console it was paired with: pair it there again to use it with one.
2. **From then on, press any button on it** while the game runs with the setting on: it wakes and connects, no Sync needed. A controller woken for a Switch 2 nearby is left alone.

**Pairs and singles.** A left and a right Joy-Con 2 that connect play together as one pair. To make one of a pair a player of its own, held sideways, hold its **SL and SR** (the small rail buttons) for three seconds; to pair two single Joy-Cons again, hold **L** on the left one and **R** on the right one together for three seconds. The player lights show the new player numbers. A game that asks for more players than there are controllers splits a pair itself, as on a Switch. The game sees Joy-Cons as Joy-Cons: a pair is a dual controller with a sensor in each Joy-Con, a single one a sideways Joy-Con with its own sensor and rail buttons; a game that knows no Joy-Cons reads them as a Pro Controller. The gyro works, and so does rumble: each Joy-Con of a pair plays its side, a Joy-Con alone plays both, and **Game rumble**, **Rumble strength** and **Rumble cutoff** apply. Hold **Home** for two seconds for the quit prompt, as on other controllers. The app asks for Bluetooth access once. An app packed again asks again: macOS ties the answer to the app's signature, and the packing script signs each build anew, so the same bundle ID does not carry it over. On a Pro Controller 2, pairing, every button, the sticks and Home are checked; its gyro scale and axes are borrowed from the Joy-Con 2 and the Pro Controller, not measured on it.

## Rumble

Game vibration plays on a connected controller that has it. Controllers with separate left/right haptic outputs keep those effects separate. The settings window's **Game rumble**, **Rumble strength** and **Rumble cutoff** control it ([Controls](settings.md#controls)). It approximates the Switch's effects: Apple's haptics cannot reproduce the Switch's exact waveforms.

## Motion (gyro)

A controller that supplies gyro data through Apple's controller support (a DualSense, a DualShock 4 over USB) drives the game's motion controls. Where the motion data macOS gives for a pad is empty (a Switch Pro Controller, a DualShock 4 over Bluetooth), the host reads the sensor from the controller's own reports instead (`DM_MOTION_HID=0` in `env.txt` turns that off). Joy-Con 2 and Pro Controller 2 gyro come over Bluetooth LE ([Switch 2 controllers](#switch-2-controllers)).

`DM_MOTION=0` in `env.txt` stops all motion. If motion misbehaves, pack with `--no-motion` (`MOTION = False` in Spyder) for an app without it; `DM_MOTION=1` in `env.txt` turns it back on.

## Game language

English by default. Pick another when packing with `--language` (`-l`), by code or by name: `-l ja`, `-l zh-Hans`, `-l French` (`LANGUAGE` in Spyder). When the NUP's control data lists the game's languages, the script prints them and stops on one the game lacks. To change it without rebuilding, pick another in the settings window.

## Settings in a file

Lines like `DM_OPMODE=0` (handheld mode: lower resolution) in `~/Library/Application Support/nxnative/<title ID>/env.txt` win over the settings window, which then shows them greyed out ([the full list](settings.md#settings-in-a-file)). `DM_FPS_TARGET=30` runs the game at 30 fps instead of 60, which uses less power. `DM_LAUNCHER=0` skips the settings window.

## Miis and console files

Mii heads and your own Miis come from your console, not from this project. Copy them from your own console into `~/Library/Application Support/nxnative/system`:

- `sysdata/<system data ID>/`: a system data archive's files, for example `sysdata/0100000000000802/` (the Switch's Mii heads) or `sysdata/0400000000000849/` (a Switch 2 game's);
- `save/8000000000000030/MiiDatabase.dat`: your console's Miis, listed before the six default ones. The file is only read.

Without them, Miis have no heads, and only the six default Miis are listed ([details](reference/services.md#a-consoles-own-mii-database-and-system-data)). When a game asks you to make a Mii, it gets the default Mii, kept in `system/host/` ([details](reference/services.md#a-mii-made-in-the-editors-place)).

## Shaders on first sight

Short stutters the first time a new scene appears are normal: the app compiles graphics for it once and remembers them for the next launch. What it remembered, and what the game's list names, is built before the game starts, behind "Preparing shaders" and a progress bar when that takes more than half a second.
