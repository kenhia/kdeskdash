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
 * 'C'(clear), '<'(backspace), 'n'(neg), 's'(sqr), 'c'(cube), 'p'(pi), 'e'(e),
 * 'r'(sqrt), 'i'(1/x), 'S'/'K'/'T'(sin/cos/tan), 'I'(INV), 'D'(deg/rad toggle),
 * 'E'(CE). */
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
        case 'r': calc_key(c, CALC_KEY_SQRT); break;
        case 'i': calc_key(c, CALC_KEY_RECIP); break;
        case 'S': calc_key(c, CALC_KEY_SIN); break;
        case 'K': calc_key(c, CALC_KEY_COS); break;
        case 'T': calc_key(c, CALC_KEY_TAN); break;
        case 'I': calc_key(c, CALC_KEY_INV); break;
        case 'D': calc_key(c, CALC_KEY_DRG); break;
        case 'E': calc_key(c, CALC_KEY_CE); break;
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


/* --- sprint 036: WI #509 post-live-test follow-ups ----------------------- */

static void test_root_and_reciprocal(void) {
    check_display("9r", "3", "sqrt(9) = 3");
    check_display("2r", "1.41421356237", "sqrt(2) to display precision");
    check_display("0r", "0", "sqrt(0) = 0");
    /* A negative root is Error, not NaN leaking into the display. */
    check_display("9nr", "Error", "sqrt of a negative is an error");

    check_display("4i", "0.25", "1/4 = 0.25");
    check_display("4ii", "4", "1/x is its own inverse");
    check_display("0i", "Error", "1/0 is an error");

    /* Both are unary operands: they feed a pending binary op. */
    check_display("2+9r=", "5", "2 + sqrt(9) = 5");
    check_display("1+4i=", "1.25", "1 + 1/4 = 1.25");
}

static void test_trig(void) {
    /* Degrees is the default, so the exact desk-calculator answers hold. */
    check_display("30S", "0.5", "sin 30deg = 0.5");
    check_display("60K", "0.5", "cos 60deg = 0.5");
    check_display("45T", "1", "tan 45deg = 1");
    check_display("0S", "0", "sin 0 = 0");

    /* INV arms the inverse for exactly one trig key. */
    check_display("0.5IS", "30", "INV sin 0.5 = 30deg");
    check_display("0.5IK", "60", "INV cos 0.5 = 60deg");
    check_display("1IT", "45", "INV tan 1 = 45deg");
    check_display("2IS", "Error", "INV sin out of domain is an error");

    /* The modifier is consumed, not sticky: the second sin is a plain sin. */
    check_display("0.5ISS", "0.5", "INV is consumed by one trig key");
    /* Pressing INV twice disarms it. */
    check_display("30IIS", "0.5", "INV twice disarms");

    /* D toggles to radians; pi radians is a half turn. */
    check_display("Dp S", "1.22464679915e-16", "sin(pi) in radians ~ 0");
    check_display("D0K", "1", "cos 0 = 1 in radians too");
    check_display("D1IT", "0.785398163397", "INV tan 1 = pi/4 radians");
    /* And back again. */
    check_display("DD30S", "0.5", "toggling twice returns to degrees");

    /* tan of a right angle has no value; floating point would otherwise show
     * 1.6e16, which reads as a real answer. Degrees only — in radians the
     * exact argument is not typeable. */
    check_display("90T", "Error", "tan 90deg is an error");
    check_display("90nT", "Error", "tan -90deg is an error");
    check_display("270T", "Error", "tan 270deg is an error");
    check_display("89T", "57.2899616308", "tan 89deg is a number");

    /* The angle mode is state, and survives a clear. */
    calc_t c;
    calc_init(&c);
    check(c.degrees, "fresh calculator is in degrees");
    script(&c, "D");
    check(!c.degrees, "DRG toggles to radians");
    script(&c, "C");
    check(!c.degrees, "CLEAR does not reset the angle mode");
}

