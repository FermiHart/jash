// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Jash Smart Caching Layer - Implementation
// Author: F E R M I ∞ H A R T <contact@fermihart.com>

#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

// Hash table
static cache_bucket_t cache_table[CACHE_SIZE];
static int cache_initialized = 0;
static int cache_hits = 0;
static int cache_misses = 0;
static int cache_total_entries = 0;

/* On-disk record (format version 2): fixed-size layout kept stable so cache
 * files stay compatible with older builds. The live entry allocates `value`
 * dynamically; only the disk representation is padded to CACHE_VALUE_MAX. */
typedef struct {
    char key[CACHE_KEY_MAX];
    char cmd[CACHE_KEY_MAX];
    char cwd[CACHE_CWD_MAX];
    char value[CACHE_VALUE_MAX];
    time_t timestamp;
    int ttl;
    char deps[CACHE_DEPS_MAX][256];
    struct stat dep_stats[CACHE_DEPS_MAX];
    int dep_count;
    int hit_count;
    int miss_count;
    float confidence;
    time_t last_access;
} cache_disk_entry_t;

// Whitelist de comandos idempotentes (sempre retornam mesmo valor no mesmo contexto)
// A ordem importa: padrões mais específicos primeiro para evitar matching incorreto.
static const struct {
    const char *pattern;
    int ttl_seconds;
    const char *deps[4];  // Sufixos relativos ao cwd que invalidam o cache
    int match_start;      // 1 = pattern deve estar no início do comando
} idempotent_cmds[] = {
    /* git commands — match_start=1 evita false positives como "legit status" */
    {"git status",          5,  {".git/HEAD", ".git/index", ".git/MERGE_HEAD", NULL}, 1},
    {"git branch",          5,  {".git/HEAD", ".git/refs/", NULL}, 1},
    {"git rev-parse",      10,  {".git/HEAD", ".git/refs/", NULL}, 1},
    {"git log",             5,  {".git/HEAD", ".git/refs/", NULL}, 1},
    {"git shortlog",        5,  {".git/HEAD", ".git/refs/", NULL}, 1},
    {"git diff",            3,  {".git/HEAD", ".git/index", NULL}, 1},
    {"git remote",         30,  {".git/config", NULL}, 1},
    {"git tag",             5,  {".git/refs/tags/", NULL}, 1},
    {"git describe",       10,  {".git/HEAD", ".git/refs/tags/", NULL}, 1},
    /* system info — rarely changes */
    {"whoami",              3600, {NULL}, 1},
    {"hostname",            3600, {NULL}, 1},
    {"uname",               3600, {NULL}, 1},
    {"id",                  60,   {NULL}, 1},
    {"groups",              60,   {NULL}, 1},
    /* path/string commands */
    {"which ",              60,   {NULL}, 1},
    {"pwd",                 0,    {NULL}, 1},
    {"dirname ",            0,    {NULL}, 0},  // puramente string
    {"basename ",           0,    {NULL}, 0},  // puramente string
    {"echo ",               0,    {NULL}, 0},  // puramente string
    {"printf ",             0,    {NULL}, 0},  // puramente string
    {"date",                1,    {NULL}, 1},
    {"uptime",              5,    {NULL}, 1},
    {NULL, 0, {NULL}, 0}
};

// Blacklist de comandos perigosos (nunca cachear)
static const char *dangerous_patterns[] = {
    "rm ", "rm\t",
    "mv ", "mv\t",
    "cp ", "cp\t",
    "chmod ", "chmod\t",
    "chown ", "chown\t",
    "kill ", "kill\t",
    "pkill ", "pkill\t",
    "sudo ", "sudo\t",
    "su ", "su\t",
    ">", ">>", "<",  // redirects
    "|",            // pipes
    "&",            // background
    NULL
};

