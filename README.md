# BloodRayne: Terminal Cut — Extras mod

A standalone `dinput8.dll` proxy for **BloodRayne: Terminal Cut** (GOG) that
adds a persistent **"All powers and combos"** option to the native New Game
menu — no files inside the game's PODs are ever modified.

## Features

- New cycler row at the top of **New Game**: *All powers and combos: Yes/No*
  (before *Training*), localized for all six official languages
  (EN/ES/FR/IT/RU/JA) and persisted in `br1mod.ini` (`[Powers] all`).
- When **Yes**, the mod replicates the classic "everything unlocked" script
  mod at runtime, in memory only:
  - `setSlowMoAvailability(true)` + `setZoomAvailability(true)` injected at
    level start,
  - `setComboLevel(5,5)` — every restrictive `setComboLevel(a,b)` is
    repointed to the max,
  - `flagOn(powerMovesAreAvailable)` injected — unlocks the power-move
    table the engine gates via `0x499A06`,
  - `flagOn(disableHarpoon[Jump])` rewritten to `flagOff(...)` — harpoon
    and harpoon jump always available.
- When **No**, level scripts run 100% stock.
- Works from the Training level through every act (LA/AR/DE), because the
  recipe is applied to the first command fetch of each level script's real
  execution pass — the same place the old `WORLD.POD` mod put its setup.

## How it works

`rayne1.exe` imports `DirectInput8Create` from `dinput8.dll`, so the proxy
shadows the system DLL in the game folder. Forwarding resolves the export
RVA from a pristine on-disk image (another component hooks the in-memory
export, which would loop back into us otherwise).

Three inline patches (`push addr; ret`, byte-verified against the exe):

- `0x4C89AE` — inside the New Game menu builder, appends our row before
  Training. Action code `0x150` toggles the ini value and reuses the native
  menu-rebuild path (`0x4CE0C6`).
- `0x4CCD46` — action dispatch load; intercepts `0x150`.
- `0x510EC2` — script-command fetch inside the level-script executor
  (`0x510E40`). In real-execution mode (`[0x1E4B570] == 0`), slot 0 is
  replayed (return lands past `inc eax`) so the four setup commands run
  before the level's first real command; restrictive commands are
  repointed to unlocked equivalents kept in our own buffers.

All addresses are resolved against `GetModuleHandle(NULL)` at runtime —
`rayne1.exe` is ASLR-relocated.

## Build

x86 Native Tools prompt:

```
build.bat
```

Produces `dinput8.dll`. Deploy next to `rayne1.exe` with `br1mod.ini`.

## Install / uninstall

- Install: copy `dinput8.dll` + `br1mod.ini` next to `rayne1.exe`.
- Uninstall: delete those two files. Saves, PODs and `PCPOD.INI` are never
  touched.