static void test_clear_entry(void) {
    /* CE drops the operand being typed and leaves the pending op alone. */
    check_display("2+5E3=", "5", "CE replaces the operand, keeps the op");
    check_display("2+5E=", "2", "CE leaves the operand zero");
    check_display("2+5E", "0", "CE shows 0");
    /* C, by contrast, takes the pending op with it. */
    check_display("2+5C3=", "3", "CLEAR drops the pending op too");

    /* CE recovers from an error, like C does. */
    check_display("0i E 7", "7", "CE recovers from an error state");

    /* Neither clear touches the registers. */
    calc_t c;
    calc_init(&c);
    script(&c, "42");
    calc_store(&c, 2);
    script(&c, "E");
    check(c.reg_set[2] && c.regs[2] == 42.0, "CE keeps the registers");
    script(&c, "C");
    check(c.reg_set[2] && c.regs[2] == 42.0, "CLEAR keeps the registers");

    /* CE disarms a pending INV so the modifier cannot outlive the entry. */
    check_display("I E 30S", "0.5", "CE disarms INV");
}

static void test_register_serialization(void) {
    char buf[CALC_REGS_STR_MAX];
    calc_t c;

    /* An untouched calculator serializes to the empty string. */
    calc_init(&c);
    calc_regs_serialize(&c, buf, sizeof(buf));
    check_str(buf, "", "no registers set serializes empty");

    /* Round trip: every set register comes back bit-identical, and the unset
     * ones stay unset. %.17g is what makes the bit-identical part true. */
    calc_init(&c);
    script(&c, "2r"); /* sqrt(2), a value no shorter format survives */
    calc_store(&c, 0);
    script(&c, "C1.5n");
    calc_store(&c, 3);
    script(&c, "C0");
    calc_store(&c, 5);
    calc_regs_serialize(&c, buf, sizeof(buf));

    calc_t d;
    calc_init(&d);
    check(calc_regs_parse(&d, buf), "a serialized line parses");
    for (int i = 0; i < CALC_REGS; i++) {
        char what[48];
        snprintf(what, sizeof(what), "register %d survives the round trip", i);
        check(d.reg_set[i] == c.reg_set[i] && d.regs[i] == c.regs[i], what);
    }
    check(!d.reg_set[1] && !d.reg_set[2] && !d.reg_set[4],
          "unset registers stay unset across the round trip");

    /* The shape is stable and readable, which is half the point of a string. */
    calc_init(&c);
    script(&c, "7");
    calc_store(&c, 1);
    calc_regs_serialize(&c, buf, sizeof(buf));
    check_str(buf, "1:7", "one register serializes as idx:value");

    /* All-or-nothing: a malformed token rejects the whole line rather than
     * restoring the registers that happened to come before it. */
    static const char *bad[] = {
        "0:1 1:notanumber", /* value that is not a number          */
        "0:1 9:2",          /* index past the end                  */
        "0:1 -1:2",         /* negative index                      */
        "0:1 2",            /* a token with no colon               */
        "0:1 :2",           /* a colon with no index               */
        "0:1 1:",           /* a colon with no value               */
        "0:1 1:inf",        /* non-finite values are not storable  */
        "0:1 1:nan",
        "",     /* nothing to restore is not a restore */
        "   ",  /* nor is whitespace                   */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        calc_init(&d);
        calc_store(&d, 4); /* a register already holding something */
        char what[96];
        snprintf(what, sizeof(what), "rejects malformed \"%s\"", bad[i]);
        check(!calc_regs_parse(&d, bad[i]), what);
        snprintf(what, sizeof(what), "leaves state alone for \"%s\"", bad[i]);
        check(d.reg_set[4] && !d.reg_set[0] && !d.reg_set[1], what);
    }

    /* Parsing replaces the register file wholesale — a register absent from
     * the line is unset afterwards, not left over from before. */
    calc_init(&d);
    script(&d, "99");
    calc_store(&d, 2);
    check(calc_regs_parse(&d, "0:5"), "a good line parses");
    check(d.reg_set[0] && d.regs[0] == 5.0, "the line's register is set");
    check(!d.reg_set[2], "a register absent from the line is cleared");

    /* Parsing must not disturb the calculation in progress. */
    calc_init(&d);
    script(&d, "12+3");
    calc_regs_parse(&d, "0:5");
    char disp[32];
    calc_display(&d, disp, sizeof(disp));
    check_str(disp, "3", "restoring registers leaves the entry alone");

    /* A short buffer truncates rather than overflowing. */
    calc_init(&c);
    script(&c, "123456789");
    for (int i = 0; i < CALC_REGS; i++)
        calc_store(&c, i);
    char tiny[8];
    calc_regs_serialize(&c, tiny, sizeof(tiny));
    check(strlen(tiny) < sizeof(tiny), "serialize respects a short buffer");
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
    test_root_and_reciprocal();
    test_trig();
    test_clear_entry();
    test_register_serialization();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_calc: all passed\n");
    return 0;
}
