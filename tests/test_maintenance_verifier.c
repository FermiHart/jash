// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <jef_verify.h>
#include <assert.h>
#include <string.h>
int main(int argc, char** argv) {
    assert(argc == 4 || argc == 5);
    int result = argc == 5 && !strcmp(argv[1], "decision") ? jef_verify_maintenance_decision(argv[2], argv[3], argv[4], NULL) :
                 argc == 4 ? jef_verify_maintenance_intent(argv[1], argv[2], argv[3]) :
                            jef_verify_maintenance_receipt(argv[1], argv[2], argv[3], argv[4]);
    return result < 0 ? 2 : result;
}
