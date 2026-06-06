#!/usr/bin/env bash
# Regenerate the LVGL C-array image from the source PNG.
# Requires: pypng, pillow, lz4 (see .venv-tools).
set -euo pipefail

cd "$(dirname "$0")/.."

PY="${PYTHON:-.venv-tools/bin/python}"
"$PY" lib/lvgl/scripts/LVGLImage.py --ofmt C --cf ARGB8888 -o assets assets/brain_rot.png

echo "Generated assets/brain_rot.c (symbol: brain_rot)"
