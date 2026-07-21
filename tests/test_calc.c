/**
 * @file test_calc.c
 * Host-only unit tests for the pure calculator core: key sequences in,
 * display/hex/bin/conversion strings out.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "calc.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* Feed a compact key script: digits, '.', '+','-','*','/', '^'(pow), '='(eq),
 * 'C'(clear), '<'(backspace), 'n'(neg), 's'(sqr), 'c'(cube), 'p'(pi), 'e'(e). */
static void script(calc_t *c, const char *keys) {
    for (const char *p = keys; *p; p++) {
        switch (*p) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            calc_key(c, (calc_key_t)(CALC_KEY_D0 + (*p - '0')));
            break;
        case '.': calc_key(c, CALC_KEY_DOT); break;
        case '+': calc_key(c, CALC_KEY_ADD); break;
        case '-': calc_key(c, CALC_KEY_SUB); break;
        case '*': calc_key(c, CALC_KEY_MUL); break;
        case '/': calc_key(c, CALC_KEY_DIV); break;
        case '^': calc_key(c, CALC_KEY_POW); break;
        case '=': calc_key(c, CALC_KEY_EQ); break;
        case 'C': calc_key(c, CALC_KEY_CLEAR); break;
        case '<': calc_key(c, CALC_KEY_BACKSPACE); break;
        case 'n': calc_key(c, CALC_KEY_NEG); break;
        case 's': calc_key(c, CALC_KEY_SQR); break;
        case 'c': calc_key(c, CALC_KEY_CUBE); break;
        case 'p': calc_key(c, CALC_KEY_PI); break;
        case 'e': calc_key(c, CALC_KEY_E); break;
        }
    }
}

static void check_display(const char *keys, const char *want, const char *what) {
    calc_t c;
    calc_init(&c);
    script(&c, keys);
    char buf[32];
    calc_display(&c, buf, sizeof(buf));
    check_str(buf, want, what);
}

static void test_entry_editing(void) {
    check_display("", "0", "fresh display is 0");
    check_display("123", "123", "digit entry");
    check_display("007", "7", "leading zeros absorbed");
    check_display("1.05", "1.05", "decimal entry shows as typed");
    check_display(".5", "0.5", "dot first becomes 0.5");
    check_display("1.2.3", "1.23", "second dot ignored");
    check_display("12<", "1", "backspace chops");
    check_display("1<<", "0", "backspace to empty is 0");
    check_display("12n", "-12", "neg while typing");
    check_display("12nn", "12", "neg toggles back");
    check_display("5=n", "-5", "neg on a result");
    check_display("1234567890123456789", "123456789012345",
                  "entry capped at 15 digits");
}

static void test_immediate_execution(void) {
    check_display("2+3=", "5", "2+3");
    check_display("2+3*4=", "20", "immediate exec: (2+3)*4");
    check_display("10/4=", "2.5", "division");
    check_display("2^10=", "1024", "pow");
    check_display("7-10=", "-3", "negative result");
    check_display("2+3+", "5", "op key shows running total");
    check_display("2+*3=", "6", "second op replaces pending (2*3)");
    check_display("5+=", "10", "eq with no rhs uses display (classic 5+=10)");
    check_display("2+3=4+1=", "5", "new calc after equals");
}

static void test_unary_and_constants(void) {
    check_display("3s", "9", "square");
    check_display("2c", "8", "cube");
    check_display("2+3s=", "11", "unary applies to rhs operand");
    check_display("4snn", "16", "unary result is a value (neg twice)");
    calc_t c;
    calc_init(&c);
    script(&c, "p");
    check(fabs(calc_value(&c) - 3.14159265358979) < 1e-12, "pi");
    script(&c, "C2*e=");
    check(fabs(calc_value(&c) - 5.43656365691809) < 1e-11, "2*e");
}

