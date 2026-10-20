// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: calc — inline calculator
// Usage: = EXPRESSION  or  calc EXPRESSION

#include "builtin.h"
#include "value_output.h"
#include <inttypes.h>
#include <core/env.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static double calc_parse_expr(jash_state_t* s, const char** p);
static void calc_skip_ws(const char** p);

static void calc_skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static double calc_parse_number(const char** p) {
    calc_skip_ws(p);
    double val = 0;
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    else if (**p == '+') { (*p)++; }

    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    if (**p == '.') {
        (*p)++;
        double frac = 0.1;
        while (**p >= '0' && **p <= '9') {
            val += (**p - '0') * frac;
            frac /= 10;
            (*p)++;
        }
    }
    return neg ? -val : val;
}

static double calc_parse_variable(jash_state_t* s, const char** p) {
    calc_skip_ws(p);
    
    // Check if it's a variable name (starts with letter or underscore)
    if ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || **p == '_') {
        char varname[64];
        int i = 0;
        
        // Read variable name
        while ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || 
               (**p >= '0' && **p <= '9') || **p == '_') {
            if (i < 63) varname[i++] = **p;
            (*p)++;
        }
        varname[i] = '\0';
        
        // Look up variable value
        const char* val = jash_env_get(s, varname);
        if (val) {
            // Try to parse as number
            char* endptr;
            double num = strtod(val, &endptr);
            if (endptr != val) {
                return num;
            }
        }
        return 0; // Variable not found or not a number
    }
    
    return calc_parse_number(p);
}

static double calc_parse_function(jash_state_t* s, const char** p) {
    calc_skip_ws(p);

    if (strncmp(*p, "sqrt(", 5) == 0) {
        *p += 5;
        double arg = calc_parse_expr(s, p);
        calc_skip_ws(p);
        if (**p == ')') (*p)++;
        return sqrt(arg);
    }
    if (strncmp(*p, "sin(", 4) == 0) {
        *p += 4;
        double arg = calc_parse_expr(s, p);
        calc_skip_ws(p);
        if (**p == ')') (*p)++;
        return sin(arg);
    }
    if (strncmp(*p, "cos(", 4) == 0) {
        *p += 4;
        double arg = calc_parse_expr(s, p);
        calc_skip_ws(p);
        if (**p == ')') (*p)++;
        return cos(arg);
    }
    if (strncmp(*p, "abs(", 4) == 0) {
        *p += 4;
        double arg = calc_parse_expr(s, p);
        calc_skip_ws(p);
        if (**p == ')') (*p)++;
        return fabs(arg);
    }

    return calc_parse_variable(s, p);
}

static double calc_parse_factor(jash_state_t* s, const char** p) {
    calc_skip_ws(p);

    if (**p == '(') {
        (*p)++;
        double val = calc_parse_expr(s, p);
        calc_skip_ws(p);
        if (**p == ')') (*p)++;
        return val;
    }

    return calc_parse_function(s, p);
}

static double calc_parse_term(jash_state_t* s, const char** p) {
    double val = calc_parse_factor(s, p);

    for (;;) {
        calc_skip_ws(p);
        if (**p == '*') {
            (*p)++;
            if (**p == '*') {
                (*p)++;
                double exp = calc_parse_factor(s, p);
                val = pow(val, exp);
            } else {
                val *= calc_parse_factor(s, p);
            }
        } else if (**p == '/') {
            (*p)++;
            double div = calc_parse_factor(s, p);
            if (div != 0) val /= div;
            else { val = 0; }
        } else if (**p == '%') {
            (*p)++;
            double mod = calc_parse_factor(s, p);
            if (mod != 0) val = fmod(val, mod);
            else { val = 0; }
        } else {
            break;
        }
    }
    return val;
}

static double calc_parse_expr(jash_state_t* s, const char** p) {
    double val = calc_parse_term(s, p);

    for (;;) {
        calc_skip_ws(p);
        if (**p == '+') { (*p)++; val += calc_parse_term(s, p); }
        else if (**p == '-') { (*p)++; val -= calc_parse_term(s, p); }
        else { break; }
    }
    return val;
}

