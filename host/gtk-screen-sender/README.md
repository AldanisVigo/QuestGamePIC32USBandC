# PIC32 Screen Sender

Standalone GTK+ 3 desktop app for sending text to the PIC32 over its USB CDC serial port.

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

It sends up to 32 printable ASCII characters followed by a newline. The companion firmware receive loop treats that newline as the display command terminator and writes the text across the detected LCD screens.
