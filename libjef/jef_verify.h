// SPDX-License-Identifier: LicenseRef-SPL-1.0
// libjef — independent verifier for jash attestation artifacts.
//
// These functions let ANY program (not jash) verify a jash chain or proof using
// only libjef. They are READ-ONLY and need no secret: chain integrity is public
// (recompute the BLAKE3 chain), and Ed25519 proofs verify with the embedded
// public key. This is the "third-party-verifiable" promise as a callable API.

#ifndef LIBJEF_JEF_VERIFY_H
#define LIBJEF_JEF_VERIFY_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verify chain integrity by recomputing hₙ = BLAKE3(hₙ₋₁ ‖ ts_be ‖ rc_le ‖ cmd)
 * for every line of `chain_path`. Returns 0 if intact, 1 if broken/malformed,
 * -1 on open/read failure (also NULL path). *out_entries (if non-NULL) is zero
 * on failure, and the total record count only on success. Empty chains are
 * permitted. Records require physical LF, no NULs, at most 16384 bytes including
 * LF, and at most 4083 escaped command bytes (the producer's payload bound).
 * Sequence starts at zero; timestamps are uint64 nanoseconds with nine decimal
 * fractional digits; rc is a signed int. Decimal tokens use producer spelling.
 * tag and display metadata (cwd/repo/dur/sess and extensions) are NOT
 * authenticated by this hash. This checks internal chain consistency only;
 * it neither authenticates the writer nor matches a chain to a proof. */
int jef_verify_chain(const char* chain_path, unsigned long long* out_entries);

/* INV-PERF10-SIGIL-PROOF-001: return all three observations from the SAME
 * verified stream. records includes SESSION_END; commands follows the
 * jashproof/1 non-sentinel count. tip covers every record. Caller owns the
 * stream at offset zero and any synchronization; this function never closes
 * it. Outputs are zero on failure. This does not authenticate the writer. */
typedef struct {
    unsigned long long records;
    unsigned long long commands;
    unsigned char tip[32];
} jef_chain_state_t;
int jef_verify_chain_stream(FILE* stream, jef_chain_state_t* state);

/* Verify a session proof — either the text `jashproof/1` form (sig_algo:
 * ed25519) or a DSSE envelope (auto-detected by a leading '{'). Uses ONLY the
 * embedded Ed25519 public key. Returns 0 valid, 1 invalid, -1 open/read failure
 * (also NULL path).
 * Legacy symmetric blake3-keyed proofs are NOT accepted here (they need the
 * session secret — this is the sovereign public verifier).
 * DSSE has a closed schema: exactly payloadType, payload, signatures, with
 * exactly one signature object containing keyid and sig. Member names are
 * JSON-decoded before uniqueness checks; shadows/unknown fields are rejected.
 * Key IDs are exactly 64 hex characters and base64 must be canonical padded
 * RFC 4648. The decoded payload limit is 2048 bytes. Proof file reads are
 * bounded at 8191 bytes and reject NULs, truncation, and trailing JSON data.
 * Text requires the exact jashproof/1 LF header and unique signed sig_algo
 * (ed25519), pubkey (64 hex), chain_entries (unsigned decimal), and chain_tip
 * (64 hex) fields. Exactly one terminal sig line holds 128 hex characters;
 * the producer's final LF may be omitted, but no other trailing bytes are
 * accepted. Other key: value body lines remain signed extensions, without
 * assigning them authorization semantics. Proof verification never matches
 * chain contents to the signed count/tip. Writer identity requires the caller
 * to pin a trusted key.
 * Receipt verification uses the same decoder and consumes the very payload
 * bytes that passed signature verification, without reopening the receipt. */
int jef_verify_proof(const char* proof_path);

/* JINV-024 — verificacao ANCORADA numa identidade conhecida.
 *
 * `jef_verify_proof` acima usa a chave publica que vem DENTRO da prova. Isso
 * demonstra que a prova nao foi adulterada DEPOIS de assinada — e so isso.
 * NAO demonstra QUEM assinou: qualquer um gera um par de chaves novo, assina
 * uma prova forjada, embute a propria chave publica, e passa.
 *
 * Enquanto a interface dizia "proof signature valid", o leitor entendia
 * autenticidade. Essa distancia entre o que a mensagem afirma e o que a
 * verificacao sustenta e o diagnostico central da auditoria de 2026-07-27.
 *
 * Esta funcao fecha a distancia: alem da assinatura, exige que a chave publica
 * embutida seja EXATAMENTE `expected_pubkey_hex` (64 chars hex). Devolve 0 se
 * valida E da identidade esperada, 1 se invalida ou de outra chave, -1 se o
 * arquivo nao abre ou falha a leitura, -2 se `expected_pubkey_hex` e malformada.
 * Signature and embedded identity are checked from one bounded file read. */
int jef_verify_proof_pinned(const char* proof_path,
                             const char* expected_pubkey_hex);
/* Same pinned signature check on one caller-owned snapshot (1..8191 bytes).
 * Does not reread a path or prove execution correctness/result bindings. */
int jef_verify_proof_bytes_pinned(const char* bytes, unsigned length,
                                  const char* expected_pubkey_hex);

/* Inspect and verify one immutable proof snapshot using the same decoders as
 * the public verifier. Returns 0 for a structurally decoded proof (the
 * signature_valid field is the verdict), 1 for malformed input, -1 for bad
 * arguments. No identity is trusted by this operation. The payload is the
 * exact signed text body or DSSE payload, and payload_type is empty for text.
 * Signature metadata must only be relied upon when signature_valid is true. */
typedef struct {
    int dsse;
    int signature_valid;
    char public_key[65];
    char payload_type[128];
    unsigned payload_length;
    unsigned char payload[8192];
} jef_proof_observation_t;
int jef_proof_inspect_bytes(const char* bytes, unsigned length,
                             jef_proof_observation_t* observation);

#define JEF_MAINTENANCE_RESPONSE_MAX (16u * 1024u * 1024u)
/* Bind an artifact publication receipt to request and concrete regular bytes.
 * Max artifact 16 MiB, immutable caller paths; 0 valid, 1 invalid/unbound,
 * -1 unreadable request/receipt, -2 malformed key. */
int jef_verify_artifact_receipt(const char* receipt, const char* request, const char* artifact, const char* key);
/* Verify signed preparation independently of a terminal. This binds the request,
 * not effects/completion. Bounds: request 32 KiB, receipt 8 KiB. Return 0 valid,
 * 1 invalid/foreign/unbound, -1 unreadable, -2 malformed key. Immutable paths. */
int jef_verify_maintenance_intent(const char* receipt_path,
                                 const char* request_path,
                                 const char* expected_pubkey_hex);
/* Same verification, additionally returns 0 (legacy) or 1 (admission-v1).
 * Unknown/duplicate protocols fail. Output is set only on successful binding. */
int jef_verify_maintenance_intent_protocol(const char*, const char*, const char*, int*);
/* Verify a signed decision linked to admission-v1 preparation; admitted is 1
 * for authorization, 0 for closure. Neither outcome proves actual effects. */
int jef_verify_maintenance_decision(const char*, const char*, const char*, int* admitted);
/* Verify a maintenance terminal and its signed intent against the concrete
 * request/response. Request framing whitespace/LF is excluded as by JSONL;
 * response body bytes are exact. No CAS objects are required or inferred.
 * Returns 0 bound, 1 invalid/foreign/incomplete, -1 unreadable, -2 bad key.
 * Caller keeps all supplied and linked files immutable during verification. */
int jef_verify_maintenance_receipt(const char* receipt_path,
                                  const char* response_path,
                                  const char* request_path,
                                  const char* expected_pubkey_hex);

/* Extrai a chave publica embutida (64 chars hex + NUL em `out_hex`, cap >= 65)
 * sem verificar a assinatura. Serve para MOSTRAR de quem a prova diz ser,
 * para comparar com a identidade conhecida. Devolve 0 em sucesso.
 * Both formats use the same structural parsing as verification; extraction
 * does not verify the signature, and DSSE never falls back to text lookup.
 * Returns 1 for malformed input, -1 for invalid arguments or open/read failure.
 * Output is only meaningful on success. */
int jef_proof_pubkey_hex(const char* proof_path, char* out_hex, unsigned cap);

/* Verify a signed agent-runtime receipt against its concrete response. Besides
 * pinned Ed25519 identity, checks the receipt artifact hash carried by the
 * response and the payload's result_blake3 binding. */
int jef_verify_runtime_receipt(const char* receipt_path,
                               const char* response_path,
                               const char* request_path,
                               const char* expected_pubkey_hex,
                               const char* expected_receipt_ref);

/* Persistent verifiers cannot derive overflow paths from sealed /proc FDs.
 * This explicit variant binds already-open stdout/stderr artifacts supplied
 * by the trusted caller; -1 retains the legacy path derivation behavior. */
int jef_verify_runtime_receipt_with_streams(
    const char* receipt_path,
    const char* response_path,
    const char* request_path,
    const char* expected_pubkey_hex,
    const char* expected_receipt_ref,
    int stdout_fd,
    int stderr_fd);

/* Verify start/action/terminal job receipts. Every phase binds the byte-exact
 * request and response. Action and terminal receipts also verify their start
 * link; terminal verification additionally binds both CAS streams. */
int jef_verify_job_receipt(
    const char* receipt_path,
    const char* response_path,
    const char* start_request_path,
    const char* expected_pubkey_hex,
    const char* expected_phase,
    const char* expected_receipt_ref);

int jef_verify_job_receipt_with_streams(
    const char* receipt_path,
    const char* response_path,
    const char* start_request_path,
    const char* expected_pubkey_hex,
    const char* expected_phase,
    const char* expected_receipt_ref,
    int stdout_fd,
    int stderr_fd);

/* Bind a concrete execution spec to pinned start + terminal job provenance.
 * Arguments follow CLI job-spec order: expected key first, then seven paths.
 * The start request MUST carry a unique top-level execution_spec_sha256 equal
 * to lowercase "sha256:<digest>" of ALL spec bytes. Spec bytes are opaque,
 * immutable, caller-versioned contract data: no JSON parsing, canonicalization,
 * newline trimming, semantic enforcement, or execution-success assertion.
 * Empty/binary regular-file specs are accepted; the inclusive size limit is
 * 1 MiB (1048576). Non-regular inputs (including FIFOs) are rejected.
 * Valid terminal nonzero rc, cancellation, etc. remain valid provenance.
 *
 * Delegates both phases to jef_verify_job_receipt, including terminal CAS and
 * start-link checks, then binds job ID, start ref/hash, and start request hash
 * to the supplied start. terminal_request_path is the terminal phase request
 * (e.g. job.wait/get/cancel), NOT the start request. Receipt basenames and the
 * sibling start / ../overflow layout must satisfy the existing verifier.
 * Requests are limited to 32 KiB, responses to 600 KiB, receipts to 16 KiB;
 * inherited DSSE signature checks limit decoded payloads to 2048 bytes.
 * JSON nesting is bounded at 64 and top-level decoded field names at 255
 * bytes. Structural lookups reject duplicate binding fields, including
 * escaped aliases.
 * Caller must keep ALL input and linked artifacts immutable during the call.
 *
 * Returns 0 bound provenance, 1 invalid/foreign/unbound, -1 unreadable,
 * oversized, or allocation failure (also NULL paths), -2 malformed expected
 * key (also NULL). Key is exactly 64 hex characters, case-insensitive.
 * Missing/invalid linked starts and CAS streams return 1, as in the existing
 * receipt verifier. No files are written. */
int jef_verify_job_spec(
    const char* expected_pubkey_hex,
    const char* spec_path,
    const char* start_request_path,
    const char* start_response_path,
    const char* start_receipt_path,
    const char* terminal_request_path,
    const char* terminal_response_path,
    const char* terminal_receipt_path);

#ifdef __cplusplus
}
#endif
#endif /* LIBJEF_JEF_VERIFY_H */
