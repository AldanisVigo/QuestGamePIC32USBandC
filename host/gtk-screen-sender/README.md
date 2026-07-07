# Quest Game Controller

Standalone GTK+ 3 desktop app for running the PIC32 two-player quiz game over USB CDC serial.

## Build

Install GTK+ 3 and `pkg-config`, then build this host project:

```sh
cd host/gtk-screen-sender
make
```

macOS with Homebrew:

```sh
brew install gtk+3 pkg-config
make
```

Debian/Ubuntu:

```sh
sudo apt install libgtk-3-dev pkg-config
make
```

## Run

```sh
./pic32-screen-sender
```

The app scans common USB CDC serial device paths:

- macOS: `/dev/cu.usbmodem*`, `/dev/cu.usbserial*`
- Linux: `/dev/ttyACM*`, `/dev/ttyUSB*`

## Protocol

Host to PIC:

- `P1:<line1>|<line2>` updates player 1 LCD.
- `P2:<line1>|<line2>` updates player 2 LCD.
- `BOTH:<line1>|<line2>` updates both LCDs.
- `QSET:<question>|<A>|<B>|<C>|<D>` sends one question to the PIC; the PIC scrolls it locally.
- `QSEL:P1:A` or `QSEL:P2:D` changes the displayed answer option for one player.
- `LED:P1:NEXT:ON`, `OFF`, `SLOW`, or `FAST` controls player 1 next LED.
- `LED:P1:SELECT:ON`, `OFF`, `SLOW`, or `FAST` controls player 1 select LED.
- `LED:P2:NEXT:ON`, `OFF`, `SLOW`, or `FAST` controls player 2 next LED.
- `LED:P2:SELECT:ON`, `OFF`, `SLOW`, or `FAST` controls player 2 select LED.

PIC to host:

- `BTN:P1:NEXT`
- `BTN:P1:SELECT`
- `BTN:P2:NEXT`
- `BTN:P2:SELECT`