static void test_errors(void) {
    check_display("5/0=", "Error", "divide by zero");
    check_display("5/0=+", "Error", "ops ignored in error state");
    check_display("5/0=C", "0", "clear recovers");
    check_display("5/0=7", "7", "digit recovers and starts fresh");
    /* Overflow: (10^100)^100 exceeds double range. */
    check_display("10^100=^100=", "Error", "overflow to error");

    calc_t c;
    calc_init(&c);
    script(&c, "42");
    calc_store(&c, 0);
    script(&c, "C5/0=");
    check(c.error, "in error state");
    calc_store(&c, 0); /* must not clobber R0 with garbage */
    script(&c, "C");
    calc_recall(&c, 0);
    check(calc_value(&c) == 42.0, "registers survive error and clear");
}

static void test_registers(void) {
    calc_t c;
    calc_init(&c);

    calc_recall(&c, 0);
    check(calc_value(&c) == 0.0 && !c.typing, "recall of unset reg is a no-op");

    script(&c, "25.4");
    calc_store(&c, 2); /* store mid-typing captures the entry value */
    script(&c, "C10*");
    calc_recall(&c, 2);
    script(&c, "=");
    check(calc_value(&c) == 254.0, "recall acts as typed operand: 10*R2");

    calc_store(&c, 5);
    check(c.reg_set[5] && c.regs[5] == 254.0, "store captures result");
    calc_store(&c, -1);
    calc_store(&c, CALC_REGS);
    calc_recall(&c, 99); /* bad indices are no-ops, not crashes */
}

static void test_display_formatting(void) {
    check_display("1/3=", "0.333333333333", "12 sig figs");
    check_display("0n", "-0", "typed -0 shows as typed");
    check_display("0n=", "0", "-0 result normalised");
    check_display("2/10000000000=", "2e-10", "small magnitude goes exponential");
}

static void test_hex_bin(void) {
    calc_t c;
    char buf[80];
    calc_init(&c);

    script(&c, "1234");
    check(calc_hex(&c, buf, sizeof(buf)), "hex for integral");
    check_str(buf, "0x4D2", "hex 1234");
    check(calc_bin(&c, buf, sizeof(buf)), "bin for integral");
    check_str(buf, "0100 1101 0010", "bin 1234 nibble-grouped");

    script(&c, "C0");
    calc_bin(&c, buf, sizeof(buf));
    check_str(buf, "0000", "bin zero is one nibble");

    script(&c, "C1.5");
    check(!calc_hex(&c, buf, sizeof(buf)) && buf[0] == '\0',
          "no hex for fractional");
    check(!calc_bin(&c, buf, sizeof(buf)), "no bin for fractional");

    script(&c, "C2n"); /* -2 */
    check(calc_hex(&c, buf, sizeof(buf)), "hex for negative integral");
    check_str(buf, "0xFFFFFFFFFFFFFFFE", "two's complement -2");
    check(!calc_bin(&c, buf, sizeof(buf)), "no bin for negative");

    script(&c, "C2^32=");
    check(!calc_bin(&c, buf, sizeof(buf)), "no bin at 2^32");
    script(&c, "C2^32=-1=");
    check(calc_bin(&c, buf, sizeof(buf)), "bin at 2^32-1");
    check(strlen(buf) == 8 * 4 + 7, "2^32-1 is 8 nibbles + 7 spaces");

    script(&c, "C2^64=");
    check(!calc_hex(&c, buf, sizeof(buf)), "no hex at 2^64 (exceeds int64)");
    script(&c, "C5/0=");
    check(!calc_hex(&c, buf, sizeof(buf)), "no hex in error state");
}

static void test_conversions(void) {
    check(calc_in_to_mm(1.0) == 25.4, "1 in = 25.4 mm");
    check(fabs(calc_mm_to_in(25.4) - 1.0) < 1e-15, "25.4 mm = 1 in");
    check(fabs(calc_mm_to_px(19.5) - 149.955) < 1e-9, "ruler cal: 19.5mm ~ 150px");
    check(fabs(calc_px_to_mm(calc_mm_to_px(7.0)) - 7.0) < 1e-12,
          "px<->mm round trip");
    check(fabs(calc_in_to_mm(calc_mm_to_in(3.0)) - 3.0) < 1e-12,
          "in<->mm round trip");
}

int main(void) {
    test_entry_editing();
    test_immediate_execution();
    test_unary_and_constants();
    test_errors();
    test_registers();
    test_display_formatting();
    test_hex_bin();
    test_conversions();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_calc: all passed\n");
    return 0;
}
