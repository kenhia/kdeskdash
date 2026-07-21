/**
 * @file calc.c
 * Pure desk-calculator core. See calc.h for the model; the short version:
 * immediate-execution infix like every pocket calculator — one accumulator,
 * one pending binary op, `2 + 3 x 4 =` is 20, no precedence.
 */
#include "calc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strict-C math.h may omit these (we build gnu11, but be safe). */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#define PENDING_NONE (-1)

void calc_init(calc_t *c) {
    memset(c, 0, sizeof(*c));
    c->pending = PENDING_NONE;
}

/* Clear the calculation but keep the registers (CLEAR key / error recovery). */
static void clear_calc(calc_t *c) {
    c->entry[0] = '\0';
    c->typing = false;
    c->value = 0.0;
    c->acc = 0.0;
    c->pending = PENDING_NONE;
    c->new_operand = false;
    c->error = false;
}

double calc_value(const calc_t *c) {
    if (c->error)
        return 0.0;
    return c->typing ? atof(c->entry) : c->value;
}

/* A binary/unary result that isn't finite poisons the display, not the regs. */
static void set_result(calc_t *c, double v) {
    if (!isfinite(v)) {
        c->error = true;
        v = 0.0;
    }
    c->value = v;
    c->typing = false;
}

static double apply_pending(calc_t *c, double rhs) {
    double a = c->acc;
    switch (c->pending) {
    case CALC_KEY_ADD: return a + rhs;
    case CALC_KEY_SUB: return a - rhs;
    case CALC_KEY_MUL: return a * rhs;
    case CALC_KEY_DIV: return rhs == 0.0 ? NAN : a / rhs;
    case CALC_KEY_POW: return pow(a, rhs);
    default:           return rhs;
    }
}

static int entry_digit_count(const calc_t *c) {
    int n = 0;
    for (const char *p = c->entry; *p; p++)
        if (*p >= '0' && *p <= '9')
            n++;
    return n;
}

static void begin_entry(calc_t *c, const char *initial) {
    snprintf(c->entry, sizeof(c->entry), "%s", initial);
    c->typing = true;
    c->new_operand = true;
}

static void key_digit(calc_t *c, int digit) {
    char d[2] = {(char)('0' + digit), '\0'};
    if (!c->typing) {
        begin_entry(c, d);
        return;
    }
    /* "0" / "-0" absorb the new digit instead of growing "05". */
    size_t len = strlen(c->entry);
    if (len > 0 && c->entry[len - 1] == '0' &&
        (len == 1 || (len == 2 && c->entry[0] == '-')) &&
        strchr(c->entry, '.') == NULL) {
        c->entry[len - 1] = d[0];
        return;
    }
    if (entry_digit_count(c) >= 15 || len + 1 >= sizeof(c->entry))
        return;
    c->entry[len] = d[0];
    c->entry[len + 1] = '\0';
}

static void key_dot(calc_t *c) {
    if (!c->typing) {
        begin_entry(c, "0.");
        return;
    }
    if (strchr(c->entry, '.') == NULL && strlen(c->entry) + 1 < sizeof(c->entry))
        strcat(c->entry, ".");
}

static void key_backspace(calc_t *c) {
    if (!c->typing)
        return;
    size_t len = strlen(c->entry);
    if (len > 0)
        c->entry[len - 1] = '\0';
    if (c->entry[0] == '\0' || strcmp(c->entry, "-") == 0)
        snprintf(c->entry, sizeof(c->entry), "0");
}

static void key_neg(calc_t *c) {
    if (c->typing) {
        size_t len = strlen(c->entry);
        if (c->entry[0] == '-') {
            memmove(c->entry, c->entry + 1, len); /* includes NUL */
        } else if (len + 1 < sizeof(c->entry)) {
            memmove(c->entry + 1, c->entry, len + 1);
            c->entry[0] = '-';
        }
    } else {
        c->value = -c->value;
    }
}

static void key_binop(calc_t *c, calc_key_t op) {
    double cur = calc_value(c);
    /* Two ops in a row (no operand between) just swap the pending op. */
    if (c->pending != PENDING_NONE && c->new_operand)
        cur = apply_pending(c, cur);
    set_result(c, cur);
    if (c->error)
        return;
    c->acc = cur;
    c->pending = op;
    c->new_operand = false;
}

static void key_eq(calc_t *c) {
    double cur = calc_value(c);
    if (c->pending != PENDING_NONE)
        cur = apply_pending(c, cur);
    c->pending = PENDING_NONE;
    set_result(c, cur);
    c->new_operand = false;
}

/* Unary op / constant: becomes the current operand in place. */
static void set_operand(calc_t *c, double v) {
    set_result(c, v);
    c->new_operand = true;
}

