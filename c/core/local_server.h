// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_LOCAL_SERVER_H
#define JASH_CORE_LOCAL_SERVER_H
#define JASH_LOCAL_CLIENTS 16
#define JASH_LOCAL_IDLE_MS 10000
int jash_local_server(const char* path, const char* scope_file, int (*session)(int shutdown_fd, int data_fd));
#endif
