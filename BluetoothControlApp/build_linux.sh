#!/usr/bin/env bash
# Builds dist/HydroponicsControl (one file, Linux x86_64) and, with --install,
# installs it with its icon and launcher for the current user (InstallDesktop.py).
#
# Needs: Python 3.10+ with venv, and on Debian/Ubuntu the GTK runtime the Flet
# desktop client links against:
#   sudo apt install python3-venv libgtk-3-0 libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libsecret-1-0 bluez
# Smoke test: dist/HydroponicsControl --demo
#
# PyInstaller does not cross-compile: run this on Linux (or WSL), not Windows.
# Build on the oldest distribution you target (glibc is forward-compatible only).
set -euo pipefail
cd "$(dirname "$0")"

# A separate venv: the Windows one in .venv cannot be reused from WSL.
VENV=.venv-linux
if [ ! -x "$VENV/bin/python" ]; then
    # Without the python3-venv package (no ensurepip), make a venv without pip
    # and install into it with the user's pip (pip --python, pip >= 22.3).
    python3 -m venv "$VENV" 2>/dev/null || { rm -rf "$VENV"; python3 -m venv --without-pip "$VENV"; }
fi
if "$VENV/bin/python" -m pip --version >/dev/null 2>&1; then
    PIP=("$VENV/bin/python" -m pip)
else
    PIP=(python3 -m pip --python "$VENV/bin/python")
fi
"${PIP[@]}" install --quiet -r Requirements-build.txt

# Imported lazily (per platform / per mode), so listed for PyInstaller.
"$VENV/bin/flet" pack Main.py \
    --name HydroponicsControl \
    --icon assets/AppIcon.png \
    --add-data "assets:assets" \
    --hidden-import BleLink DemoController OsBond dbus_fast.aio dbus_fast.service \
    --bundle-id com.appveyor.flet \
    --product-name "Hydroponics Control" \
    --product-version 0.9.0 \
    --distpath dist -y
echo "Built dist/HydroponicsControl"

if [ "${1:-}" = "--install" ]; then
    "$VENV/bin/python" InstallDesktop.py
fi
