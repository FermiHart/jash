// SPDX-License-Identifier: LicenseRef-SPL-1.0
// slm_loader.h — GGUF Model Loader Interface
// Part of Jash Cognitive OS (ADR-0022)

#ifndef SLM_LOADER_H
#define SLM_LOADER_H

#include <stdint.h>
#include <stddef.h>

// Load a GGUF model from path. lazy=1 means mmap without loading into RAM.
// Returns model_id (>=0) on success, -1 on failure.
int slm_load_model(const char* path, uint8_t lazy);

// Unload a model and free resources
void slm_unload_model(int model_id);

// Get pointer to model data (triggers page-in for lazy models)
const void* slm_get_tensor(int model_id, size_t index);

// Get model file size in bytes
size_t slm_get_model_size(int model_id);

// Check if model is loaded
uint8_t slm_model_loaded(int model_id);

// Discover and load all .gguf files in a directory
int slm_discover_models(const char* dir_path);

// Init/cleanup loader
void slm_loader_init(void);
void slm_loader_cleanup(void);

#endif // SLM_LOADER_H