// FNV-1a hash function
static uint32_t fnv1a_hash(const char *str) {
    uint32_t hash = 2166136261u;
    for (const char *p = str; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t cache_hash(const char *cmd, const char *cwd) {
    uint32_t hash = fnv1a_hash(cmd);
    if (cwd) {
        hash ^= fnv1a_hash(cwd);
        hash *= 16777619u;
    }
    return hash % CACHE_SIZE;
}

// Release the heap-allocated value of an entry
static void cache_entry_release(cache_entry_t *entry) {
    free(entry->value);
    entry->value = NULL;
}

// Remove entry i from a bucket (frees its value, keeps order)
static void cache_bucket_remove(cache_bucket_t *bucket, int i) {
    cache_entry_release(&bucket->entries[i]);
    memmove(&bucket->entries[i],
            &bucket->entries[i + 1],
            (bucket->count - i - 1) * sizeof(cache_entry_t));
    bucket->count--;
    cache_total_entries--;
}

// Evict the globally least-recently-used entry (uses last_access)
static void cache_evict_lru(void) {
    cache_bucket_t *victim_bucket = NULL;
    int victim_idx = -1;
    time_t oldest = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        for (int j = 0; j < cache_table[i].count; j++) {
            time_t la = cache_table[i].entries[j].last_access;
            if (!victim_bucket || la < oldest) {
                victim_bucket = &cache_table[i];
                victim_idx = j;
                oldest = la;
            }
        }
    }
    if (victim_bucket) cache_bucket_remove(victim_bucket, victim_idx);
}

void cache_init(void) {
    if (cache_initialized) return;
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        cache_table[i].entries = NULL;
        cache_table[i].count = 0;
        cache_table[i].capacity = 0;
    }
    
    cache_initialized = 1;
    cache_hits = 0;
    cache_misses = 0;
}

void cache_destroy(void) {
    if (!cache_initialized) return;
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache_table[i].entries) {
            for (int j = 0; j < cache_table[i].count; j++) {
                cache_entry_release(&cache_table[i].entries[j]);
            }
            free(cache_table[i].entries);
            cache_table[i].entries = NULL;
        }
        cache_table[i].count = 0;
        cache_table[i].capacity = 0;
    }
    cache_total_entries = 0;
    
    cache_initialized = 0;
}

// Helper: match command against pattern, respecting match_start
static int cmd_matches_pattern(const char *cmd, const char *pattern, int match_start) {
    if (!cmd || !pattern) return 0;
    if (match_start) {
        /* Pattern must appear at the very start of the command string.
         * This prevents "legit status" from matching "git status". */
        return strncmp(cmd, pattern, strlen(pattern)) == 0;
    }
    return strstr(cmd, pattern) != NULL;
}

// Verifica se comando é cacheável
static int is_cacheable(const char *cmd) {
    // Verifica blacklist
    for (int i = 0; dangerous_patterns[i]; i++) {
        if (strstr(cmd, dangerous_patterns[i])) {
            return 0;  // Não cacheável
        }
    }
    
    // Verifica se está na whitelist
    for (int i = 0; idempotent_cmds[i].pattern; i++) {
        if (cmd_matches_pattern(cmd, idempotent_cmds[i].pattern, idempotent_cmds[i].match_start)) {
            return 1;  // Cacheável
        }
    }
    
    // Por padrão, não cacheia comandos desconhecidos
    return 0;
}

// Verifica se cache entry ainda é válida
static int is_cache_valid(cache_entry_t *entry) {
    if (!entry || !entry->key[0]) return 0;
    
    // Check TTL
    if (entry->ttl > 0) {
        time_t now = time(NULL);
        if (now - entry->timestamp > entry->ttl) {
            return 0;  // Expirou
        }
    }
    
    // Check file dependencies
    for (int i = 0; i < entry->dep_count; i++) {
        struct stat current;
        if (stat(entry->deps[i], &current) != 0) {
            return 0;  // Arquivo não existe mais
        }
        
        // Compare mtime e size
        if (current.st_mtime != entry->dep_stats[i].st_mtime ||
            current.st_size != entry->dep_stats[i].st_size) {
            return 0;  // Arquivo mudou
        }
    }
    
    return 1;
}

cache_entry_t* cache_lookup(const char *cmd, const char *cwd) {
    if (!cache_initialized || !cmd) {
        cache_misses++;
        return NULL;
    }
    
    uint32_t idx = cache_hash(cmd, cwd);
    cache_bucket_t *bucket = &cache_table[idx];
    
    for (int i = 0; i < bucket->count; i++) {
        cache_entry_t *entry = &bucket->entries[i];
        
        // Verifica se é o mesmo comando + cwd
        if (strcmp(entry->cmd, cmd) == 0 &&
            strcmp(entry->cwd, cwd ? cwd : "") == 0) {
            if (is_cache_valid(entry)) {
                // Cache hit!
                entry->hit_count++;
                entry->last_access = time(NULL);
                cache_hits++;
                return entry;
            } else {
                // Cache miss - entrada inválida
                entry->miss_count++;
                break;
            }
        }
    }
    
    cache_misses++;
    return NULL;
}

