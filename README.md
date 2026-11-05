<p align="center">
  <img src="src/core/icon.png" alt="Yumemi icon" width="128" height="128">
</p>
<h1 align="center">Yumemi</h1>
<p align="center">A Game Boy Advance emulator written in C using raylib</p>

## Usage
Open a ROM:
- Drag and drop a `.gba` file onto `yumemi.exe`.
- Open a `.gba` in the emulator itself (**File**->**Import ROM**)

## Features
| Component                       | Status |
|---------------------------------|--------|
| CPU (ARM + THUMB)               | Mostly |
| PPU                             | Done |
| APU                             | Planned |
| DMA                             | Done |
| Timers                          | Done |
| Interrupts                      | Done |
| Keypad                          | Done |
| Save data (SRAM, Flash, EEPROM) | Done |
| Save states                     | Done |
| RTC                             | Planned |
| BIOS                            | Mostly (HLE only) |

## Controls
| GBA button | Key |
|------------|-----|
| A          | `Z` |
| B          | `X` |
| L          | `Q` |
| R          | `W` |
| Start      | `Enter` |
| Select     | `Backspace` |
| D-pad      | Arrow keys |

## Test results
Tests taken from here: [gba-test](https://github.com/jsmolka/gba-tests)
| Test        | Result |
|-------------|-----|
| arm         | Success |
| bios        | Success |
| memory      | Success |
| nes         | Fail (001) |
| ppu         | Success |
| flash64     | Success |
| flash128    | Success |
| (Save) none | Success |
| sram        | Success |
| thumb       | Success |
| unsafe      | Fail (001) |

## Sources & Tools
- [Very Sleepy](http://www.codersnotes.com/sleepy/)
- [GBATEK](https://mgba-emu.github.io/gbatek/)
- [ARM7TDMI reference (PDF)](https://www.dwedit.org/files/ARM7TDMI.pdf)