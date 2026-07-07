# QuestGamePIC32USBandC

PIC32 USB CDC project with a GTK host controller app.

Current working baseline:

- PIC32 enumerates as USB CDC serial.
- GTK host app connects over `/dev/cu.usbmodem*`.
- Host app can send text to the PIC.
- PIC drives two I2C LCD backpacks at `0x26` and `0x23`.
- CDC control transfers are acknowledged correctly, so macOS opens the device without the previous timeout.
