// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/jash_fastpath.h>

#include <stdio.h>

static int expect_kind(const char* label, const char* input,
                       jash_fastpath_kind_t expected) {
    jash_fastpath_result_t result = jash_fastpath_classify(input);
    if (result.kind == expected) return 0;
    fprintf(stderr, "%s: got kind %d, expected %d\n",
            label, (int)result.kind, (int)expected);
    return 1;
}

int main(void) {
    static const char long_request[] =
        "crie 10 arquivos .c com codigos matematicos complexos em cada e "
        "todos devem se conectar no makefile e somar uma conta que no final "
        "dara um valor X";
    int failures = 0;

    failures += expect_kind("long Portuguese coding request", long_request,
                            FASTPATH_NONE);
    failures += expect_kind("English standalone no", "no",
                            FASTPATH_DISAGREE);
    failures += expect_kind("Portuguese prepositional no", "arquivo no makefile",
                            FASTPATH_NONE);
    failures += expect_kind("negative imperative is not small talk", "nao crie",
                            FASTPATH_NONE);
    failures += expect_kind("greeting stays on fastpath", "ola",
                            FASTPATH_GREETING);

    if (failures) return 1;
    puts("PASS: smalltalk fastpath leaves requests for the AI router");
    return 0;
}
