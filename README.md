# QuestGamePIC32USBandC

PIC32 USB CDC project with a GTK host game-controller app.

Current baseline:

- PIC32 enumerates as USB CDC serial.
- GTK host app connects over `/dev/cu.usbmodem*`.
- Host app edits and runs a 6-category, 5-row quiz board with a built-in starter trivia deck and A-D answers.
- Category selection alternates between players; the non-chooser is locked during board selection.
- PIC drives two I2C LCD backpacks: player 1 at `0x26`, player 2 at `0x23`.
- PIC stores the active question/options and scrolls player LCD text locally.
- PIC reads four active-low buttons on chip pins `6`, `7`, `11`, and `14`.
- PIC controls four button LEDs on chip pins `16`, `24`, `25`, and `26`, including solid/slow/fast game-state patterns.
- CDC control transfers are acknowledged correctly, so macOS opens the device without the previous timeout.

Default button and LED wiring:

| Button | Switch wire 1 | Switch wire 2 | LED + | LED - |
|---|---:|---:|---:|---:|
| P1 Next | chip pin `6` | `GND` | `3.3V through 470Ω` | chip pin `16` |
| P1 Select | chip pin `7` | `GND` | `3.3V through 470Ω` | chip pin `24` |
| P2 Next | chip pin `11` | `GND` | `3.3V through 470Ω` | chip pin `25` |
| P2 Select | chip pin `14` | `GND` | `3.3V through 470Ω` | chip pin `26` |

LED outputs are active-low: the PIC drives the LED pin low to turn the LED on.

Build:

```sh
cd quest.X
make build

cd ../host/gtk-screen-sender
make
```

Firmware HEX:

```text
quest.X/dist/default/production/quest.X.production.hex
```