void calc_key(calc_t *c, calc_key_t k) {
    if (c->error) {
        /* Error state: C clears; starting a new number recovers; else ignored. */
        bool starts_entry = (k >= CALC_KEY_D0 && k <= CALC_KEY_D9) ||
                            k == CALC_KEY_DOT || k == CALC_KEY_PI ||
                            k == CALC_KEY_E;
        if (k == CALC_KEY_CLEAR || starts_entry)
            clear_calc(c);
        else
            return;
        if (k == CALC_KEY_CLEAR)
            return;
    }

    if (k >= CALC_KEY_D0 && k <= CALC_KEY_D9) {
        key_digit(c, k - CALC_KEY_D0);
        return;
    }
    switch (k) {
    case CALC_KEY_DOT:       key_dot(c); break;
    case CALC_KEY_BACKSPACE: key_backspace(c); break;
    case CALC_KEY_NEG:       key_neg(c); break;
    case CALC_KEY_ADD:
    case CALC_KEY_SUB:
    case CALC_KEY_MUL:
    case CALC_KEY_DIV:
    case CALC_KEY_POW:       key_binop(c, k); break;
    case CALC_KEY_SQR: {
        double v = calc_value(c);
        set_operand(c, v * v);
        break;
    }
    case CALC_KEY_CUBE: {
        double v = calc_value(c);
        set_operand(c, v * v * v);
        break;
    }
    case CALC_KEY_PI:        set_operand(c, M_PI); break;
    case CALC_KEY_E:         set_operand(c, M_E); break;
    case CALC_KEY_EQ:        key_eq(c); break;
    case CALC_KEY_CLEAR:     clear_calc(c); break;
    default: break;
    }
}

void calc_store(calc_t *c, int r) {
    if (c->error || r < 0 || r >= CALC_REGS)
        return;
    c->regs[r] = calc_value(c);
    c->reg_set[r] = true;
}

void calc_recall(calc_t *c, int r) {
    if (c->error || r < 0 || r >= CALC_REGS || !c->reg_set[r])
        return;
    set_operand(c, c->regs[r]);
}

void calc_format_value(double v, char *buf, size_t n) {
    if (v == 0.0)
        v = 0.0; /* normalise -0 */
    snprintf(buf, n, "%.12g", v);
}

void calc_display(const calc_t *c, char *buf, size_t n) {
    if (c->error) {
        snprintf(buf, n, "Error");
        return;
    }
    if (c->typing) {
        snprintf(buf, n, "%s", c->entry);
        return;
    }
    calc_format_value(c->value, buf, n);
}

/* Integral and representable in int64? (2^63 is exact in double; the largest
 * double below it is fine, so accept v in [INT64_MIN, 2^63).) */
static bool integral_i64(double v, int64_t *out) {
    if (!isfinite(v) || v != floor(v))
        return false;
    if (v < -9223372036854775808.0 || v >= 9223372036854775808.0)
        return false;
    *out = (int64_t)v;
    return true;
}

bool calc_hex(const calc_t *c, char *buf, size_t n) {
    buf[0] = '\0';
    int64_t i;
    if (c->error || !integral_i64(calc_value(c), &i))
        return false;
    /* Two's complement for negatives, i.e. the raw 64-bit pattern. */
    snprintf(buf, n, "0x%llX", (unsigned long long)(uint64_t)i);
    return true;
}

bool calc_bin(const calc_t *c, char *buf, size_t n) {
    buf[0] = '\0';
    int64_t i;
    if (c->error || !integral_i64(calc_value(c), &i))
        return false;
    if (i < 0 || i > 0xFFFFFFFFLL)
        return false;
    uint32_t u = (uint32_t)i;
    int nibbles = 1;
    while (nibbles < 8 && (u >> (4 * nibbles)) != 0)
        nibbles++;
    size_t need = (size_t)nibbles * 5; /* 4 bits + separator/NUL per nibble */
    if (n < need)
        return false;
    char *p = buf;
    for (int nb = nibbles - 1; nb >= 0; nb--) {
        for (int b = 3; b >= 0; b--)
            *p++ = (char)('0' + ((u >> (4 * nb + b)) & 1));
        *p++ = (nb > 0) ? ' ' : '\0';
    }
    return true;
}

double calc_in_to_mm(double v) { return v * CALC_MM_PER_IN; }
double calc_mm_to_in(double v) { return v / CALC_MM_PER_IN; }
double calc_mm_to_px(double v) { return v * CALC_PX_PER_MM; }
double calc_px_to_mm(double v) { return v / CALC_PX_PER_MM; }
