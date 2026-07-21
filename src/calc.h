/**
 * @file calc.h
 * Pure desk-calculator core: immediate-execution infix (one pending binary op),
 * a typed-entry buffer, six store/recall registers, and readout formatting
 * (display, hex, binary, unit conversions). No LVGL, no Redis, deterministic —
 * the calc mode is thin glue over this.
 */
#ifndef KDESKDASH_CALC_H
#define KDESKDASH_CALC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CALC_REGS 6

/* Entry buffer: up to 15 significant digits plus sign and decimal point. */
#define CALC_ENTRY_MAX 17

/* Panel calibration, measured with a mm ruler against the 1920x440 screen
 * (see clock.c: 150 px buttons span 19.5 mm). Used for the px<->mm readouts. */
#define CALC_PX_PER_MM 7.69

/* Exact by definition. */
#define CALC_MM_PER_IN 25.4

typedef enum {
    CALC_KEY_D0, /* digits are contiguous: CALC_KEY_D0 + n */
    CALC_KEY_D1,
    CALC_KEY_D2,
    CALC_KEY_D3,
    CALC_KEY_D4,
    CALC_KEY_D5,
    CALC_KEY_D6,
    CALC_KEY_D7,
    CALC_KEY_D8,
    CALC_KEY_D9,
    CALC_KEY_DOT,
    CALC_KEY_BACKSPACE,
    CALC_KEY_NEG, /* +/- toggle */
    CALC_KEY_ADD,
    CALC_KEY_SUB,
    CALC_KEY_MUL,
    CALC_KEY_DIV,
    CALC_KEY_POW, /* binary x^y */
    CALC_KEY_SQR, /* unary x^2 */
    CALC_KEY_CUBE, /* unary x^3 */
    CALC_KEY_PI,
    CALC_KEY_E,
    CALC_KEY_EQ,
    CALC_KEY_CLEAR,
} calc_key_t;

typedef struct {
    char   entry[CALC_ENTRY_MAX + 1]; /* the number being typed */
    bool   typing;                    /* entry[] is live (mid-entry) */
    double value;                     /* displayed value when not typing */
    double acc;                       /* left operand of the pending op */
    int    pending;                   /* calc_key_t binary op, or -1 */
    bool   new_operand;               /* an operand arrived since `pending` was set */
    bool   error;                     /* div-by-zero / overflow; C or a digit recovers */
    double regs[CALC_REGS];
    bool   reg_set[CALC_REGS];
} calc_t;

/* Reset everything, including registers. */
void calc_init(calc_t *c);

/* Feed one key press. Registers survive CLEAR and errors. */
void calc_key(calc_t *c, calc_key_t k);

/* Copy the current display value into register r (0..CALC_REGS-1). No-op on
 * error state or bad index. */
void calc_store(calc_t *c, int r);

/* Recall register r into the display (as a freshly entered operand). No-op if
 * the register was never stored, on error state, or bad index. */
void calc_recall(calc_t *c, int r);

/* The current numeric value shown on the display (0.0 while in error state). */
double calc_value(const calc_t *c);

/* Main display string: the entry as typed, the formatted value, or "Error". */
void calc_display(const calc_t *c, char *buf, size_t n);

/* Format v like the main display does ("%.12g", -0 normalised). */
void calc_format_value(double v, char *buf, size_t n);

/* Hex readout ("0x1A2B", two's complement for negatives). Only for integral
 * values representable in int64; returns false (buf = "") otherwise. */
bool calc_hex(const calc_t *c, char *buf, size_t n);

/* Binary readout, nibble-grouped ("0100 1101 0010"). Only for integral values
 * in [0, 2^32); returns false (buf = "") otherwise. */
bool calc_bin(const calc_t *c, char *buf, size_t n);

/* Unit conversions for the live readout panel. */
double calc_in_to_mm(double v);
double calc_mm_to_in(double v);
double calc_mm_to_px(double v);
double calc_px_to_mm(double v);

#endif /* KDESKDASH_CALC_H */
