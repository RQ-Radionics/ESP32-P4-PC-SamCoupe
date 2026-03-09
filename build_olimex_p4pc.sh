#!/usr/bin/env bash
# build_olimex_p4pc.sh — Build SimCoupe for Olimex ESP32-P4-PC (RISC-V RV32IMAFC)
#
# Uses ONLY sdkconfig.defaults (common) + sdkconfig.defaults.olimex-p4pc.
# Does NOT include sdkconfig.defaults.esp32p4 (which enables the esp-hosted
# C6 WiFi coprocessor and would trigger SDIO probing on a board without one).
#
# Usage:
#   ./build_olimex_p4pc.sh          — build only
#   ./build_olimex_p4pc.sh flash    — build + flash (auto-detect port)
#   ./build_olimex_p4pc.sh monitor  — build + flash + monitor
set -e

cd "$(dirname "$0")"

echo "=== SimCoupe ESP32-P4-PC build ==="

# Wipe cached sdkconfig and build/ to ensure a clean start.
# SDKCONFIG_DEFAULTS overrides win because idf.py applies them after the
# Kconfig defaults during the configure phase of `build`.
rm -f sdkconfig
rm -rf build

export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.olimex-p4pc"

idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.olimex-p4pc" \
    set-target esp32p4

idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.olimex-p4pc" \
    build

echo ""
echo "=== Build complete ==="
echo ""

case "${1:-}" in
    flash)
        echo "Flashing..."
        idf.py -p "${PORT:-/dev/cu.usbmodem*}" flash
        ;;
    monitor)
        echo "Flashing and monitoring..."
        idf.py -p "${PORT:-/dev/cu.usbmodem*}" flash monitor
        ;;
    *)
        echo "Done. Flash with:"
        echo "  idf.py -p /dev/cu.usbmodem* flash"
        echo "  idf.py -p /dev/cu.usbmodem* flash monitor"
        ;;
esac
