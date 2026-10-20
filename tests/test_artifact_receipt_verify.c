// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <jef_verify.h>
#include <assert.h>
int main(int argc, char** argv) {
    assert(argc == 5);
    int result = jef_verify_artifact_receipt(argv[1], argv[2], argv[3], argv[4]);
    return result < 0 ? 2 : result;
}
