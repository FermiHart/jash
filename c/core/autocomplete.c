// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Autocompletion: paths, commands, flags

#include "autocomplete.h"
#include "history.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_COMPLETIONS 256
#define MAX_PATH_LEN 4096

// Check if path is executable
static int is_executable(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IXUSR);
}

// Complete file/directory paths
int autocomplete_path(const char* partial, char** matches, int max_matches) {
    int nmatches = 0;
    
    // Extract directory and prefix
    char dir[MAX_PATH_LEN];
    const char* prefix;
    
    const char* last_slash = strrchr(partial, '/');
    if (last_slash) {
        size_t dir_len = last_slash - partial;
        if (dir_len == 0) {
            strlcpy(dir, "/", sizeof(dir));
        } else {
            if (dir_len >= sizeof(dir)) return 0;
            memcpy(dir, partial, dir_len);
            dir[dir_len] = '\0';
        }
        prefix = last_slash + 1;
    } else {
        strlcpy(dir, ".", sizeof(dir));
        prefix = partial;
    }
    
    DIR* d = opendir(dir);
    if (!d) return 0;
    
    size_t prefix_len = strlen(prefix);
    struct dirent* ent;
    
    while ((ent = readdir(d)) != NULL && nmatches < max_matches) {
        if (ent->d_name[0] == '.' && prefix[0] != '.') {
            continue;  // Skip hidden files unless prefix starts with .
        }
        
        if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
            char full_path[MAX_PATH_LEN];
            
            if (strcmp(dir, ".") == 0) {
                snprintf(full_path, sizeof(full_path), "%s", ent->d_name);
            } else if (strcmp(dir, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "/%s", ent->d_name);
            } else {
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
            }
            
            // Check if directory
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                strlcat(full_path, "/", sizeof(full_path));
            }
            
            matches[nmatches++] = strdup(full_path);
        }
    }
    
    closedir(d);
    return nmatches;
}

// Complete command names
int autocomplete_command(const char* partial, char** matches, int max_matches) {
    int nmatches = 0;
    size_t prefix_len = strlen(partial);
    
    // Search in PATH
    const char* path_env = getenv("PATH");
    if (!path_env) return 0;
    
    char* path_copy = strdup(path_env);
    char* dir = strtok(path_copy, ":");
    
    while (dir && nmatches < max_matches) {
        DIR* d = opendir(dir);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL && nmatches < max_matches) {
                if (strncmp(ent->d_name, partial, prefix_len) == 0) {
                    char full_path[MAX_PATH_LEN];
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
                    
                    if (is_executable(full_path)) {
                        // Check if already in list
                        int duplicate = 0;
                        for (int i = 0; i < nmatches; i++) {
                            if (strcmp(matches[i], ent->d_name) == 0) {
                                duplicate = 1;
                                break;
                            }
                        }
                        if (!duplicate) {
                            matches[nmatches++] = strdup(ent->d_name);
                        }
                    }
                }
            }
            closedir(d);
        }
        dir = strtok(NULL, ":");
    }
    
    free(path_copy);
    return nmatches;
}

// Complete flags for common commands
int autocomplete_flag(const char* cmd, const char* partial, char** matches, int max_matches) {
    int nmatches = 0;
    
    // Common flags for common commands
    if (strcmp(cmd, "ls") == 0) {
        const char* flags[] = {"-l", "-a", "-la", "-lh", "-R", "--color", "--help", NULL};
        for (int i = 0; flags[i] && nmatches < max_matches; i++) {
            if (strncmp(flags[i], partial, strlen(partial)) == 0) {
                matches[nmatches++] = strdup(flags[i]);
            }
        }
    } else if (strcmp(cmd, "grep") == 0) {
        const char* flags[] = {"-i", "-v", "-r", "-n", "-l", "-c", "--color", "--help", NULL};
        for (int i = 0; flags[i] && nmatches < max_matches; i++) {
            if (strncmp(flags[i], partial, strlen(partial)) == 0) {
                matches[nmatches++] = strdup(flags[i]);
            }
        }
    } else if (strcmp(cmd, "rm") == 0) {
        const char* flags[] = {"-r", "-rf", "-f", "-i", "-v", "--interactive=never", "--help", NULL};
        for (int i = 0; flags[i] && nmatches < max_matches; i++) {
            if (strncmp(flags[i], partial, strlen(partial)) == 0) {
                matches[nmatches++] = strdup(flags[i]);
            }
        }
    } else if (strcmp(cmd, "git") == 0) {
        const char* subcmds[] = {"status", "add", "commit", "push", "pull", "checkout", "branch", "log", "--help", NULL};
        for (int i = 0; subcmds[i] && nmatches < max_matches; i++) {
            if (strncmp(subcmds[i], partial, strlen(partial)) == 0) {
                matches[nmatches++] = strdup(subcmds[i]);
            }
        }
    }
    
    return nmatches;
}

// Main autocomplete function
int autocomplete(const char* line, int cursor_pos, char** matches, int max_matches) {
    if (cursor_pos == 0 || !line[cursor_pos - 1]) {
        return 0;
    }
    
    // Find last word
    const char* last_space = NULL;
    for (int i = cursor_pos - 1; i >= 0; i--) {
        if (line[i] == ' ') {
            last_space = &line[i];
            break;
        }
    }
    
    const char* partial = last_space ? last_space + 1 : line;
    
    // First word? Complete command
    if (!last_space || last_space == line) {
        return autocomplete_command(partial, matches, max_matches);
    }
    
    // Flag? Complete flag
    if (partial[0] == '-') {
        char cmd[256];
        sscanf(line, "%255s", cmd);
        return autocomplete_flag(cmd, partial, matches, max_matches);
    }
    
    // Otherwise, complete path
    return autocomplete_path(partial, matches, max_matches);
}

// Show completions
void autocomplete_show(char** matches, int nmatches) {
    if (nmatches == 0) return;
    
    if (nmatches == 1) {
        printf("%s", matches[0]);
    } else {
        printf("\n");
        for (int i = 0; i < nmatches; i++) {
            printf("%-30s", matches[i]);
            if ((i + 1) % 3 == 0) printf("\n");
        }
        if (nmatches % 3 != 0) printf("\n");
    }
}

// Free completions
void autocomplete_free(char** matches, int nmatches) {
    for (int i = 0; i < nmatches; i++) {
        free(matches[i]);
    }
}
