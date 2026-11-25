# Playing

How to build the game's Mac app: [README](../README.md#how-to-play).

- **Settings:** a window before the game starts sets frame rate, console mode (docked or handheld), language, rumble and more. Click **Play**; your choices are remembered. What each one does: [Settings](settings.md).
- **Reset:** Game > Reset Game (⌘R) starts the game again from its first screen, as closing it and opening it again does on a Switch: progress the game has not saved is lost. It takes under a second, with no settings window and no shader preparation.
- **Saves and logs:** the save is in `~/Library/Application Support/NativeNavi/<title ID>/save/`, the log is `~/Library/Logs/NativeNavi/<title ID>.log` (the previous run is `<title ID>.prev.log`). A save made with an nxnative build of the game, in `~/Library/Application Support/nxnative/<title ID>/`, is copied over at the first start, with that app's settings; the old copy stays. When something goes wrong, send the log.

## Controllers

A controller that Apple's controller support lists (an Xbox or PlayStation controller, a Switch Pro Controller) is a player as soon as it connects.

A controller macOS pairs but Apple's controller support does not list, such as an 8BitDo in its own mode (Ultimate 2C Wireless over Bluetooth), is read as a raw HID gamepad, with full analog sticks: it becomes a player at its first button press (a device that only declares a gamepad, such as a keyboard receiver, never presses one and so takes no player), and the log says `controller '<name>' (HID) connected: press a button to play`, then `a button pressed: player N`. The press that joins also reaches the game. A pad Apple's support says it will list gets two seconds to appear there first, so it is never read twice (an Xbox, PlayStation or Nintendo controller is always left to Apple's support); if it never appears, it is read as HID after that wait, and the log says so. Its buttons follow 8BitDo's numbering by default; another maker's pad may number them differently, and the log then says so. `DM_HID_MAP=A=1,B=2,X=4,Y=5` in `env.txt` moves them (here: the printed letters instead of the positions), `DM_HID_LOG=1` logs the number of each button pressed, and `DM_HID_PADS=0` turns this off.

A wired Xbox One controller from another maker than Microsoft, such as a PDP Wired Controller, has no macOS driver: macOS sees it on USB, but its light stays off and Apple's controller support never lists it. The host reads it itself: it powers the pad on as Linux does, and the pad is a player from its connection; the log says `controller '<name>' (USB, Xbox One, …): no macOS driver has it: read here`, then `(USB) connected: player N`. Its buttons go by position, as an Xbox controller's do through Apple's support, and its Xbox button is Home. It has no vibration yet. A pad macOS's own Xbox driver takes (Microsoft's) is left to it. `DM_GIP_PADS=0` in `env.txt` turns this off.

Two identical controllers that Apple's controller support lists as one, such as two Pro Controllers over USB, are each a player of their own: after 3 seconds the log says `GameController lists 1 of 2`, and each takes a player, by USB port. They have no vibration, player lights or Home button to quit. `DM_PAD_IDENTICAL=0` in `env.txt` turns this off.

With [Press a button to join](settings.md#press-a-button-to-join) on in the settings (`DM_PAD_PRESS_TO_JOIN=1` in `env.txt`), a controller becomes a player only when one of its buttons is pressed, as the lowest player free: the players choose their order and how many play, over Bluetooth or USB, identical pads included, and a controller connected while the game runs joins at its first press. The log says `press a button to play`, then `a button pressed: player N`. The press that joins also reaches the game. When a game asks for more players than it sees, a prompt over the game says how many are wanted and joined and waits for the others: a controller that presses a button (or connects, with the setting off) joins, A finishes once enough are in, and B or Escape closes it, as on a Switch.

To quit with a controller, hold its Home button (the logo button on an Xbox or PlayStation controller) for 2 seconds, then press A to quit or B to go back to the game. Quitting this way is the same as ⌘Q: progress the game has not saved is lost. If holding Home opens the Games app instead, macOS keeps the Home button for itself: its controller settings for the Home button decide whether apps may use it. `DM_HOME_QUIT=0` in `env.txt` turns the hold off.

The mouse pointer hides when you press a key or a controller button, or move a stick, while it is over the game picture. Moving the mouse shows it again, as does switching to another window or app.

**Match the labels on an Xbox controller.** The game uses the Switch's button positions, so an Xbox pad's right-hand B presses the game's A. To play by the printed Xbox letters, connect the controller, open **System Settings → Game Controllers**, select it, and in its button settings map **A to B**, **B to A**, **X to Y** and **Y to X**, leaving the other buttons on **Standard**. The change applies to other apps too. The Home quit prompt reads macOS's own A and B, so its buttons may differ from the game's. [Apple's controller settings guide](https://support.apple.com/en-us/111099)

## Switch 2 controllers

Joy-Con 2 and the Pro Controller 2 do not appear in macOS's Bluetooth settings: they speak Nintendo's own Bluetooth LE protocol. Turn on **Switch 2 controllers** in the game's settings window ([Switch 2 controllers](settings.md#switch-2-controllers)), then:

1. **Pair a controller once, in the game.** While the game runs, hold the controller's small Sync button until its player lights sweep. The game pairs it and it plays; the log says `paired with this Mac`. A controller paired this way forgets the console it was paired with: pair it there again to use it with one.
2. **From then on, press any button on it** while the game runs with the setting on: it wakes and connects, no Sync needed. A controller woken for a Switch 2 nearby is left alone.

**Pairs and singles.** A left and a right Joy-Con 2 that connect play together as one pair. To make one of a pair a player of its own, held sideways, hold its **SL and SR** (the small rail buttons) for three seconds; to pair two single Joy-Cons again, hold **L** on the left one and **R** on the right one together for three seconds. The player lights show the new player numbers. A game that asks for more players than there are controllers splits a pair itself, as on a Switch. The game sees Joy-Cons as Joy-Cons: a pair is a dual controller with a sensor in each Joy-Con, a single one a sideways Joy-Con with its own sensor and rail buttons; a game that knows no Joy-Cons reads them as a Pro Controller. The gyro works, and so does rumble: each Joy-Con of a pair plays its side, a Joy-Con alone plays both, and **Game rumble**, **Rumble strength** and **Rumble cutoff** apply. Hold **Home** for two seconds for the quit prompt, as on other controllers. The app asks for Bluetooth access once. An app packed again asks again: macOS ties the answer to the app's signature, and the packing script signs each build anew, so the same bundle ID does not carry it over. On a Pro Controller 2, pairing, every button, the sticks and Home are checked; its gyro scale and axes are borrowed from the Joy-Con 2 and the Pro Controller, not measured on it.

## Rumble

Game vibration plays on a connected controller that has it. Controllers with separate left/right haptic outputs keep those effects separate. The settings window's **Game rumble**, **Rumble strength** and **Rumble cutoff** control it ([Controls](settings.md#controls)). It approximates the Switch's effects: Apple's haptics cannot reproduce the Switch's exact waveforms.

## Motion (gyro)

A controller that supplies gyro data through Apple's controller support (a DualSense, a DualShock 4 over USB) drives the game's motion controls. Where the motion data macOS gives for a pad is empty (a Switch Pro Controller, a DualShock 4 over Bluetooth, an 8BitDo Ultimate 2 Wireless over Bluetooth with firmware 1.03 or later), the host reads the sensor from the controller's own reports instead (`DM_MOTION_HID=0` in `env.txt` turns that off). Joy-Con 2 and Pro Controller 2 gyro come over Bluetooth LE ([Switch 2 controllers](#switch-2-controllers)).

A Switch Pro-compatible controller whose thumbstick stays at zero in Apple's controller support while it moves in its own reports gets that stick from the reports after a short tilt; its buttons, vibration, motion and any working stick stay with Apple's support ([details](reference/services.md#switch-pro-controller-stick-stays-zero-in-gamecontroller)). `DM_PRO_STICKS=0` in `env.txt` turns that off.

`DM_MOTION=0` in `env.txt` stops all motion. If motion misbehaves, pack with `--no-motion` (`MOTION = False` in Spyder) for an app without it; `DM_MOTION=1` in `env.txt` turns it back on.

## Game language

English by default. Pick another when packing with `--language` (`-l`), by code or by name: `-l ja`, `-l zh-Hans`, `-l French` (`LANGUAGE` in Spyder). When the NUP's control data lists the game's languages, the script prints them and stops on one the game lacks. To change it without rebuilding, pick another in the settings window.

## Settings in a file

Lines like `DM_OPMODE=0` (handheld mode: lower resolution) in `~/Library/Application Support/NativeNavi/<title ID>/env.txt` win over the settings window, which then shows them greyed out ([the full list](settings.md#settings-in-a-file)). `DM_FPS_TARGET=30` runs the game at 30 fps instead of 60, which uses less power. `DM_LAUNCHER=0` skips the settings window.

## Console files

The host ships no console data. The one console file it reads is your console's Mii database, when you copy it from your own console to `~/Library/Application Support/NativeNavi/system/save/8000000000000030/MiiDatabase.dat`: a game that looks a Mii up there finds it. The file is only read; without it the database is empty.

## Shaders on first sight

Short stutters the first time a new scene appears are normal: the app compiles graphics for it once and remembers them for the next launch. What it remembered, and what the game's list names, is built before the game starts, behind "Preparing shaders" and a progress bar when that takes more than half a second.