void cache_store(const char *cmd, const char *cwd, const char *value,
                 int ttl, const char **deps, int dep_count) {
    if (!cache_initialized || !cmd || !value) return;
    
    // Verifica se comando é cacheável
    if (!is_cacheable(cmd)) return;
    
    uint32_t idx = cache_hash(cmd, cwd);
    cache_bucket_t *bucket = &cache_table[idx];
    
    // Verifica se já existe entrada para este comando
    for (int i = 0; i < bucket->count; i++) {
        if (strcmp(bucket->entries[i].cmd, cmd) == 0 &&
            strcmp(bucket->entries[i].cwd, cwd ? cwd : "") == 0) {
            // Atualiza entrada existente
            cache_entry_t *entry = &bucket->entries[i];
            size_t vlen = strlen(value);
            if (vlen >= CACHE_VALUE_MAX) return;  // JINV-029: never persist a truncated value
            char *new_value = realloc(entry->value, vlen + 1);
            if (!new_value) return;  // OOM: keep stale entry
            entry->value = new_value;
            memcpy(entry->value, value, vlen + 1);
            entry->timestamp = time(NULL);
            entry->ttl = ttl;
            entry->last_access = entry->timestamp;
            
            // Atualiza dependencies (crítico para git: .git/HEAD pode mudar)
            entry->dep_count = 0;
            if (deps && dep_count > 0) {
                for (int j = 0; j < dep_count && j < CACHE_DEPS_MAX; j++) {
                    strncpy(entry->deps[j], deps[j], 255);
                    entry->deps[j][255] = '\0';
                    if (stat(deps[j], &entry->dep_stats[j]) != 0) {
                        memset(&entry->dep_stats[j], 0, sizeof(struct stat));
                    }
                    entry->dep_count++;
                }
            }
            return;
        }
    }
    
    // Rejeita valores acima do teto por entrada (callers já garantem, defesa)
    size_t vlen = strlen(value);
    if (vlen >= CACHE_VALUE_MAX) return;

    // Cap global de entradas: despeja a menos recentemente usada (LRU)
    if (cache_total_entries >= CACHE_MAX_ENTRIES) {
        cache_evict_lru();
        if (cache_total_entries >= CACHE_MAX_ENTRIES) return;  // cache vazio
    }

    // Expande bucket se necessário
    if (bucket->count >= bucket->capacity) {
        int new_cap = bucket->capacity == 0 ? 4 : bucket->capacity * 2;
        cache_entry_t *new_entries = realloc(bucket->entries,
                                              new_cap * sizeof(cache_entry_t));
        if (!new_entries) return;  // OOM

        bucket->entries = new_entries;
        bucket->capacity = new_cap;
    }

    char *value_copy = malloc(vlen + 1);
    if (!value_copy) return;  // OOM
    memcpy(value_copy, value, vlen + 1);

    // Cria nova entrada
    cache_entry_t *entry = &bucket->entries[bucket->count];
    memset(entry, 0, sizeof(cache_entry_t));

    // Gera key
    snprintf(entry->key, CACHE_KEY_MAX, "%u", cache_hash(cmd, cwd));

    // Copia comando e valor
    strncpy(entry->cmd, cmd, CACHE_KEY_MAX - 1);
    entry->cmd[CACHE_KEY_MAX - 1] = '\0';
    strncpy(entry->cwd, cwd ? cwd : "", CACHE_CWD_MAX - 1);
    entry->cwd[CACHE_CWD_MAX - 1] = '\0';
    entry->value = value_copy;
    
    // Timestamp e TTL
    entry->timestamp = time(NULL);
    entry->ttl = ttl;
    entry->last_access = entry->timestamp;
    
    // Dependencies
    entry->dep_count = 0;
    if (deps && dep_count > 0) {
        for (int i = 0; i < dep_count && i < CACHE_DEPS_MAX; i++) {
            strncpy(entry->deps[i], deps[i], 255);
            entry->deps[i][255] = '\0';
            
            // Captura stat() do arquivo
            if (stat(deps[i], &entry->dep_stats[i]) != 0) {
                memset(&entry->dep_stats[i], 0, sizeof(struct stat));
            }
            
            entry->dep_count++;
        }
    }
    
    // Inicializa heurística
    entry->hit_count = 0;
    entry->miss_count = 0;
    entry->confidence = 1.0;
    
    bucket->count++;
    cache_total_entries++;
}

