# @NAME@

@GAMES@, recompiled to run natively on your PC.

@NAME@ contains **no game code or data**. You use your own copy of the game:
the first time you open @NAME@, it recompiles that copy on this computer.
Nothing it makes from your game may be shared.

## What you need

- Windows 10 or 11, 64-bit, with a Vulkan-capable GPU.
- Your own **decrypted** dump of the game: `.3ds`, `.cci`, `.cxi` or `.cia`.
- Its **decrypted** update, as a `.cia`, if it has one.
- About 2 GB free on the system drive for the recompiled game and its build
  files. The first setup takes about a minute (longer on slower computers);
  the rest of the game then finishes building in the background while you
  play, and is used from the next start. On a modest processor, tick
  **Fast compile**: the background build takes about a third of the time,
  the code the game nearly never runs left unoptimized.
- A C compiler is optional: if clang can't build here, @NAME@ downloads one
  (Zig) during setup.

## Getting started

1. Extract this zip into a folder of its own. Keep the `3dsrecomp` and `game`
   folders and `port.toml` beside `@NAME@.exe`.
2. Open `@NAME@.exe`, press **Set up** beside your game and follow the steps:
   pick your dump, pick its update, start the recompile.
3. When it is done, press **Play**. You can also add a desktop shortcut.

`@NAME@.exe --play` starts the game played last right away.

## Playing

- **F1** opens the settings over the game: video, sound, controls.
- **F11** toggles fullscreen.
- **Single screen** (F1, *Screens*): the top screen fills the window, the
  bottom one comes in as a panel on **Tab** or the left stick's click (**L3**);
  the mouse, or the right stick as a pointer with **R2**/**R3**, touches it.
  **L2** swaps the screens.
- **60 FPS** (F1): frames drawn between the game's for smoother motion,
  without speeding the game up; up to your display's rate.

## Where things are

| What | Where |
| --- | --- |
| @NAME@'s choices | `%APPDATA%\@NAME@\config.toml` |
| Graphics, controls, sound | `%APPDATA%\@NAME@\player.toml` |
| Saves | `%APPDATA%\@NAME@\user\savedata\` |
| Each game's recompiled code and build files | `%APPDATA%\@NAME@\<title id>\` |

Back up your saves before trying new versions.

## Credits

The licenses of what @NAME@ is built with are in `LICENSES\`.
