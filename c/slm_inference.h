// SPDX-License-Identifier: LicenseRef-SPL-1.0
// slm_inference.h — Local Inference Engine Interface
// Part of Jash Cognitive OS (ADR-0022)

#ifndef SLM_INFERENCE_H
#define SLM_INFERENCE_H

#include <stdint.h>
#include <stddef.h>

// Run inference on a model. Returns 0 on success.
float slm_infer(int model_id, const float* input, size_t input_len,
                float* output, size_t output_len);

// Batch inference for multiple inputs
typedef struct {
    float** inputs;
    size_t* input_lens;
    float** outputs;
    size_t* output_lens;
    size_t batch_size;
    uint64_t total_latency_us;
} slm_batch_t;

int slm_infer_batch(slm_batch_t* batch);

// Tokenize text into float array (simplified BPE)
size_t slm_tokenize(const char* text, float* tokens, size_t max_tokens);

// Math utilities
float slm_sigmoid(float x);
float slm_softmax(const float* logits, size_t len, size_t idx);

// Init/cleanup
void slm_inference_init(void);
void slm_inference_cleanup(void);

#endif // SLM_INFERENCE_H