static void calc_print_result(jash_writer_t w, void* ctx, double result) {
    if (isfinite(result) && fabs(result) < 1e15 && result == (double)(long long)result) {
        long long iv = (long long)result;
        if (iv == 0) {
            jash_writes(w, ctx, "0\n");
            return;
        }
        char buf[32];
        int neg = 0;
        unsigned bi = 0;
        long long tmp = iv;
        if (tmp < 0) { neg = 1; tmp = -tmp; }
        char rev[32];
        unsigned ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (char)(tmp % 10); tmp /= 10; }
        if (neg) buf[bi++] = '-';
        while (ri > 0) buf[bi++] = rev[--ri];
        buf[bi] = '\0';
        jash_writes(w, ctx, buf);
        jash_writes(w, ctx, "\n");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6g", result);
        jash_writes(w, ctx, buf);
        jash_writes(w, ctx, "\n");
    }
}

/* Structured arithmetic is exact in the declared bounded rational domain.
 * JDV1 has no floating primitive: approximations use an explicit versioned
 * binary64 record, never an unversioned kind or a numeric string coercion. */
typedef struct {
    uint64_t numerator, denominator;
    int negative, approximate;
    double binary;
} calc_number_t;
typedef struct {
    jash_state_t* state;
    const char* at;
    unsigned depth, nodes;
    int error;
} calc_reader_t;

