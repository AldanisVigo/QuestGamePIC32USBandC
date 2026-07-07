#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
HEX_FILE="$PROJECT_DIR/dist/default/production/HelloPic32.X.production.hex"

echo "Building..."
make -C "$PROJECT_DIR" -f nbproject/Makefile-default.mk dist/default/production/HelloPic32.X.production.hex

echo "HEX: $HEX_FILE"

if [ ! -f "$HEX_FILE" ]; then
    echo "ERROR: HEX file not found."
    exit 1
fi

echo "Programming PIC32 with IPE 6.20 + PICkit 3..."
/Applications/microchip/mplabx/v6.20/mplab_platform/mplab_ipe/bin/ipecmd.sh \
    -TPPK3 \
    -P32MX270F256B \
    -F"$HEX_FILE" \
    -M

echo "Done!"