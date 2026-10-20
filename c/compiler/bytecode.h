/*
 * Jash Bytecode Compiler - Bash Script Compiler (JIT)
 * Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * 
 * ADR-0002: Bash Script Compiler
 * Wave 1: Bytecode format + VM core
 */

#ifndef JASH_BYTECODE_H
#define JASH_BYTECODE_H

#include <stdint.h>
#include <stddef.h>

/* Bytecode magic and version */
#define JBC_MAGIC "JBC\x01"
#define JBC_VERSION 1

/* Opcodes */
typedef enum {
    OP_HALT       = 0xFF,  /* Stop execution */
    OP_PUSH_CONST = 0x01,  /* Push constant from pool */
    OP_PUSH_VAR   = 0x02,  /* Push variable value */
    OP_STORE_VAR  = 0x03,  /* Pop and store to variable */
    OP_ADD        = 0x04,  /* Pop 2, push sum */
    OP_SUB        = 0x05,  /* Pop 2, push difference */
    OP_MUL        = 0x06,  /* Pop 2, push product */
    OP_DIV        = 0x07,  /* Pop 2, push quotient */
    OP_MOD        = 0x08,  /* Pop 2, push remainder */
    OP_CMP_LT     = 0x09,  /* Pop 2, push (a < b) */
    OP_CMP_GT     = 0x0A,  /* Pop 2, push (a > b) */
    OP_CMP_EQ     = 0x0B,  /* Pop 2, push (a == b) */
    OP_JMP        = 0x0C,  /* Unconditional jump */
    OP_JZ         = 0x0D,  /* Jump if zero (false) */
    OP_JNZ        = 0x0E,  /* Jump if nonzero (true) */
    OP_CALL_FUNC  = 0x0F,  /* Call shell function */
    OP_RET        = 0x10,  /* Return from function */
    OP_ECHO       = 0x11,  /* Pop and print */
    OP_EXEC       = 0x12,  /* Execute external command */
    OP_PIPE_START = 0x13,  /* Begin pipeline */
    OP_PIPE_END   = 0x14,  /* End pipeline */
    OP_DUP        = 0x15,  /* Duplicate top of stack */
    OP_POP        = 0x16,  /* Discard top of stack */
    OP_INC        = 0x17,  /* Increment top of stack */
} jbc_opcode_t;

/* Bytecode file header */
typedef struct {
    char magic[4];           /* "JBC\x01" */
    uint16_t version;        /* Format version */
    uint16_t n_consts;       /* Number of constants */
    uint32_t n_ops;          /* Number of opcodes */
} jbc_header_t;

/* Constant pool entry */
typedef struct {
    enum {
        CONST_INT,
        CONST_STRING
    } type;
    union {
        int64_t ival;
        char *sval;
    };
} jbc_const_t;

/* Bytecode module */
typedef struct {
    jbc_header_t header;
    jbc_const_t *consts;     /* Constant pool */
    uint8_t *code;           /* Opcode stream */
    size_t code_size;        /* Size of code in bytes */
} jbc_module_t;

/* VM stack */
#define VM_STACK_SIZE 4096
typedef struct {
    int64_t stack[VM_STACK_SIZE];
    int stack_top;
} vm_stack_t;

/* Variable environment */
#define VM_MAX_VARS 256
typedef struct {
    char *name;
    int64_t value;
} vm_var_t;

typedef struct {
    vm_var_t vars[VM_MAX_VARS];
    int count;
} vm_env_t;

/* VM state */
typedef struct {
    vm_stack_t stack;
    vm_env_t env;              /* Variable environment */
    uint8_t *pc;               /* Program counter */
    jbc_module_t *module;      /* Current module */
} vm_state_t;

/* API */

/* Create a new bytecode module */
jbc_module_t *jbc_module_create(void);

/* Free a bytecode module */
void jbc_module_free(jbc_module_t *mod);

/* Add a constant to the pool */
int jbc_add_const_int(jbc_module_t *mod, int64_t val);
int jbc_add_const_string(jbc_module_t *mod, const char *str);

/* Emit an opcode */
void jbc_emit(jbc_module_t *mod, uint8_t op);
void jbc_emit_u8(jbc_module_t *mod, uint8_t op, uint8_t arg);
void jbc_emit_u16(jbc_module_t *mod, uint8_t op, uint16_t arg);
void jbc_emit_u32(jbc_module_t *mod, uint8_t op, uint32_t arg);

/* Write bytecode to file */
int jbc_write(jbc_module_t *mod, const char *path);

/* Read bytecode from file */
jbc_module_t *jbc_read(const char *path);

/* Disassemble bytecode */
void jbc_disasm(jbc_module_t *mod);

/* VM execution */
vm_state_t *vm_create(jbc_module_t *mod);
void vm_free(vm_state_t *vm);
int vm_run(vm_state_t *vm);

#endif /* JASH_BYTECODE_H */