void cache_invalidate(const char *cmd, const char *cwd) {
    if (!cache_initialized || !cmd) return;
    
    uint32_t idx = cache_hash(cmd, cwd);
    cache_bucket_t *bucket = &cache_table[idx];
    
    for (int i = 0; i < bucket->count; i++) {
        if (strcmp(bucket->entries[i].cmd, cmd) == 0 &&
            strcmp(bucket->entries[i].cwd, cwd ? cwd : "") == 0) {
            // Remove entrada
            cache_bucket_remove(bucket, i);
            return;
        }
    }
}

void cache_clear(void) {
    if (!cache_initialized) return;
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        for (int j = 0; j < cache_table[i].count; j++) {
            cache_entry_release(&cache_table[i].entries[j]);
        }
        cache_table[i].count = 0;
    }
    cache_total_entries = 0;
    
    cache_hits = 0;
    cache_misses = 0;
}

void cache_stats(int *hits, int *misses, int *size) {
    if (hits) *hits = cache_hits;
    if (misses) *misses = cache_misses;
    
    if (size) {
        int total = 0;
        for (int i = 0; i < CACHE_SIZE; i++) {
            total += cache_table[i].count;
        }
        *size = total;
    }
}

// Returns the cache config (TTL + dependency suffixes) for a command.
// Returns 1 if cacheable, 0 if not. Fills *config.
int cache_get_cmd_config(const char *cmd, cache_cmd_config_t *config) {
    if (!cmd || !config) return 0;
    
    // Check blacklist first
    for (int i = 0; dangerous_patterns[i]; i++) {
        if (strstr(cmd, dangerous_patterns[i])) {
            return 0;
        }
    }
    
    // Find matching whitelist entry
    for (int i = 0; idempotent_cmds[i].pattern; i++) {
        if (cmd_matches_pattern(cmd, idempotent_cmds[i].pattern, idempotent_cmds[i].match_start)) {
            config->ttl = idempotent_cmds[i].ttl_seconds;
            config->dep_count = 0;
            for (int j = 0; idempotent_cmds[i].deps[j] && j < CACHE_DEPS_MAX; j++) {
                config->dep_suffixes[j] = idempotent_cmds[i].deps[j];
                config->dep_count++;
            }
            return 1;
        }
    }
    
    return 0;
}

// Persistência simples (binária)
int cache_save_to_disk(const char *path) {
    if (!cache_initialized || !path) return -1;
    
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    
    // Header
    uint32_t magic = 0x4A415348;  // "JASH"
    uint32_t version = 2;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&version, sizeof(version), 1, f);
    
    // Conta apenas entradas válidas — o header deve bater com o que é
    // escrito abaixo, senão o load quebra no EOF prematuro e perde entradas
    int total = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        for (int j = 0; j < cache_table[i].count; j++) {
            cache_entry_t *entry = &cache_table[i].entries[j];
            if (is_cache_valid(entry) && strlen(entry->value) < CACHE_VALUE_MAX) total++;
        }
    }
    fwrite(&total, sizeof(total), 1, f);
    
    // Escreve entradas válidas no registro de disco (formato fixo, versão 2)
    for (int i = 0; i < CACHE_SIZE; i++) {
        for (int j = 0; j < cache_table[i].count; j++) {
            cache_entry_t *entry = &cache_table[i].entries[j];
            if (!is_cache_valid(entry)) continue;
            
            cache_disk_entry_t rec;
            memset(&rec, 0, sizeof(rec));
            memcpy(rec.key, entry->key, sizeof(rec.key));
            memcpy(rec.cmd, entry->cmd, sizeof(rec.cmd));
            memcpy(rec.cwd, entry->cwd, sizeof(rec.cwd));
            size_t vlen = strlen(entry->value);
            if (vlen >= CACHE_VALUE_MAX) continue;  // JINV-029: skip, do not truncate
            memcpy(rec.value, entry->value, vlen);
            rec.value[vlen] = '\0';
            rec.timestamp = entry->timestamp;
            rec.ttl = entry->ttl;
            memcpy(rec.deps, entry->deps, sizeof(rec.deps));
            memcpy(rec.dep_stats, entry->dep_stats, sizeof(rec.dep_stats));
            rec.dep_count = entry->dep_count;
            rec.hit_count = entry->hit_count;
            rec.miss_count = entry->miss_count;
            rec.confidence = entry->confidence;
            rec.last_access = entry->last_access;
            fwrite(&rec, sizeof(rec), 1, f);
        }
    }
    
    fclose(f);
    return 0;
}

