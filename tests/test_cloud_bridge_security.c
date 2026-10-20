#include "../../modules/slm-mesh/cloud_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char* marker = getenv("CLOUD_MARKER");
    if (!marker) return 2;
    setenv("OPENAI_API_KEY", "test-key_123", 1);

    char prompt[1024];
    snprintf(prompt, sizeof(prompt),
             "hello 'quoted' \"json\" ; touch %s; # $(touch %s)",
             marker, marker);

    char response[2048];
    cb_error_t rc = cb_query(CB_PROVIDER_GPT, NULL, prompt, "system",
                             response, sizeof(response), NULL);
    if (rc != CB_OK) {
        fprintf(stderr, "cb_query rc=%d\n", rc);
        return 1;
    }
    if (strcmp(response, "ok") != 0) {
        fprintf(stderr, "unexpected response: %s\n", response);
        return 1;
    }
    if (access(marker, F_OK) == 0) {
        fprintf(stderr, "prompt became shell syntax\n");
        return 1;
    }
    return 0;
}
