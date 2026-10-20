// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Jash Smart Caching Layer - Phase 1: Basic Cache
// Author: F E R M I ∞ H A R T <contact@fermihart.com>

#ifndef JASH_CACHE_H
#define JASH_CACHE_H

#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define CACHE_SIZE 256
#define CACHE_KEY_MAX 512
#define CACHE_CWD_MAX 512
#define CACHE_VALUE_MAX 131072
#define CACHE_DEPS_MAX 8
// Global cap on live entries; beyond this the LRU entry is evicted
#define CACHE_MAX_ENTRIES 1024

// Cache entry structure
typedef struct {
    char key[CACHE_KEY_MAX];           // Hash do comando + contexto
    char cmd[CACHE_KEY_MAX];           // Comando original (para debug)
    char cwd[CACHE_CWD_MAX];           // Diretório faz parte da identidade
    char *value;                       // Output capturado (heap, strlen+1 bytes)
    time_t timestamp;                  // Quando foi capturado
    int ttl;                           // Tempo de vida em segundos (0 = infinito)
    
    // File dependencies para invalidation
    char deps[CACHE_DEPS_MAX][256];    // Arquivos que invalidam o cache
    struct stat dep_stats[CACHE_DEPS_MAX]; // stat() no momento do cache
    int dep_count;
    
    // Heurística adaptativa
    int hit_count;                     // Quantas vezes foi usado
    int miss_count;                    // Quantas vezes foi invalidado
    float confidence;                  // 0.0 - 1.0
    
    // LRU tracking
    time_t last_access;
} cache_entry_t;

// Cache bucket (hash table)
typedef struct {
    cache_entry_t *entries;
    int count;
    int capacity;
} cache_bucket_t;

// Inicializa o cache
void cache_init(void);

// Libera o cache
void cache_destroy(void);

// Lookup no cache - retorna NULL se não encontrado ou inválido
cache_entry_t* cache_lookup(const char *cmd, const char *cwd);

// Armazena resultado no cache
void cache_store(const char *cmd, const char *cwd, const char *value, 
                 int ttl, const char **deps, int dep_count);

// Invalida entrada específica
void cache_invalidate(const char *cmd, const char *cwd);

// Limpa todo o cache
void cache_clear(void);

// Estatísticas
void cache_stats(int *hits, int *misses, int *size);

// Configuração de cache para um comando (retornado por cache_get_cmd_config)
typedef struct {
    int ttl;                    // TTL em segundos (-1 = não cacheável)
    const char *dep_suffixes[CACHE_DEPS_MAX]; // Sufixos relativos ao cwd (ex: ".git/HEAD")
    int dep_count;
} cache_cmd_config_t;

// Retorna configuração de cache para um comando (TTL + deps)
// Retorna 1 se cacheável, 0 se não. Preenche *config.
int cache_get_cmd_config(const char *cmd, cache_cmd_config_t *config);

// Persistência
int cache_save_to_disk(const char *path);
int cache_load_from_disk(const char *path);

#endif // JASH_CACHE_H