int cache_load_from_disk(const char *path) {
    if (!cache_initialized || !path) return -1;
    
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    // Verifica header
    uint32_t magic, version;
    int total;
    
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x4A415348) {
        fclose(f);
        return -1;
    }
    
    if (fread(&version, sizeof(version), 1, f) != 1 || version != 2) {
        fclose(f);
        return -1;
    }
    
    if (fread(&total, sizeof(total), 1, f) != 1) {
        fclose(f);
        return -1;
    }
    
    // Lê entradas
    for (int i = 0; i < total; i++) {
        cache_disk_entry_t rec;
        if (fread(&rec, sizeof(rec), 1, f) != 1) break;
        // Defesa contra arquivo corrompido: garante NUL-termination
        rec.key[CACHE_KEY_MAX - 1] = '\0';
        rec.cmd[CACHE_KEY_MAX - 1] = '\0';
        rec.cwd[CACHE_CWD_MAX - 1] = '\0';
        rec.value[CACHE_VALUE_MAX - 1] = '\0';
        
        // Monta entrada viva (value emprestado do registro só p/ validação)
        cache_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.key, rec.key, sizeof(entry.key));
        memcpy(entry.cmd, rec.cmd, sizeof(entry.cmd));
        memcpy(entry.cwd, rec.cwd, sizeof(entry.cwd));
        entry.value = rec.value;
        entry.timestamp = rec.timestamp;
        entry.ttl = rec.ttl;
        memcpy(entry.deps, rec.deps, sizeof(entry.deps));
        memcpy(entry.dep_stats, rec.dep_stats, sizeof(entry.dep_stats));
        entry.dep_count = rec.dep_count;
        // Defesa: dep_count fora da faixa leria deps[] fora dos limites
        if (entry.dep_count < 0 || entry.dep_count > CACHE_DEPS_MAX)
            entry.dep_count = 0;
        entry.hit_count = rec.hit_count;
        entry.miss_count = rec.miss_count;
        entry.confidence = rec.confidence;
        entry.last_access = rec.last_access;
        
        // Verifica se ainda é válida
        if (!is_cache_valid(&entry)) continue;
        
        // Respeita o cap global de entradas
        if (cache_total_entries >= CACHE_MAX_ENTRIES) break;
        
        // Copia o valor para o heap (a entrada viva é dona do ponteiro)
        size_t vlen = strlen(rec.value);
        char *value_copy = malloc(vlen + 1);
        if (!value_copy) continue;  // OOM
        memcpy(value_copy, rec.value, vlen + 1);
        entry.value = value_copy;
        
        // Insere no cache
        uint32_t idx = cache_hash(entry.cmd, entry.cwd);
        cache_bucket_t *bucket = &cache_table[idx];
        
        if (bucket->count >= bucket->capacity) {
            int new_cap = bucket->capacity == 0 ? 4 : bucket->capacity * 2;
            cache_entry_t *new_entries = realloc(bucket->entries,
                                                  new_cap * sizeof(cache_entry_t));
            if (!new_entries) { free(value_copy); continue; }
            
            bucket->entries = new_entries;
            bucket->capacity = new_cap;
        }
        
        bucket->entries[bucket->count] = entry;
        bucket->count++;
        cache_total_entries++;
    }
    
    fclose(f);
    return 0;
}
