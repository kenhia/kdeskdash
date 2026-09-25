#!/usr/bin/env bash
# font-coverage.sh — the committed fonts really carry the characters
# fonts/generate.sh declares (WI #2657; ported from kpidash WI #2646).
#
# fonts/*.c are generated artifacts checked into the repo so that a
# cross-compile needs no Node.js. That is the right trade and it buys one
# specific way to be wrong: someone widens RANGE in fonts/generate.sh and does
# not re-run the generator. Nothing fails, and the only symptom is a box on the
# panel plus one warning line in a journal nobody is reading.
#
# So this asserts the two ends against each other: every codepoint RANGE and
# SYMBOLS declare is actually present in every generated font's cmaps, and
# every size SIZES names has a file. All three are read from generate.sh
# rather than restated here, because a second copy of a list is one more thing
# that can drift. SYMBOLS matters as much as RANGE here: these fonts replace
# LVGL's built-ins, so a symbol missing from them is an LV_SYMBOL_* the panel
# used to draw and no longer does.

set -euo pipefail
cd "$(dirname "$0")/.."

pass=0
fail=0

check() { # check <description> <condition-already-evaluated:0|1>
    if [ "$2" -eq 0 ]; then
        pass=$((pass + 1))
    else
        echo "FAIL: $1" >&2
        fail=$((fail + 1))
    fi
}

RANGE="$(sed -n 's/^RANGE="\(.*\)"$/\1/p' fonts/generate.sh)"
SYMBOLS="$(sed -n 's/^SYMBOLS="\(.*\)"$/\1/p' fonts/generate.sh)"
SIZES="$(sed -n 's/^SIZES=(\(.*\))$/\1/p' fonts/generate.sh)"
for v in RANGE SYMBOLS SIZES; do
    if [ -z "${!v}" ]; then
        echo "FAIL: could not read $v= from fonts/generate.sh" >&2
        exit 1
    fi
done
echo "RANGE from fonts/generate.sh: $RANGE"
echo "SIZES from fonts/generate.sh: $SIZES"

# Expand "0x20-0x7E,0xA0-0xFF,0x2022" into one decimal codepoint per line.
#
# printf '%s\n', not '%s': without the trailing newline `read` gets an
# unterminated last line, returns non-zero, and the loop exits WITHOUT running
# the body — so the final entry in RANGE is silently dropped and never
# checked. Caught by deliberately breaking this gate to see it fail.
expand_range() {
    printf '%s\n' "$1" | tr ',' '\n' | while IFS= read -r part; do
        [ -n "$part" ] || continue
        case "$part" in
        *-*)
            lo=$((${part%%-*}))
            hi=$((${part##*-}))
            ;;
        *)
            lo=$((part))
            hi=$lo
            ;;
        esac
        cp=$lo
        while [ "$cp" -le "$hi" ]; do
            echo "$cp"
            cp=$((cp + 1))
        done
    done
}

# Every codepoint a font's cmaps resolve, one decimal value per line.
#
# lv_font_conv emits two cmap shapes. FORMAT0_TINY is a contiguous run from
# .range_start of .range_length codepoints. SPARSE_TINY names a
# unicode_list_N[] whose entries are OFFSETS from .range_start, not absolute
# codepoints — getting that wrong is the one real trap in this parse, and it
# is why the ellipsis and the two Nerd Font glyphs live in the same cmap here.
font_codepoints() {
    awk '
    /^static const uint16_t unicode_list_[0-9]+\[\] = \{/ {
        name = $4; sub(/\[\]$/, "", name); inlist = 1; buf[name] = ""; next
    }
    inlist { if ($0 ~ /\};/) { inlist = 0; next } buf[name] = buf[name] $0; next }

    /\.range_start/ {
        if (match($0, /\.range_start = [0-9]+/))
            start = substr($0, RSTART + 15, RLENGTH - 15) + 0
        if (match($0, /\.range_length = [0-9]+/))
            len = substr($0, RSTART + 16, RLENGTH - 16) + 0
        next
    }
    /\.unicode_list/ {
        if ($0 ~ /\.unicode_list = NULL/) {
            for (i = 0; i < len; i++) print start + i
        } else if (match($0, /unicode_list_[0-9]+/)) {
            lname = substr($0, RSTART, RLENGTH)
            n = split(buf[lname], parts, /[ ,\t]+/)
            for (i = 1; i <= n; i++) {
                if (parts[i] ~ /^0x[0-9a-fA-F]+$/) {
                    off = strtonum(parts[i])
                    print start + off
                }
            }
        }
        next
    }
    ' "$1" | sort -u
}

# Sorted lexicographically, not numerically, on both sides: `comm` compares
# with the C collation and rejects numerically-sorted input outright.
required="$(expand_range "$RANGE,$SYMBOLS" | sort -u)"
required_count="$(printf '%s\n' "$required" | grep -c .)"
echo "required codepoints: $required_count"
check "RANGE expands to a plausible number of codepoints" \
    "$([ "$required_count" -ge 95 ] && echo 0 || echo 1)"

for sz in $SIZES; do
    check "fonts/kd_font_montserrat_${sz}.c exists (SIZES names it)" \
        "$([ -f "fonts/kd_font_montserrat_${sz}.c" ] && echo 0 || echo 1)"
done

for f in fonts/kd_font_montserrat_*.c; do
    covered="$(font_codepoints "$f")"
    missing="$(comm -23 <(printf '%s\n' "$required") <(printf '%s\n' "$covered") | head -20)"
    if [ -n "$missing" ]; then
        echo "FAIL: $f is missing codepoints generate.sh declares:" >&2
        printf '%s\n' "$missing" | while IFS= read -r cp; do
            printf '        U+%04X\n' "$cp" >&2
        done >&2
        echo "       re-run: bash fonts/generate.sh" >&2
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

# Pinned by name so a future narrowing of RANGE has to argue with the
# incident, not just with a number: U+2014 EM DASH is WI #2657's headline,
# U+00B0 / U+00B7 each cost kpidash a measured journal flood (WI #363, #2646).
for cp in 8212 176 183; do
    got="$(font_codepoints fonts/kd_font_montserrat_20.c | grep -cx "$cp" || true)"
    check "$(printf 'U+%04X is in the panel font (regression: a named incident)' "$cp")" \
        "$([ "$got" -eq 1 ] && echo 0 || echo 1)"
done

echo "font-coverage: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
