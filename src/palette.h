/**
 * @file palette.h
 * The canonical named color palette — every deliberate design-language color,
 * one X-macro table, paint-store names. "Make the label CLAUDE_CORAL" is a
 * complete instruction; the `palette` mode is this table rendered as swatches.
 *
 * Adding a color is one X() line. Legacy strays (clock's pre-design-language
 * shades) are intentionally absent — repaint them onto palette colors instead
 * of adding them here (see WI 514).
 */
#ifndef KDESKDASH_PALETTE_H
#define KDESKDASH_PALETTE_H

#include <stdint.h>

/* name, rgb (24-bit), usage note shown on the swatch card */
#define KD_PALETTE(X) \
    /* --- surfaces & structure --- */ \
    X(VOID,          0x05070d, "app background - every screen") \
    X(DEEP_SLATE,    0x0a0f1a, "panel / card fill") \
    X(RAISED_SLATE,  0x101726, "pressed / hover lift on panels") \
    X(GUNMETAL_SEAM, 0x1b2334, "hairline borders, dividers") \
    X(QUIET_KEY,     0x141c2b, "calc: fn / constant keys") \
    X(SLATE_KEY,     0x1a2332, "calc: digit island") \
    X(STEEL_KEY,     0x24344d, "calc: binary-op keys") \
    X(SMOKED_MAROON, 0x3a1b22, "calc: C key - muted danger") \
    X(SCORCHED_WASH, 0x2a1109, "claude: row wash behind BLOCKED ON YOU") \
    /* --- text --- */ \
    X(MOON_INK,      0xe9edf6, "primary text") \
    X(STEEL_MIST,    0x8b95ab, "secondary text, captions") \
    X(FADED_DENIM,   0x525d73, "muted / disabled text") \
    /* --- accents & status --- */ \
    X(CLAUDE_CORAL,  0xcf6b4a, "accent: claude, menu headers, calc hex") \
    X(BURNT_CORAL,   0x99492e, "calc: = key - darker coral anchor") \
    X(ALARM_EMBER,   0xe0563f, "claude: hard-blocked status") \
    X(PATIENT_AMBER, 0xb9832c, "claude: awaiting input / warn tone") \
    X(WORKING_JADE,  0x35a271, "claude: working status; dev OK") \
    X(INSIDER_MINT,  0x38be84, "Remote: Insiders green; calc mm-out rows") \
    X(EDGE_TEAL,     0x2ec4c4, "Remote: Edge windows; calc bin + mm-in rows") \
    X(CODE_BLUE,     0x60a5eb, "Remote: VS Code stable blue") \
    X(ROCKET_RED,    0xef5350, "Remote: Apps rail") \
    X(STAR_GOLD,     0xd9a441, "Remote: favorite star") \
    X(HOST_GREY,     0x969696, "Remote: host names") \
    X(ZOMBIE_RUST,   0xc0392b, "GoLZ: menu tile, zombie red") \
    /* --- dev charts --- */ \
    X(CPU_SKY,       0x4dabf7, "dev chart: CPU") \
    X(GPU_GRASS,     0x40c057, "dev chart: GPU") \
    X(RAM_SALMON,    0xff6b6b, "dev chart: RAM") \
    X(VRAM_MANGO,    0xff922b, "dev chart: VRAM") \
    X(TOP_LILAC,     0xb197fc, "dev chart: top process") \
    X(SELECT_BLUE,   0x3d6fb0, "dev: selected host row")

typedef enum {
#define KD_PAL_ENUM(name, rgb, usage) KD_PAL_##name,
    KD_PALETTE(KD_PAL_ENUM)
#undef KD_PAL_ENUM
    KD_PAL_COUNT
} kd_pal_id_t;

/* Number of palette entries (== KD_PAL_COUNT; a function for table iteration). */
int kd_pal_count(void);

/* Accessors are bounds-checked: NULL / 0 for an out-of-range index. */
const char *kd_pal_name(int i);
uint32_t    kd_pal_rgb(int i);
const char *kd_pal_usage(int i);

/* Look up an entry by name, case-insensitive ("claude_coral" works).
 * Returns the index, or -1 if unknown. */
int kd_pal_find(const char *name);

#endif /* KDESKDASH_PALETTE_H */
