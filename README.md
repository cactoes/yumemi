<p align="center">
  <img src="src/core/icon.png" alt="Yumemi icon" width="128" height="128">
</p>
<h1 align="center">Yumemi</h1>
<p align="center">A Game Boy Advance emulator</p>

## Features
| Component                       | Status |
|---------------------------------|--------|
| CPU (ARM + THUMB)               | Done |
| PPU                             | Done |
| APU                             | Planned |
| DMA                             | Done |
| Timers                          | Done |
| Interrupts                      | Done |
| Keypad                          | Done |
| Save data (SRAM, Flash, EEPROM) | Partial |
| Save states                     | Partial |
| RTC                             | Planned |
| BIOS                            | HLE only |

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

## Sources & Tools
- [Very Sleepy](http://www.codersnotes.com/sleepy/)
- [GBATEK](https://mgba-emu.github.io/gbatek/)
- [ARM7TDMI reference (PDF)](https://www.dwedit.org/files/ARM7TDMI.pdf)