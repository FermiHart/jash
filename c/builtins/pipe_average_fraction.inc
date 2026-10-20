// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Exact means of JDV integer primitives. The total can exceed U64 while the
 * mean remains inside the input range. An improper numerator would therefore
 * lose valid results; use a versioned mixed rational when a fraction remains.
 * No decimal primitive, float, string coercion or wire kind is introduced. */
static uint64_t average_gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t next = a % b; a = b; b = next; }
    return a;
}
static int native_average_fraction(dataset_t* data, const decimal_t* total,
                                    uint64_t count, unsigned kinds,
                                    jdv_value_t* out, int* fractional) {
    *fractional = 0;
    if (!count || !total->len) return 0;
    if (total->exponent < 0 || total->len + (size_t)total->exponent > PIPE_NUM_MAX || count > PIPE_ROW_MAX)
        return bad(EDOM);
    uint64_t whole = 0, remainder = 0;
    size_t length = total->len + (size_t)total->exponent;
    for (size_t position = length; position > 0;) {
        position--;
        unsigned digit = position >= (size_t)total->exponent ? total->digits[position - (size_t)total->exponent] : 0;
        uint64_t dividend = remainder * 10u + digit;
        uint64_t quotient = dividend / count; remainder = dividend % count;
        if (whole > (UINT64_MAX - quotient) / 10u) return bad(EOVERFLOW);
        whole = whole * 10u + quotient;
    }
    if (!remainder) return 0;
    uint64_t divisor = average_gcd(remainder, count);
    const char* unit = kinds == (1u << JDV_FILESIZE) ? "bytes" :
        kinds == (1u << JDV_DURATION) ? "nanoseconds" :
        kinds == (1u << JDV_TIMESTAMP) ? "unix_nanoseconds" : "scalar";
    jdv_value_t schema, dimension;
    const char* identifier = "jash.number.mixed_rational.v1";
    if (jdv_value_symbol_limited(&data->arena, identifier, strlen(identifier), &schema, &data->limits) ||
        jdv_value_symbol_limited(&data->arena, unit, strlen(unit), &dimension, &data->limits)) return -1;
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("schema"), schema}, {JDV_LITERAL("negative"), jdv_value_bool(total->negative)},
        {JDV_LITERAL("whole"), jdv_value_u64(whole)}, {JDV_LITERAL("numerator"), jdv_value_u64(remainder / divisor)},
        {JDV_LITERAL("denominator"), jdv_value_u64(count / divisor)}, {JDV_LITERAL("unit"), dimension}
    };
    if (jdv_value_record_limited(&data->arena, fields, 6, out, &data->limits)) return -1;
    *fractional = 1; return 0;
}
