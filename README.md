# 🎮 GB1k - GameBoy Emulator

A compact Nintendo Game Boy (DMG) emulator in a single C file of under 1000 lines, built on [SDL3](https://libsdl.org).

GB1k isn't trying to be cycle-perfect. It aims for the smallest, most readable code that still plays the classic library well, including Tetris, Super Mario Land 1 & 2, Zelda: Link's Awakening, Mega Man I–V, Wario Land, DuckTales and Pokémon Red/Yellow.

## Features

- Complete Sharp SM83 CPU instruction set, with timing accurate to the memory access
- Scanline renderer for background, window and sprites (8×8 and 8×16, flipping, priority, 10 sprites per line)
- Timer and interrupts (VBlank, STAT, timer)
- 4-channel stereo audio: two pulse channels (with sweep on channel 1), wave and noise, plus NR50/NR51 panning and master volume
- One simplified bank-switching mapper that runs most MBC1, MBC3 and MBC5 games
- Battery saves, stored automatically per ROM
- Four colour palettes, adjustable volume and per-channel audio muting
- Keyboard and gamepad input
- Resizable window with crisp pixel-art scaling

## Building

You need a C11 compiler (e.g. Clang or GCC) and SDL3.

```bash
# macOS
brew install sdl3

# Debian / Ubuntu (SDL3 packages are available from Debian trixie / Ubuntu 25.04 onwards)
sudo apt install libsdl3-dev
```

Then build:

```bash
make
```

The code is plain C11 with no compiler extensions; it builds cleanly with `-Wpedantic`.

## Usage

```bash
./gb1k path/to/game.gb
```

The window opens at about 80% of your desktop size and can be resized freely. The window title shows the game title from the cartridge header.

### Controls

| Game Boy | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrow keys, `W A S D`, `8 4 2 6` / numpad | D-pad |
| A | `Z`, `Y`, `I`, `Ctrl` | South (Xbox **A** / PlayStation **✕**) |
| B | `X`, `O`, `Alt` | East (Xbox **B** / PlayStation **○**) |
| Start | `Enter`, `K` | West (Xbox **X** / PlayStation **□**) |
| Select | `Space`, `L` | North (Xbox **Y** / PlayStation **△**) |

| Hotkey | Action |
|---|---|
| `F1`–`F4` | Toggle audio channel 1–4 on/off (pulse 1, pulse 2, wave, noise; useful for audio debugging) |
| `F5`| Toggle turbo mode (make CPU 4x as fast) |
| `F9` | Cycle colour palette (grey, classic green, sepia, blue) |
| `F10` | Cycle volume (100% → 50% → 25% → mute) |
| `Esc` | Quit |

Both `Z` and `Y` map to A so the layout works on QWERTY and QWERTZ keyboards.

### Tips

- **Saves are written when you quit.** Exit with `Esc` or by closing the window. If the process is killed or crashes, progress since the last start is lost.
- Save files are named `save.<hash>.ram` and stored in the per-user SDL preferences folder (`kieselsteini/gb1k`), so it doesn't matter where you start the emulator from:
  - macOS: `~/Library/Application Support/kieselsteini/gb1k/`
  - Linux: `~/.local/share/kieselsteini/gb1k/`
  - Windows: `%APPDATA%\kieselsteini\gb1k\`

  The hash is computed from the whole ROM, so different versions or revisions of a game get separate saves.
- Only cartridges whose header says they have a battery get a save file.
- Gamepads can be plugged in while the emulator is running.

## Limitations

These are deliberate trade-offs to keep the code small:

- DMG only: no Game Boy Color or Super Game Boy features
- No serial link, so link-cable multiplayer and trading don't work
- The PPU draws whole scanlines, so mid-scanline raster effects aren't emulated, and mode 3 has a fixed, short length
- The window layer has no internal line counter, so games that toggle the window mid-frame can show glitches
- OAM DMA completes instantly; the HALT bug and the TIMA reload delay aren't emulated
- Simplified mapper: no RAM-enable handling, no MBC3 real-time clock, no MBC1 multicart wiring
- No boot ROM: the emulator starts directly in the post-boot DMG state
