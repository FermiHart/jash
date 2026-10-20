// Exercise the actual protocol parser under sanitizers, without runtime state.
#include "core/serve.c"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    char canary = 'x';
    jp_t j = {"\"\""};
    CHECK(jp_string(&j, &canary, 0) == -1 && canary == 'x');
    char out[5];
    j.p = "\"\\ud83d\\ude00\"";
    CHECK(jp_string(&j, out, sizeof(out)) == 4 && !strcmp(out, "\xf0\x9f\x98\x80"));
    j.p = "\"\\ud83d\\ude00\"";
    CHECK(jp_string(&j, out, 4) == -1);
    int tagged = 0;
    j.p = "{\"data\":\"/w==\",\"encoding\":\"base64\"}";
    CHECK(jp_byte_string(&j, out, 2, &tagged) == 1 && tagged && (unsigned char)out[0] == 255 && !out[1]);
    j.p = "{\"encoding\":\"base64\",\"data\":\"/w==\"}";
    CHECK(jp_byte_string(&j, out, 1, NULL) == -1);
    const char* bad_bytes[] = {"{\"data\":\"AA==\",\"encoding\":\"base64\"}",
        "{\"data\":\"Zh==\",\"encoding\":\"base64\"}",
        "{\"data\":\"Zg==AAAA\",\"encoding\":\"base64\"}",
        "{\"data\":\"Zg==\",\"encoding\":\"base64\",}",
        "{\"data\":\"Zg==\",\"data\":\"Zg==\",\"encoding\":\"base64\"}",
        "{\"data\":\"Zg==\",\"encoding\":\"base64\",\"encoding\":\"base64\"}",
        "{\"data\":\"Zg==\",\"encoding\":\"base64\",\"extra\":true}",
        "{\"data\":\"Zg==\"}", "{\"data\":\"Zg==\",\"encoding\":\"hex\"}", NULL};
    for (int i = 0; bad_bytes[i]; i++) {
        j.p = bad_bytes[i];
        CHECK(jp_byte_string(&j, out, sizeof(out), NULL) == -1);
    }
    const char* bad[] = {"\"\\ud800", "\"\\ud800\\u", "\"\\ud800\\", "\"\\udc00\"",
                        "\"\xf0\x9f", "[1,,2]", "[1,]", "{\"a\":}", "01", "1e", "NaN", NULL};
    for (int i = 0; bad[i]; i++) {
        char* exact = strdup(bad[i]);
        CHECK(exact);
        j.p = exact;
        CHECK(!jp_skip(&j));
        CHECK(j.p <= exact + strlen(exact));
        free(exact);
    }
    // Exhaust the argv arena exactly, then try an empty extra argument.
    const size_t pad = SRV_ARENA_MAX - 2; // command "x" plus NUL
    char* wire = malloc(SRV_ARENA_MAX + 128);
    srv_req_t* request = calloc(1, sizeof(*request));
    CHECK(wire && request);
    char* p = wire;
    const char* prefix = "{\"argv\":[\"x\",\"";
    memcpy(p, prefix, strlen(prefix)); p += strlen(prefix);
    memset(p, 'a', pad - 1); p += pad - 1;
    strcpy(p, "\",\"\",\"\"]}");
    const char* error = srv_parse(wire, request);
    CHECK(error != NULL);
    CHECK(request->arena_used <= SRV_ARENA_MAX);
    srv_req_free(request);
    free(request); free(wire);

    // Seeded malformed token fuzz: exact-size input allocations expose any
    // lookahead beyond NUL. No execution or filesystem side effects possible.
    uint32_t seed = 56;
    for (unsigned trial = 0; trial < 20000; trial++) {
        size_t length = trial % 96;
        char* text = malloc(length + 1);
        CHECK(text);
        for (size_t i = 0; i < length; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            text[i] = (char)(1 + seed % 255);
        }
        text[length] = 0;
        j.p = text;
        (void)jp_skip(&j);
        CHECK(j.p <= text + length);
        j.p = text;
        (void)jp_string(&j, out, sizeof(out));
        CHECK(j.p <= text + length);
        j.p = text;
        (void)jp_byte_string(&j, out, sizeof(out), NULL);
        CHECK(j.p <= text + length);
        free(text);
    }
    puts("agent JSON parser: bounds, Unicode, arena exhaustion, 20000 fuzz inputs passed");
    return 0;
}