static calc_number_t calc_zero(void) { return (calc_number_t){.denominator = 1}; }
static calc_number_t calc_fail(calc_reader_t* reader, int code) {
    if (!reader->error) reader->error = code;
    return calc_zero();
}
static uint64_t calc_gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t next = a % b; a = b; b = next; }
    return a;
}
static calc_number_t calc_normalize(calc_number_t value) {
    if (!value.numerator) return calc_zero();
    uint64_t gcd = calc_gcd(value.numerator, value.denominator);
    value.numerator /= gcd; value.denominator /= gcd;
    return value;
}
static int calc_mul_u64(uint64_t a, uint64_t b, uint64_t* result) {
    if (b && a > UINT64_MAX / b) return -1;
    *result = a * b;
    return 0;
}
static double calc_binary(calc_number_t value) {
    if (value.approximate) return value.binary;
    double result = (double)value.numerator / (double)value.denominator;
    return value.negative ? -result : result;
}
static calc_number_t calc_approx(calc_reader_t* reader, double value) {
    if (!isfinite(value)) return calc_fail(reader, EDOM);
    return (calc_number_t){.denominator = 1, .approximate = 1, .binary = value};
}
static calc_number_t calc_negate(calc_number_t value) {
    if (value.approximate) value.binary = -value.binary;
    else if (value.numerator) value.negative = !value.negative;
    return value;
}
static calc_number_t calc_add(calc_reader_t* reader, calc_number_t a, calc_number_t b) {
    if (reader->error) return calc_zero();
    if (a.approximate || b.approximate) return calc_approx(reader, calc_binary(a) + calc_binary(b));
    uint64_t divisor = calc_gcd(a.denominator, b.denominator), left, right, denominator;
    if (calc_mul_u64(a.numerator, b.denominator / divisor, &left) ||
        calc_mul_u64(b.numerator, a.denominator / divisor, &right) ||
        calc_mul_u64(a.denominator, b.denominator / divisor, &denominator)) return calc_fail(reader, EOVERFLOW);
    calc_number_t result = {.denominator = denominator, .negative = a.negative};
    if (a.negative == b.negative) {
        if (right > UINT64_MAX - left) return calc_fail(reader, EOVERFLOW);
        result.numerator = left + right;
    } else if (left >= right) result.numerator = left - right;
    else { result.numerator = right - left; result.negative = b.negative; }
    return calc_normalize(result);
}
static calc_number_t calc_multiply(calc_reader_t* reader, calc_number_t a, calc_number_t b, int divide) {
    if (reader->error) return calc_zero();
    if (divide && (b.approximate ? b.binary == 0 : b.numerator == 0)) return calc_fail(reader, EDOM);
    if (a.approximate || b.approximate) return calc_approx(reader,
        divide ? calc_binary(a) / calc_binary(b) : calc_binary(a) * calc_binary(b));
    if (divide) { uint64_t swap = b.numerator; b.numerator = b.denominator; b.denominator = swap; }
    uint64_t left = calc_gcd(a.numerator, b.denominator);
    uint64_t right = calc_gcd(b.numerator, a.denominator);
    a.numerator /= left; b.denominator /= left;
    b.numerator /= right; a.denominator /= right;
    calc_number_t result = {.negative = a.negative != b.negative};
    if (calc_mul_u64(a.numerator, b.numerator, &result.numerator) ||
        calc_mul_u64(a.denominator, b.denominator, &result.denominator)) return calc_fail(reader, EOVERFLOW);
    return calc_normalize(result);
}
static calc_number_t calc_modulo(calc_reader_t* reader, calc_number_t a, calc_number_t b) {
    if (reader->error) return calc_zero();
    if (b.approximate ? b.binary == 0 : b.numerator == 0) return calc_fail(reader, EDOM);
    if (a.approximate || b.approximate) return calc_approx(reader, fmod(calc_binary(a), calc_binary(b)));
    calc_number_t quotient = calc_multiply(reader, a, b, 1);
    quotient.numerator /= quotient.denominator; quotient.denominator = 1;
    return calc_add(reader, a, calc_negate(calc_multiply(reader, b, quotient, 0)));
}
static calc_number_t calc_power(calc_reader_t* reader, calc_number_t a, calc_number_t b) {
    if (reader->error) return calc_zero();
    if (a.approximate || b.approximate || b.denominator != 1)
        return calc_approx(reader, pow(calc_binary(a), calc_binary(b)));
    if (b.numerator > 4096u) return calc_fail(reader, E2BIG);
    if (b.negative) {
        if (!a.numerator) return calc_fail(reader, EDOM);
        uint64_t swap = a.numerator; a.numerator = a.denominator; a.denominator = swap;
    }
    calc_number_t result = {.numerator = 1, .denominator = 1};
    uint64_t exponent = b.numerator;
    while (exponent && !reader->error) {
        if (exponent & 1u) result = calc_multiply(reader, result, a, 0);
        exponent >>= 1;
        if (exponent) a = calc_multiply(reader, a, a, 0);
    }
    return result;
}
static uint64_t calc_isqrt(uint64_t value) {
    uint64_t low = 0, high = UINT64_C(1) << 32;
    while (low + 1 < high) {
        uint64_t middle = low + (high - low) / 2;
        if (middle <= value / middle) low = middle;
        else high = middle;
    }
    return low;
}
static calc_number_t calc_numeric(calc_reader_t* reader) {
    const char* begin = reader->at;
    const char* cursor = begin;
    while (*cursor >= '0' && *cursor <= '9') cursor++;
    const char* fraction = NULL;
    if (*cursor == '.') {
        fraction = ++cursor;
        while (*cursor >= '0' && *cursor <= '9') cursor++;
    }
    if (cursor == begin || (fraction == begin + 1 && cursor == fraction)) return calc_fail(reader, EINVAL);
    const char* end = cursor;
    if (fraction) while (end > fraction && end[-1] == '0') end--;
    calc_number_t value = calc_zero();
    for (const char* p = begin; p < end; p++) {
        if (*p == '.') continue;
        unsigned digit = (unsigned)(*p - '0');
        if (value.numerator > (UINT64_MAX - digit) / 10u) return calc_fail(reader, EOVERFLOW);
        value.numerator = value.numerator * 10u + digit;
    }
    if (fraction) for (const char* p = fraction; p < end; p++) {
        if (value.denominator > UINT64_MAX / 10u) return calc_fail(reader, EOVERFLOW);
        value.denominator *= 10u;
    }
    reader->at = cursor;
    return calc_normalize(value);
}
static int calc_ident(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
static calc_number_t calc_checked_expr(calc_reader_t* reader);
static calc_number_t calc_unary(calc_reader_t* reader);
static calc_number_t calc_primary(calc_reader_t* reader) {
    calc_skip_ws(&reader->at);
    if (++reader->nodes > 4096u) return calc_fail(reader, E2BIG);
    if (*reader->at == '(') {
        reader->at++;
        calc_number_t value = calc_checked_expr(reader);
        calc_skip_ws(&reader->at);
        if (*reader->at != ')') return calc_fail(reader, EINVAL);
        reader->at++;
        return value;
    }
    if (calc_ident((unsigned char)*reader->at)) {
        char name[256]; size_t length = 0;
        while (calc_ident((unsigned char)*reader->at) || (*reader->at >= '0' && *reader->at <= '9')) {
            if (length + 1u == sizeof(name)) return calc_fail(reader, E2BIG);
            name[length++] = *reader->at++;
        }
        name[length] = '\0';
        calc_skip_ws(&reader->at);
        if (*reader->at == '(') {
            int function = !strcmp(name, "abs") ? 1 : !strcmp(name, "sqrt") ? 2
                : !strcmp(name, "sin") ? 3 : !strcmp(name, "cos") ? 4 : 0;
            if (!function) return calc_fail(reader, EINVAL);
            reader->at++;
            calc_number_t arg = calc_checked_expr(reader);
            calc_skip_ws(&reader->at);
            if (*reader->at != ')') return calc_fail(reader, EINVAL);
            reader->at++;
            if (reader->error) return calc_zero();
            if (function == 1) {
                if (arg.approximate) arg.binary = fabs(arg.binary);
                else arg.negative = 0;
                return arg;
            }
            if (function == 2 && !arg.approximate) {
                if (arg.negative) return calc_fail(reader, EDOM);
                uint64_t n = calc_isqrt(arg.numerator), d = calc_isqrt(arg.denominator);
                if (n * n == arg.numerator && d * d == arg.denominator)
                    return (calc_number_t){.numerator = n, .denominator = d};
            }
            double binary = calc_binary(arg);
            return calc_approx(reader, function == 2 ? sqrt(binary) : function == 3 ? sin(binary) : cos(binary));
        }
        const char* variable = jash_env_get(reader->state, name);
        if (!variable) return calc_zero();
        calc_reader_t content = {.state = reader->state, .at = variable};
        calc_skip_ws(&content.at);
        int negative = *content.at == '-';
        if (*content.at == '+' || *content.at == '-') content.at++;
        calc_number_t value = calc_numeric(&content);
        calc_skip_ws(&content.at);
        if (content.error || *content.at) return calc_fail(reader, content.error ? content.error : EINVAL);
        return negative ? calc_negate(value) : value;
    }
    return calc_numeric(reader);
}
static calc_number_t calc_checked_power(calc_reader_t* reader) {
    calc_number_t base = calc_primary(reader);
    calc_skip_ws(&reader->at);
    if (!reader->error && reader->at[0] == '*' && reader->at[1] == '*') {
        reader->at += 2;
        calc_number_t exponent = calc_unary(reader);
        return calc_power(reader, base, exponent);
    }
    return base;
}
static calc_number_t calc_unary(calc_reader_t* reader) {
    if (reader->error) return calc_zero();
    if (++reader->depth > 64u) { reader->depth--; return calc_fail(reader, E2BIG); }
    calc_skip_ws(&reader->at);
    calc_number_t value;
    if (*reader->at == '-' || *reader->at == '+') {
        int negative = *reader->at++ == '-';
        value = calc_unary(reader);
        if (negative) value = calc_negate(value);
    } else value = calc_checked_power(reader);
    reader->depth--;
    return value;
}
static calc_number_t calc_checked_term(calc_reader_t* reader) {
    calc_number_t value = calc_unary(reader);
    for (;;) {
        calc_skip_ws(&reader->at);
        char op = *reader->at;
        if (reader->error || (op != '*' && op != '/' && op != '%')) break;
        reader->at++;
        calc_number_t next = calc_unary(reader);
        value = op == '%' ? calc_modulo(reader, value, next)
            : calc_multiply(reader, value, next, op == '/');
    }
    return value;
}
static calc_number_t calc_checked_expr(calc_reader_t* reader) {
    calc_number_t value = calc_checked_term(reader);
    for (;;) {
        calc_skip_ws(&reader->at);
        char op = *reader->at;
        if (reader->error || (op != '+' && op != '-')) break;
        reader->at++;
        calc_number_t next = calc_checked_term(reader);
        value = calc_add(reader, value, op == '-' ? calc_negate(next) : next);
    }
    return value;
}
static jdv_value_t calc_literal(const char* text) {
    return (jdv_value_t){.kind = JDV_SYMBOL, .as.slice = {(const uint8_t*)text, strlen(text)}};
}
static int calc_structured(jash_state_t* state, jash_writer_t writer, void* context,
                            const char* expression, int mode) {
    calc_reader_t reader = {.state = state, .at = expression};
    calc_number_t result = calc_zero();
    size_t length = strlen(expression);
    if (length > 16384u) reader.error = E2BIG;
    else if ((!strncmp(expression, "--json", 6) && (!expression[6] || expression[6] == ' ' || expression[6] == '\t')) ||
             (!strncmp(expression, "--data", 6) && (!expression[6] || expression[6] == ' ' || expression[6] == '\t')) ||
             (!strncmp(expression, "--data-v2", 9) && (!expression[9] || expression[9] == ' ' || expression[9] == '\t')))
        reader.error = EINVAL;
    else result = calc_checked_expr(&reader);
    calc_skip_ws(&reader.at);
    if (*reader.at && !reader.error) reader.error = EINVAL;
    jdv_limits_t limits = JDV_DEFAULT_LIMITS;
    jdv_schema_t schema = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
    if (reader.error) {
        const char* reason = reader.error == EOVERFLOW ? "numeric_overflow"
            : reader.error == E2BIG ? "expression_limit" : reader.error == EDOM ? "numeric_domain" : "invalid_expression";
        return mode == 1 ? jash_structured_error(writer, context, "calc", reason, 2)
            : value_output_data(writer, context, NULL, 0, NULL, &schema, &limits, 65536u, 2, reason);
    }
    void* storage = malloc(128u * 1024u);
    if (!storage) return 125;
    jdv_arena_t arena; jdv_arena_init(&arena, storage, 128u * 1024u);
    jdv_record_input_t fields[4];
    if (!result.approximate) {
        fields[0] = (jdv_record_input_t){JDV_LITERAL("schema"), calc_literal("jash.number.rational.v1")};
        fields[1] = (jdv_record_input_t){JDV_LITERAL("negative"), jdv_value_bool(result.negative)};
        fields[2] = (jdv_record_input_t){JDV_LITERAL("numerator"), jdv_value_u64(result.numerator)};
        fields[3] = (jdv_record_input_t){JDV_LITERAL("denominator"), jdv_value_u64(result.denominator)};
    } else {
        int exponent = 0;
        double fraction = frexp(result.binary, &exponent);
        int64_t significand = (int64_t)ldexp(fraction, 53);
        exponent -= 53;
        if (!significand) exponent = 0;
        else while (significand % 2 == 0) { significand /= 2; exponent++; }
        fields[0] = (jdv_record_input_t){JDV_LITERAL("schema"), calc_literal("jash.number.binary64.v1")};
        fields[1] = (jdv_record_input_t){JDV_LITERAL("significand"), jdv_value_i64(significand)};
        fields[2] = (jdv_record_input_t){JDV_LITERAL("exponent2"), jdv_value_i64(exponent)};
        fields[3] = (jdv_record_input_t){JDV_LITERAL("negative_zero"), jdv_value_bool(result.binary == 0 && signbit(result.binary))};
    }
    jdv_value_t number = jdv_value_nothing(), row = jdv_value_nothing();
    int rc = jdv_value_record(&arena, fields, 4, &number);
    jdv_value_t source = {.kind = JDV_STRING, .as.slice = {(const uint8_t*)expression, length}};
    jdv_record_input_t outer[] = {
        {JDV_LITERAL("schema"), calc_literal("jash.calc.v1")},
        {JDV_LITERAL("expression"), source},
        {JDV_LITERAL("exact"), jdv_value_bool(!result.approximate)},
        {JDV_LITERAL("result"), number}
    };
    if (!rc) rc = jdv_value_record(&arena, outer, 4, &row);
    if (!rc) rc = mode == 1 ? value_output_json(writer, context, "calc", &row, &limits, 65536u, 0)
        : value_output_data(writer, context, &row, 1, NULL, &schema, &limits, 65536u, 0, NULL);
    else rc = 125;
    free(storage);
    return rc;
}

int jash_b_calc(jash_state_t* s, jash_writer_t w, void* ctx,
                const char* args) {
    const char* selected = args ? args : "";
    calc_skip_ws(&selected);
    int mode = 0;
    size_t selector = 0;
    if (!strncmp(selected, "--json", 6) && (!selected[6] || selected[6] == ' ' || selected[6] == '\t')) { mode = 1; selector = 6; }
    else if (!strncmp(selected, "--data-v2", 9) && (!selected[9] || selected[9] == ' ' || selected[9] == '\t')) { mode = 2; selector = 9; }
    else if (!strncmp(selected, "--data", 6) && (!selected[6] || selected[6] == ' ' || selected[6] == '\t')) { mode = 2; selector = 6; }
    if (mode) {
        selected += selector;
        calc_skip_ws(&selected);
        return calc_structured(s, w, ctx, selected, mode);
    }
    if (!args || !*args) {
        jash_writes(w, ctx,
            "calc: usage: = EXPRESSION  or  calc EXPRESSION\n");
        return 2;
    }

    const char* p = args;
    calc_skip_ws(&p);
    if (!*p) {
        jash_writes(w, ctx,
            "calc: usage: = EXPRESSION  or  calc EXPRESSION\n");
        return 2;
    }

    double result = calc_parse_expr(s, &p);
    calc_print_result(w, ctx, result);
    return 0;
}
