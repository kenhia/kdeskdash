#!/usr/bin/env bash
# fonts/generate.sh — generate the panel's Montserrat text fonts (WI #2657).
#
# Prerequisites: npx lv_font_conv (npm install lv_font_conv)
# Usage: bash fonts/generate.sh
#
# The generated .c files are committed, so a cross-compile needs no Node.js.
# `just check-fonts` asserts the committed files still carry what RANGE and
# SYMBOLS below declare.
#
# These REPLACE LVGL's built-in lv_font_montserrat_NN (disabled in lv_conf.h).
# The built-ins cover ASCII plus ° and • only, so an agent's em dash in the
# Claude feed drew as a box. The recipe is LVGL's own
# (lib/lvgl/scripts/built_in_font/built_in_font_gen.py) — the same TTF, the
# same FontAwesome symbol set behind LV_SYMBOL_*, the same flags — with a wider
# RANGE. Both source fonts are read from the pinned LVGL submodule rather than
# vendored a second time. The set is kpidash's (WI #2646), so the two
# dashboards agree on what an agent can type.
#
#   0x20-0x7E  ASCII printable.
#   0xA0-0xFF  Latin-1 Supplement. Subsumes 0xB0 ° DEGREE SIGN and 0xB7 ·
#              MIDDLE DOT, and covers the accented letters and © ± « » ½ that
#              ordinary prose reaches for.
#   0x2013/14  – EN DASH, — EM DASH. The most common thing an agent types.
#   0x2018/19  ' ' single curly quotes — what a smart-quoted apostrophe is.
#   0x201C/1D  " " double curly quotes.
#   0x2022     • BULLET, used as a separator.
#   0x2026     … HORIZONTAL ELLIPSIS, what an editor makes of "...".
#
# A character outside the set still draws as a box; src/logfilter.c makes
# sure the journal names it once rather than on every redraw.

set -euo pipefail
cd "$(dirname "$0")"

SRC="../lib/lvgl/scripts/built_in_font"
TEXT_TTF="${SRC}/Montserrat-Medium.ttf"
SYMBOL_WOFF="${SRC}/FontAwesome5-Solid+Brands+Regular.woff"
BPP=4
RANGE="0x20-0x7E,0xA0-0xFF,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026"
# LV_SYMBOL_* — copied verbatim from built_in_font_gen.py (LVGL v9.2.2).
SYMBOLS="61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

# The sizes src/ uses. Adding one means adding it here, to lv_conf.h's
# LV_FONT_CUSTOM_DECLARE, and to CMakeLists.txt's KD_FONT_SOURCES.
SIZES=(14 20 28 36 48)

for sz in "${SIZES[@]}"; do
    name="kd_font_montserrat_${sz}"
    echo "Generating ${name}.c ..."
    npx lv_font_conv \
        --no-compress --no-prefilter \
        --bpp "${BPP}" \
        --size "${sz}" \
        --font "${TEXT_TTF}" -r "${RANGE}" \
        --font "${SYMBOL_WOFF}" -r "${SYMBOLS}" \
        --format lvgl \
        --force-fast-kern-format \
        -o "${name}.c" \
        --lv-font-name "${name}"
done

echo "Done. Generated ${#SIZES[@]} font files."
