cmake_minimum_required(VERSION 3.20)
# ASM is required: the Linux osif L0 backend is c/osif/linux/syscall.S. Without
# ASM enabled, CMake silently DROPS the .S from the source list — the target
# still links (nothing in c/ references bear_osif_syscall6 directly), so the
# breakage is invisible: osif/L0 simply is not in the Linux binary.
project(bearjash VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Feature tests ───────────────────────────────────────────────────────────
include(CheckSymbolExists)
include(CTest)
check_symbol_exists(strlcpy "string.h" HAVE_STRLCPY)
check_symbol_exists(strlcat "string.h" HAVE_STRLCAT)
if(HAVE_STRLCPY)
    add_compile_definitions(HAVE_STRLCPY=1)
endif()
if(HAVE_STRLCAT)
    add_compile_definitions(HAVE_STRLCAT=1)
endif()

# ── Build id for drift detection (Wave 7): git short hash + dirty flag ──────
# Captured at BUILD time, not configure time. Doing this with execute_process()
# here froze the hash the moment `cmake -B build` ran: a binary five commits
# later still reported the configure-time commit, and `jash version`,
# `--whoami` and the JSON envelope all repeated it. Those are the project's own
# drift detection (JINV-119) — a confidently wrong answer there is worse than
# none. cmake/GitVersion.cmake regenerates the header on every build and only
# REWRITES it when the value changed, so incremental builds stay incremental.
set(JASH_SOURCE_REVISION "" CACHE STRING
    "Canonical 40-hex source revision for vendored/archive builds")
if(JASH_SOURCE_REVISION)
    string(LENGTH "${JASH_SOURCE_REVISION}" JASH_SOURCE_REVISION_LENGTH)
    if(NOT JASH_SOURCE_REVISION_LENGTH EQUAL 40 OR
       NOT JASH_SOURCE_REVISION MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "JASH_SOURCE_REVISION must be exactly 40 lowercase hex characters")
    endif()
    if(NOT DEFINED ENV{SOURCE_DATE_EPOCH} OR
       NOT "$ENV{SOURCE_DATE_EPOCH}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "vendored/archive builds require numeric SOURCE_DATE_EPOCH")
    endif()
endif()
set(JASH_GITVERSION_H "${CMAKE_BINARY_DIR}/jash_gitversion.h")
execute_process(COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${JASH_GITVERSION_H}
                -DREVISION=${JASH_SOURCE_REVISION}
                -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake)
add_custom_target(jash_gitversion ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${JASH_GITVERSION_H}
            -DREVISION=${JASH_SOURCE_REVISION}
            -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${JASH_GITVERSION_H}
    COMMENT "Refreshing build id from git")
# -include, so the four consumers (main.c, version.c, agent_json.c,
# caps_json.c) need no edit and their generated .d files track the header —
# a changed hash recompiles exactly those, nothing else.
add_compile_options(-include ${JASH_GITVERSION_H})

# ── Build provenance: timestamp, user, host ─────────────────────────────────
# Surface these in `jash version` so a human can tell at a glance whether the
# binary on disk matches the source tree / build time they expect.
string(TIMESTAMP JASH_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
string(TIMESTAMP JASH_BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)
add_compile_definitions(
    JASH_BUILD_TIMESTAMP="${JASH_BUILD_TIMESTAMP}"
    JASH_BUILD_DATE="${JASH_BUILD_DATE}"
)

# ── Build type default ─────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── OS detection → osif target selection (L0 syscall + L1 osinfo, ADR-0017)─
option(JASH_TINALINUX "Build Jash for the TinaLinux guest" OFF)
option(JASH_BLIBC "Link against the TinaLinux sovereign libc" OFF)
option(JASH_BETHOS "Build Jash for the BethOS guest using Bear libc" OFF)
if(JASH_TINALINUX AND JASH_BETHOS)
    message(FATAL_ERROR "JASH_TINALINUX and JASH_BETHOS are mutually exclusive")
endif()
if((JASH_TINALINUX OR JASH_BETHOS) AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The guest OSIFs require the Linux x86_64 syscall ABI")
endif()
if(JASH_BETHOS AND NOT JASH_BLIBC)
    message(FATAL_ERROR "JASH_BETHOS requires JASH_BLIBC")
endif()
if(JASH_BLIBC AND NOT (JASH_TINALINUX OR JASH_BETHOS))
    message(FATAL_ERROR "JASH_BLIBC requires JASH_TINALINUX or JASH_BETHOS")
endif()
if(JASH_BETHOS)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR "JASH_BETHOS currently implements x86_64 only")
    endif()
    set(OSIF_TARGET "bethos")
    set(OSIF_SRC "c/osif/bethos/syscall.S")
    set(OSIF_OSINFO_SRC "c/osif/bethos/osinfo.c")
    set(OSIF_FLAGS "-ffreestanding -fno-pic")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(OSIF_TARGET "macos")
    set(OSIF_SRC "c/osif/macos/syscall.c")
    set(OSIF_OSINFO_SRC "c/osif/macos/osinfo.c")
    set(OSIF_FLAGS "-mmacosx-version-min=12.0")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # c/osif/linux/syscall.S is hand-written System V AMD64 asm (rax/rdi/syscall,
    # __NR_exit=60). On aarch64/riscv it would silently mis-assemble into a
    # binary that traps at the first syscall. Fail loudly at configure time
    # instead — porting means adding a syscall.S for that arch, not relaxing
    # this check.
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR
            "osif/linux only implements x86_64 (got '${CMAKE_SYSTEM_PROCESSOR}'). "
            "Add c/osif/linux/syscall-${CMAKE_SYSTEM_PROCESSOR}.S and select it here.")
    endif()
    set(OSIF_TARGET "linux")
    set(OSIF_SRC "c/osif/linux/syscall.S")
    set(OSIF_OSINFO_SRC "c/osif/linux/osinfo.c")
    set(OSIF_FLAGS "-ffreestanding -fno-pic")
    if(JASH_TINALINUX)
        set(OSIF_TARGET "tina")
        set(OSIF_OSINFO_SRC "c/osif/tina/osinfo.c")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(OSIF_TARGET "netbsd")
    set(OSIF_SRC "c/osif/netbsd/syscall.c")
    set(OSIF_OSINFO_SRC "c/osif/netbsd/osinfo.c")
    set(OSIF_FLAGS "-D_DEFAULT_SOURCE")
    # _POSIX_C_SOURCE alone hides BSD extensions (LOCK_EX, NSIG, EUCLEAN,
    # vfork) behind NetBSD's strict-POSIX gate; _NETBSD_SOURCE re-exposes
    # them (mirrors the Darwin _DARWIN_C_SOURCE block below).
    add_definitions(-D_NETBSD_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Minix")
    set(OSIF_TARGET "minix")
    set(OSIF_SRC "c/osif/minix/syscall.c")
    set(OSIF_OSINFO_SRC "c/osif/minix/osinfo.c")
    set(OSIF_FLAGS "-D_DEFAULT_SOURCE")
    add_definitions(-D_NETBSD_SOURCE)
    # Bare x86_64 cross-compilers do not predefine the target OS macro.
    # MINIX headers and the shared IPC/poll adapters need it for ABI selection.
    add_definitions(-D__minix -DJASH_TARGET_MINIX=1)
elseif(BEYONDBSD)
    set(OSIF_TARGET "beyondbsd")
    set(OSIF_SRC "c/osif/beyondbsd/syscall.c")
    # BeyondBSD osinfo not implemented yet — falls back to ENOSYS via
    # the POSIX-common functions; see Docs/ports/<target>-conformance.md.
    set(OSIF_OSINFO_SRC "")
    set(OSIF_FLAGS "-ffreestanding -nostdinc -fno-builtin -fcommon")
    add_definitions(-D__NetBSD__ -DBEAR_OS_NETBSD=1)
    include_directories(c)  # for jash_netbsd_compat.h
else()
    message(FATAL_ERROR "Unsupported OS: ${CMAKE_SYSTEM_NAME}. osif/ needs macos, linux, minix, netbsd, beyondbsd, or bethos.")
endif()

message(STATUS "[OSIF] Target: ${OSIF_TARGET} (${OSIF_SRC})")

set(JASH_MINIX_BEAR_SURFACE_HEADER "" CACHE FILEPATH
    "MINIX blibc/include/bear/surface.h for the MINIX Surface OSIF")
set(JASH_MINIX_BEAR_SURFACE_INCLUDE "")
if(OSIF_TARGET STREQUAL "minix" AND JASH_MINIX_BEAR_SURFACE_HEADER)
    if(NOT EXISTS "${JASH_MINIX_BEAR_SURFACE_HEADER}")
        message(FATAL_ERROR
            "JASH_MINIX_BEAR_SURFACE_HEADER must name the MINIX Bear Surface ABI header")
    endif()
    set(JASH_MINIX_BEAR_SURFACE_INCLUDE
        "${CMAKE_CURRENT_BINARY_DIR}/minix-bear-include")
    file(MAKE_DIRECTORY
        "${JASH_MINIX_BEAR_SURFACE_INCLUDE}/minix_surface_api")
    configure_file("${JASH_MINIX_BEAR_SURFACE_HEADER}"
        "${JASH_MINIX_BEAR_SURFACE_INCLUDE}/minix_surface_api/surface.h" COPYONLY)
elseif(OSIF_TARGET STREQUAL "minix")
    message(STATUS
        "Set JASH_MINIX_BEAR_SURFACE_HEADER to build the MINIX Surface OSIF")
endif()

# The guest supplies its own UAPI and Proc-NU SDK. No paths to a particular
# developer's checkout are baked into Jash; normal host builds stay untouched.
if(JASH_TINALINUX)
    foreach(required JASH_TINA_UAPI_DIR JASH_TINA_PROCNU_DIR)
        if(NOT IS_DIRECTORY "${${required}}")
            message(FATAL_ERROR "${required} must name an existing directory")
        endif()
    endforeach()
    add_compile_definitions(JASH_TINALINUX=1)
    include_directories(BEFORE "${JASH_TINA_PROCNU_DIR}")
    # Exported Linux UAPI uses GNU constructs such as __int128 and named
    # variadic macros; keep -Wpedantic/-Werror on Jash's own sources.
    include_directories(BEFORE SYSTEM "${JASH_TINA_UAPI_DIR}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()
if(JASH_BETHOS)
    add_compile_definitions(JASH_BETHOS=1)
    # BethOS exposes the guest's blocking serial read, not host POSIX
    # readiness/termios. Reuse the Beyond guest path for readline and boot.
    add_compile_definitions(JASH_TARGET_BEYOND=1 BEAR_NO_LOCAL_EXEC_TLS=1)
    include_directories(BEFORE "${JASH_BLIBC_SRC}/posix/include"
        "${JASH_BLIBC_SRC}/include" "${JASH_BLIBC_SRC}")
    set(JASH_BLIBC_COMPAT_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/c/osif/bethos/blibc_compat.h")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()
if(JASH_BLIBC)
    foreach(required JASH_BLIBC_SRC JASH_BLIBC_LIB JASH_BLIBC_GCCINC)
        if(NOT EXISTS "${${required}}")
            message(FATAL_ERROR "${required} must exist")
        endif()
    endforeach()
    if(JASH_BETHOS AND NOT JASH_BLIBC_DEFSYM)
        set(JASH_BLIBC_DEFSYM
            -Wl,--defsym=abs=bear_abs
            -Wl,--defsym=access=bear_access
            -Wl,--defsym=atexit=bear_atexit
            -Wl,--defsym=atoi=bear_atoi
            -Wl,--defsym=atol=bear_atol
            -Wl,--defsym=bsearch=bear_bsearch
            -Wl,--defsym=calloc=bear_calloc
            -Wl,--defsym=chmod=bear_chmod
            -Wl,--defsym=chown=bear_chown
            -Wl,--defsym=close=bear_close
            -Wl,--defsym=dprintf=bear_dprintf
            -Wl,--defsym=exit=bear_exit
            -Wl,--defsym=fchdir=bear_fchdir
            -Wl,--defsym=fprintf=bear_fprintf
            -Wl,--defsym=free=bear_free
            -Wl,--defsym=fstat=bear_fstat
            -Wl,--defsym=fstatat=bear_fstatat
            -Wl,--defsym=ftruncate=bear_ftruncate
            -Wl,--defsym=getenv=bear_getenv
            -Wl,--defsym=getline=bear_getline
            -Wl,--defsym=getrlimit=bear_getrlimit
            -Wl,--defsym=getrusage=bear_getrusage
            -Wl,--defsym=link=bear_link
            -Wl,--defsym=lstat=bear_lstat
            -Wl,--defsym=malloc=bear_malloc
            -Wl,--defsym=memset=bear_memset
            -Wl,--defsym=mkdir=bear_mkdir
            -Wl,--defsym=printf=bear_printf
            -Wl,--defsym=qsort=bear_qsort
            -Wl,--defsym=read=bear_read
            -Wl,--defsym=readlink=bear_readlink
            -Wl,--defsym=realloc=bear_realloc
            -Wl,--defsym=rename=bear_rename
            -Wl,--defsym=rmdir=bear_rmdir
            -Wl,--defsym=setenv=bear_setenv
            -Wl,--defsym=setrlimit=bear_setrlimit
            -Wl,--defsym=snprintf=bear_snprintf
            -Wl,--defsym=stat=bear_stat
            -Wl,--defsym=strlen=bear_strlen
            -Wl,--defsym=strncmp=bear_strncmp
            -Wl,--defsym=strtol=bear_strtol
            -Wl,--defsym=strtoll=bear_strtoll
            -Wl,--defsym=strtoul=bear_strtoul
            -Wl,--defsym=strtoull=bear_strtoull
            -Wl,--defsym=symlink=bear_symlink
            -Wl,--defsym=sysconf=bear_sysconf
            -Wl,--defsym=unsetenv=bear_unsetenv
            -Wl,--defsym=vsnprintf=bear_vsnprintf)
    endif()
    add_compile_definitions(JASH_BLIBC=1 BLIBC_STDINT_H=1)
    include_directories(BEFORE "${JASH_BLIBC_SRC}/posix/include"
        "${JASH_BLIBC_SRC}/include" "${JASH_BLIBC_SRC}")
    if(NOT JASH_BLIBC_COMPAT_HEADER)
        set(JASH_BLIBC_COMPAT_HEADER
            "${CMAKE_CURRENT_SOURCE_DIR}/c/osif/tina/blibc_compat.h")
    endif()
    add_compile_options(-isystem "${JASH_BLIBC_GCCINC}"
        "$<$<COMPILE_LANGUAGE:C>:-include${JASH_BLIBC_SRC}/include/stdint.h>"
        "$<$<COMPILE_LANGUAGE:C>:-includestring.h>"
        "$<$<COMPILE_LANGUAGE:C>:-include${JASH_BLIBC_COMPAT_HEADER}>"
        -ffreestanding -nostdlib -nostdinc -fno-stack-protector
        -U_FORTIFY_SOURCE -fno-pie -no-pie)
endif()

# ── LTO (Link-Time Optimization) for smaller binary + dead code elimination ──
if(NOT JASH_TINALINUX AND NOT JASH_BETHOS)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
# JINV-006 — otimizar para VELOCIDADE por padrão, tamanho sob demanda.
#
# O Release usava `-Oz` (mínimo tamanho). Medição de 2026-07-27: `-Oz` custa
# 30-39% de CPU no laço interpretador e compra ZERO em latência de startup — o
# binário menor não carrega mais rápido porque o custo de startup do jash é
# I/O e telemetria, não paging de texto. Para um shell que um agente LLM
# invoca MILHARES de vezes por sessão, essa é a troca errada.
#
# `-Oz` segue disponível para quem realmente precisa do binário pequeno
# (embedding — o caso do BeyondBSD): -DJASH_OPTIMIZE_SIZE=ON.
option(JASH_OPTIMIZE_SIZE "Otimizar para tamanho (-Oz) em vez de velocidade (-O2)" OFF)
if(JASH_OPTIMIZE_SIZE)
    set(CMAKE_C_FLAGS_RELEASE "-Oz -DNDEBUG")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Oz -g -DNDEBUG")
else()
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
endif()

# ── Aggressive dead code elimination ─────────────────────────────────────────
add_compile_options(-fdata-sections -ffunction-sections)
# ADR-0016 §3.2: unwind tables have zero consumers in this codebase
# (no backtrace()/execinfo anywhere in c/ — verified by grep). Drop them
# in Release; RelWithDebInfo keeps them for profilers.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-fno-asynchronous-unwind-tables -fno-unwind-tables)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # ld64 has no --gc-sections; -dead_strip is the equivalent.
    add_link_options(-Wl,-dead_strip)
else()
    add_link_options(-Wl,--gc-sections)
endif()

# ── AddressSanitizer (optional) ────────────────────────────────────────────
option(ENABLE_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_ASAN)
    # ADR-0036 W3: UBSan bundled with ASan — both are cheap to run together
    # and catch different bug classes (memory errors vs. signed-overflow /
    # misaligned-access / etc., the class BUG-040 was in).
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── Compiler warnings (ADR-0036 W6) ────────────────────────────────────────
# Base set + the W6 hardening flags. All project builds reject warnings;
# JASH_WERROR remains a configure-time guard for existing CI invocations.
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter
                    -Wformat=2 -Wvla -Wshadow -Wpointer-arith
                    -Wstrict-prototypes -Wmissing-prototypes
                    # -Wconversion -Wsign-conversion (too noisy for existing codebase)
                    # -Wmissing-field-initializers (in -Wextra) flags the 133
                    # builtin registry rows { "name", fn, "help" } for the
                    # implicit NULL argv_fn 4th field — C guarantees zero-init
                    # of omitted fields, so this is valid idiomatic code, not a
                    # bug. Same call the Linux kernel makes. -Wformat=2 etc.
                    # still catch the real defects.
                    -Wno-missing-field-initializers)
option(JASH_WERROR "Treat warnings as errors (required for every build)" ON)
if(NOT JASH_WERROR)
    message(FATAL_ERROR "Jash requires -Werror; JASH_WERROR=OFF is unsupported")
endif()
add_compile_options(-Werror)

# ── Security hardening flags ───────────────────────────────────────────────
# Stack protection, buffer overflow detection, PIE/ASLR
if(NOT JASH_BLIBC AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fstack-protector-strong)
    add_compile_definitions(_FORTIFY_SOURCE=2)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_options(-fPIE)
        add_link_options(-pie)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        # macOS PIE is default since 10.7, but make explicit
        add_compile_options(-fPIE)
        add_link_options(-pie)
    endif()
endif()

# ── Compile-time assertions ────────────────────────────────────────────────
# Ensure critical buffer sizes are powers of 2 and within limits
# These are verified at compile time via static_assert in headers
# (CMake doesn't handle function-style macros well on command line)
# add_compile_definitions(
#     JASH_CTASSERT_LINE_CAP=1/(!!(JASH_LINE_CAP && !(JASH_LINE_CAP & (JASH_LINE_CAP - 1))))
#     JASH_CTASSERT_EVAL_DEPTH=1/(!!(JASH_EVAL_MAX_DEPTH > 0 && JASH_EVAL_MAX_DEPTH <= 1000))
# )

# ── Static analysis integration ────────────────────────────────────────────
# cppcheck target
add_custom_target(cppcheck
    COMMAND cppcheck --enable=all --std=c11 --suppress=missingIncludeSystem
        -I c -I c/sys -I c/osif -I c/liquid
        --inline-suppr --force -j2
        c/
    COMMENT "Running cppcheck static analysis"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# clang-tidy target (if available)
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    # clang-tidy itself has no -j option. Avoid shell substitution/globbing:
    # an unescaped $(nproc) invalidates the entire Ninja manifest.
    file(GLOB_RECURSE JASH_TIDY_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/c/*.c")
    add_custom_target(clang-tidy
        COMMAND "${CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}"
            "-checks=*,-llvmlibc-*,-modernize-*,-readability-*,-google-*,-llvm-*,-misc-unused-parameters"
            ${JASH_TIDY_SOURCES}
        COMMENT "Running clang-tidy static analysis"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
endif()

# ── Definitions ────────────────────────────────────────────────────────────
add_definitions(-DJASH_TARGET_POSIX -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # _POSIX_C_SOURCE alone hides BSD extensions (DT_DIR/DT_REG, strcasecmp)
    # behind Darwin's strict-POSIX gate; _DARWIN_C_SOURCE re-exposes them.
    add_definitions(-D_DARWIN_C_SOURCE)
endif()

# ── Include paths ──────────────────────────────────────────────────────────
include_directories(c c/sys c/osif c/liquid libjef)

# C source files
set(JASH_SOURCES
    c/main.c
    c/boot.c
    c/function_defs.c
    c/job_control.c
    c/core/version.c
    c/core/state.c
    c/core/frecency.c
    c/core/predict.c
    c/core/env.c
    c/core/argv.c
    c/core/memo.c
    c/core/linediff.c
    c/core/claims.c
    c/core/jobs_bg.c
    c/core/fschange.c
    c/core/serve.c
    c/core/execution_ir.c
    c/core/agent_process.c
    c/core/agent_cgroup.c
    c/core/caps_json.c
    c/core/agent_contract.c
    c/core/agent_cancel.c
    c/core/query_cancel.c
    c/core/json_writer.c
    c/core/structured_output.c
    c/core/data_json.c
    c/core/data_emit.c
    c/core/data_value.c
    c/core/data_stream.c
    c/core/data_wire.c
    c/core/data_wire_decode.c
    c/core/data_wire_import.c
    c/core/data_filter.c
    c/builtins/discovery_output.c
    c/builtins/shell_output.c
    c/builtins/directory_data.c
    c/builtins/process_data.c
    c/builtins/jobs_data.c
    c/core/mutation_report.c
    c/core/recovery.c
    c/core/local_server.c
    c/osif/ipc.c
    c/osif/buffer.c
    c/builtins/builtin_effects.c
    c/builtins/builtin_assert.c
    c/builtins/builtin_why.c
    c/builtins/builtin_resume.c
    c/core/provenance.c
    c/core/history.c
    c/core/persist.c
    c/core/alias.c
    c/core/suggest.c
    c/core/glob.c
    c/core/live_reload.c
    c/core/cache.c
    c/core/smart_gate.c
    c/core/posix_parser.c
    c/core/autocomplete.c
    c/core/syntax_highlight.c
    c/core/unix_format.c
    c/core/jash_ai.c
    c/core/ai_observation.c
    c/core/ai_source_page.c
    c/core/ai_patch.c
    c/core/jash_qa.c
    c/core/agent_json.c
    c/core/token_tracker.c
    c/parser/eval.c
    c/parser/source_location.c
    c/parser/exec.c
    c/parser/pipeline.c
    c/parser/expand.c
    c/parser/param_expand.c
    c/core/var_cache.c
    c/parser/arithmetic.c
    c/parser/cmdsub.c
    c/parser/conditional.c
    c/parser/quotes.c
    c/parser/heredoc.c
    c/parser/script.c
    c/parser/for_loop.c
    c/parser/while_loop.c
    c/parser/if_block.c
    c/parser/case_stmt.c
    c/parser/select_loop.c
    c/parser/builtin_redir.c
    c/parser/coproc.c
    c/readline/loop.c
    c/readline/edit.c
    c/readline/render.c
    c/readline/keymap.c
    c/readline/complete.c
    c/readline/jux.c
    c/readline/balance.c
    c/builtins/registry.c
    c/builtins/cd.c
    c/builtins/pwd.c
    c/builtins/echo.c
    c/builtins/exit.c
    c/builtins/export.c
    c/builtins/unset.c
    c/builtins/mapfile.c
    c/builtins/env.c
    c/builtins/alias.c
    c/builtins/unalias.c
    c/builtins/history.c
    c/builtins/fc.c
    c/builtins/help.c
    c/builtins/type.c
    c/builtins/which.c
    c/builtins/version.c
    c/builtins/clear.c
    c/builtins/true_false.c
    c/builtins/builtin_test.c
    c/builtins/debug.c
    c/builtins/builtin_lint.c
    c/builtins/builtin_jobs.c
    c/builtins/builtin_wait.c
    c/builtins/builtin_blamefile.c
    c/builtins/builtin_distill.c
    c/builtins/builtin_fg.c
    c/builtins/builtin_bg.c
    c/builtins/builtin_disown.c
    c/builtins/builtin_suspend.c
    c/builtins/builtin_return.c
    c/builtins/builtin_break.c
    c/builtins/builtin_continue.c
    c/builtins/source.c
    c/builtins/printf.c
    c/builtins/read.c
    c/builtins/exec.c
    c/builtins/eval.c
    c/builtins/trap.c
    c/builtins/let.c
    c/builtins/local.c
    c/builtins/readonly.c
    c/builtins/misc.c
    c/builtins/shift.c
    c/builtins/getopts.c
    c/builtins/pushd.c
    c/builtins/popd.c
    c/builtins/dirs.c
    c/builtins/abbr.c
    c/builtins/juxconfig.c
    c/builtins/builtin_set.c
    c/builtins/shopt.c
    c/builtins/compgen.c
    c/builtins/complete.c
    c/builtins/array.c
    c/builtins/declare.c
    c/builtins/builtin_builtin.c
    c/builtins/enable.c
    c/builtins/builtin_hash.c
    c/builtins/builtin_logout.c
    c/builtins/builtin_plugin.c
    c/builtins/calc.c
    c/builtins/builtin_persist.c
    c/builtins/builtin_theme.c
    c/builtins/builtin_statusbar.c
    c/builtins/builtin_spinner.c
    c/builtins/builtin_kill.c
    c/builtins/ls_collector.c
    c/builtins/ls_data.c
    c/builtins/builtin_ls.c
    c/builtins/builtin_touch.c
    c/builtins/builtin_cat.c
    c/builtins/builtin_find.c
    c/builtins/builtin_glob.c
    c/builtins/builtin_sed.c
    c/builtins/builtin_wc.c
    c/builtins/builtin_stat.c
    c/builtins/builtin_xargs.c
    c/builtins/builtin_diff.c
    c/builtins/builtin_awk.c
    c/builtins/builtin_rm.c
    c/builtins/builtin_trash.c
    c/builtins/builtin_tread.c
    c/builtins/builtin_grep.c
    c/builtins/builtin_timeout.c
    c/builtins/builtin_cp.c
    c/builtins/builtin_mv.c
    c/builtins/builtin_ps.c
    c/builtins/builtin_proc.c
    c/builtins/builtin_du.c
    c/builtins/builtin_df.c
    c/builtins/builtin_pipe.c
    c/builtins/builtin_impact.c
    c/builtins/builtin_handoff.c
    c/builtins/builtin_recall.c
    c/builtins/builtin_checkpoint.c
    c/builtins/builtin_expect.c
    c/builtins/builtin_explain.c
    c/builtins/builtin_mesh.c
    c/builtins/builtin_gap.c
    c/builtins/builtin_dashboard.c
    c/builtins/builtin_config.c
    c/builtins/builtin_context.c
    c/builtins/builtin_agent.c
    c/plugin.c
    c/theme/prompt.c
    c/theme/banner.c
    c/theme/io.c
    c/theme/theme.c
    c/readline/autopair.c
    c/readline/palette.c
    c/readline/bracket.c
    c/core/error_exp.c
    c/core/record.c
    c/ui/statusbar.c
    c/easter/bear_egg.c
    c/output/highlighter.c
    c/ui/progress.c
    c/editor/snippet.c
    c/completion/smart_suggestions.c
    c/osif/rust_ffi_stub.c
    c/osif/posix_common.c
    ${OSIF_SRC}
    ${OSIF_OSINFO_SRC}
    $<$<BOOL:${JASH_BETHOS}>:c/osif/bethos/posix_compat.c>
    $<$<BOOL:${JASH_TINALINUX}>:${JASH_TINA_PROCNU_DIR}/tina_procnu.c>
    $<$<BOOL:${JASH_TINALINUX}>:c/osif/tina/surface.c>
    c/builtins/osif.c
    c/builtins/osif_port.c
    c/builtins/uname.c
    c/core/timeline.c
    c/builtins/builtin_intent.c
    c/module/loader.c
    c/module/sigil.c
    c/module/jash_crypto.c
    c/module/blake3.c
    libjef/ed25519.c          # W4b: Ed25519 in core so modules resolve the symbols
    libjef/jef_verify.c       # W3b: libjef verifier in core; the module delegates to it
    c/builtins/module.c
    c/builtins/sigil.c
    c/core/session_meta.c
    c/core/err_digest.c
    c/core/output_budget.c
    c/core/salience.c
    c/core/gold.c
    c/core/prefetch.c
    c/core/drift.c
    c/core/risk_predict.c
    c/core/llm_status.c
    c/core/guard.c
    c/core/differentiators.c
    c/core/adopt.c
    c/core/preexec.c
    c/core/nudge.c
    c/core/hints.c
    c/core/chain_read.c
    c/core/loopguard.c
    c/core/world.c
    c/core/jash_mcache.c
    c/core/jash_brain.c
    c/core/jash_fastpath.c
    c/core/jash_narrator.c
    c/core/jash_pia.c
    c/core/jash_invoice.c
    c/core/digest_engine.c
    c/core/context_engine.c
    c/core/stance.c
    c/core/provider_git.c
    c/core/provider_state.c
    c/core/provider_digest.c
    c/liquid/jash_liquid.c
    c/builtins/builtin_digest.c
    c/builtins/builtin_state.c
    c/builtins/builtin_json.c
    c/builtins/builtin_speculate.c
    c/builtins/builtin_revert.c
    c/builtins/builtin_cost.c
    c/builtins/builtin_contract.c
    c/builtins/builtin_agent_guard.c
    c/builtins/builtin_agent_guide.c
    c/builtins/builtin_adopt.c
    c/builtins/builtin_brain.c
    c/builtins/builtin_goal.c
    c/builtins/builtin_llm_invoice.c
    c/builtins/builtin_change.c
    c/builtins/builtin_mistake.c
    c/builtins/builtin_token.c
    c/builtins/builtin_learn.c
    c/builtins/builtin_predict.c
    c/builtins/builtin_preference.c
    c/builtins/builtin_reflect.c
    c/builtins/builtin_selfcheck.c
    c/builtins/builtin_prompt_forge.c
    c/journal/journal.c
    c/builtins/builtin_journal.c
    c/builtins/builtin_stance.c
    c/builtins/builtin_breathe.c
    c/builtins/builtin_mirror.c
    c/builtins/builtin_gossip.c
    c/builtins/builtin_sense.c
    c/builtins/builtin_forge.c
    c/builtins/builtin_weave.c
    c/builtins/builtin_metaops.c

    # ADR-0045: Jash Graph Engine (knowledge graph for code understanding)
    c/graph/jash_arena.c
    c/graph/jash_nodes.c
    c/graph/jash_algo.c
    c/graph/jash_files.c
    c/graph/jash_parse.c
    c/builtins/builtin_graph.c
    c/builtins/builtin_mem.c
)

if(OSIF_TARGET STREQUAL "minix")
    list(APPEND JASH_SOURCES
        c/osif/minix/poll.c
        c/osif/minix/surface.c
    )
endif()

# jash-agentd shares the shell evaluator and state model, but its link closure is
# deliberately source-isolated from every human and dynamic-extension surface.
set(JASH_AGENT_SOURCES ${JASH_SOURCES})
list(REMOVE_ITEM JASH_AGENT_SOURCES
    c/main.c
    c/osif/minix/surface.c
    c/boot.c
    c/plugin.c
    c/module/loader.c
    c/liquid/jash_liquid.c
    c/core/jash_ai.c
    c/core/suggest.c
    c/core/autocomplete.c
    c/core/syntax_highlight.c
    c/theme/prompt.c
    c/theme/banner.c
    c/theme/theme.c
    c/output/highlighter.c
    c/ui/statusbar.c
    c/ui/progress.c
    c/editor/snippet.c
    c/completion/smart_suggestions.c
    c/easter/bear_egg.c
    c/readline/loop.c
    c/readline/edit.c
    c/readline/render.c
    c/readline/keymap.c
    c/readline/complete.c
    c/readline/jux.c
    c/readline/balance.c
    c/readline/autopair.c
    c/readline/palette.c
    c/readline/bracket.c
    c/builtins/fc.c
    c/builtins/source.c
    c/builtins/eval.c
    c/builtins/compgen.c
    c/builtins/complete.c
    c/builtins/juxconfig.c
    c/builtins/builtin_plugin.c
    c/builtins/module.c
    c/builtins/sigil.c
    c/builtins/builtin_theme.c
    c/builtins/builtin_statusbar.c
    c/builtins/builtin_spinner.c
    c/builtins/builtin_brain.c
    c/builtins/clear.c
)
list(APPEND JASH_AGENT_SOURCES c/agent/jash_agentd_main.c)

option(JASH_STATIC_AGENT_RUNTIME
    "Link the agent runtime and independent verifier as static musl executables" OFF)
if(JASH_STATIC_AGENT_RUNTIME)
    get_filename_component(_jash_compiler_name "${CMAKE_C_COMPILER}" NAME)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT _jash_compiler_name MATCHES "musl")
        message(FATAL_ERROR
            "JASH_STATIC_AGENT_RUNTIME requires a Linux musl compiler")
    endif()
endif()

# ── osif target-specific compile flags ─────────────────────────────────────
set_source_files_properties(${OSIF_SRC} PROPERTIES COMPILE_FLAGS "${OSIF_FLAGS}")

# ── libjef: sovereign attestation crypto as a standalone, linkable library ──
#    (ROADMAP W3 / ADR-0003 S0) The crypto layer is self-contained and
#    dependency-free, so any C program can link libjef to embed a BLAKE3
#    hash-chain + SHA-256/HMAC attestation without pulling in the whole shell.
#    Built as a SEPARATE artifact: jash keeps compiling these sources itself
#    (so dlopen'd modules still resolve the jash_* crypto symbols), and libjef
#    is the reusable deliverable + standalone test target. Not built by the
#    default `--target jash`; build explicitly: `cmake --build B --target jef`.
add_library(jef STATIC c/module/jash_crypto.c c/module/blake3.c libjef/ed25519.c
                       libjef/jef_verify.c)
target_include_directories(jef PUBLIC c libjef)
set_target_properties(jef PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_executable(jef_test libjef/test_jef.c)
target_link_libraries(jef_test jef)
set_target_properties(jef_test PROPERTIES EXCLUDE_FROM_ALL TRUE)

if(BUILD_TESTING)
    if(OSIF_TARGET STREQUAL "minix")
        if(JASH_MINIX_BEAR_SURFACE_INCLUDE)
            add_test(NAME minix_osif_cmake_target_flags
                COMMAND python3
                    ${CMAKE_SOURCE_DIR}/tests/test_minix_cmake_target_flags.py
                    ${CMAKE_BINARY_DIR}/compile_commands.json
                    ${JASH_MINIX_BEAR_SURFACE_INCLUDE})
        else()
            add_test(NAME minix_osif_cmake_target_flags
                COMMAND python3
                    ${CMAKE_SOURCE_DIR}/tests/test_minix_cmake_target_flags.py
                    ${CMAKE_BINARY_DIR}/compile_commands.json)
        endif()
    endif()
    add_test(NAME ls_pretty
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_ls_pretty.sh)
    add_test(NAME ls_locale_sort
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_ls_locale_sort.sh)
    add_test(NAME cat_highlight
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_cat_highlight.sh)
    add_test(NAME ai_conversation_memory
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/test_ai_conversation_memory.py)
    add_test(NAME ai_auto_mode
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_ai_auto_mode.sh)
    add_executable(test_jash_fastpath
        tests/test_jash_fastpath.c
        c/core/jash_fastpath.c)
    target_include_directories(test_jash_fastpath PRIVATE c)
    add_test(NAME jash_fastpath COMMAND test_jash_fastpath)

    add_executable(test_data_value
        tests/test_data_value.c
        c/core/data_value.c
        c/module/blake3.c)
    target_include_directories(test_data_value PRIVATE c)
    add_test(NAME data_value COMMAND test_data_value)

    add_executable(test_data_json
        tests/test_data_json.c
        c/core/data_json.c
        c/core/data_value.c
        c/core/json_writer.c
        c/module/blake3.c)
    target_include_directories(test_data_json PRIVATE c)
    target_link_libraries(test_data_json PRIVATE m)
    add_test(NAME data_json COMMAND test_data_json)

    add_executable(test_data_emit
        tests/test_data_emit.c
        c/core/data_emit.c
        c/core/data_value.c
        c/core/data_stream.c
        c/core/data_wire.c
        c/core/data_wire_decode.c
        c/core/data_wire_import.c
        c/module/blake3.c)
    target_include_directories(test_data_emit PRIVATE c)
    add_test(NAME data_emit COMMAND test_data_emit)

    add_executable(test_query_cancel
        tests/test_query_cancel.c
        c/core/query_cancel.c)
    target_include_directories(test_query_cancel PRIVATE c)
    add_test(NAME query_cancel COMMAND test_query_cancel)

    add_executable(test_memory_store_cooperative tests/test_memory_store_cooperative.c)
    target_include_directories(test_memory_store_cooperative PRIVATE c)
    target_compile_options(test_memory_store_cooperative PRIVATE -UNDEBUG)
    add_test(NAME memory_store_cooperative COMMAND test_memory_store_cooperative)

    add_executable(test_chain_tokenize tests/test_chain_tokenize.c c/core/chain_read.c)
    target_include_directories(test_chain_tokenize PRIVATE c)
    target_compile_options(test_chain_tokenize PRIVATE -UNDEBUG)
    add_test(NAME chain_tokenize COMMAND test_chain_tokenize)

    add_executable(test_gold_delta_checked tests/test_gold_delta_checked.c
        c/core/gold.c c/module/blake3.c)
    target_include_directories(test_gold_delta_checked PRIVATE c)
    target_compile_definitions(test_gold_delta_checked PRIVATE _GNU_SOURCE)
    target_compile_options(test_gold_delta_checked PRIVATE -UNDEBUG)
    add_test(NAME gold_delta_checked COMMAND test_gold_delta_checked)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_digest_engine_unit
            tests/test_digest_engine_unit.c c/builtins/builtin_digest.c
            c/builtins/builtin_json.c c/core/digest_engine.c c/core/json_writer.c
            c/core/argv.c c/theme/io.c c/module/blake3.c c/module/jash_crypto.c
            c/core/adopt.c c/core/output_budget.c c/core/structured_output.c
            c/core/data_json.c c/core/data_value.c c/core/data_emit.c
            c/core/data_stream.c c/core/data_wire.c c/core/data_wire_decode.c
            c/core/data_wire_import.c c/parser/builtin_redir.c)
        target_include_directories(test_digest_engine_unit PRIVATE c c/sys)
        target_compile_options(test_digest_engine_unit PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(test_digest_engine_unit PRIVATE -Wl,--gc-sections
            -Wl,--wrap=read -Wl,--wrap=fsync -Wl,--wrap=fstatat)
        target_link_libraries(test_digest_engine_unit PRIVATE m pthread)
    endif()

    if(OSIF_TARGET STREQUAL "linux")
        add_executable(test_linux_mounts tests/test_linux_mounts.c)
        target_include_directories(test_linux_mounts PRIVATE c c/osif)
        add_test(NAME linux_mounts COMMAND test_linux_mounts)
    endif()

    add_executable(test_data_stream
        tests/test_data_stream.c
        c/core/data_stream.c
        c/core/data_value.c
        c/module/blake3.c)
    target_include_directories(test_data_stream PRIVATE c)
    add_test(NAME data_stream COMMAND test_data_stream)

    add_executable(test_ls_collector
        tests/test_ls_collector.c
        c/builtins/ls_collector.c)
    target_include_directories(test_ls_collector PRIVATE c)
    add_test(NAME ls_collector COMMAND test_ls_collector)

    add_executable(test_ls_data
        tests/test_ls_data.c
        c/builtins/ls_data.c
        c/builtins/ls_collector.c
        c/core/data_wire.c
        c/core/data_wire_decode.c
        c/core/data_wire_import.c
        c/core/data_stream.c
        c/core/data_value.c
        c/module/blake3.c)
    target_include_directories(test_ls_data PRIVATE c)
    add_test(NAME ls_data COMMAND test_ls_data)

    add_executable(test_data_wire
        tests/test_data_wire.c
        c/core/data_wire.c
        c/core/data_wire_decode.c
        c/core/data_wire_import.c
        c/core/data_stream.c
        c/core/data_value.c
        c/module/blake3.c)
    target_include_directories(test_data_wire PRIVATE c)
    add_test(NAME data_wire COMMAND test_data_wire)

    add_executable(test_data_filter
        tests/test_data_filter.c
        c/core/data_filter.c
        c/builtins/ls_data.c
        c/builtins/ls_collector.c
        c/core/data_stream.c
        c/core/data_value.c
        c/module/blake3.c)
    target_include_directories(test_data_filter PRIVATE c)
    add_test(NAME data_filter COMMAND test_data_filter)

    add_executable(test_ai_source_page
        tests/test_ai_source_page.c
        c/core/ai_source_page.c
        c/core/json_writer.c
        c/module/blake3.c)
    target_include_directories(test_ai_source_page PRIVATE c)
    add_test(NAME ai_source_page COMMAND test_ai_source_page)

    # Sealed memfd observations and JDF1 FD handoff are Linux-qualified.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_ai_observation
            tests/test_ai_observation.c
            c/core/ai_observation.c
            c/core/data_filter.c
            c/osif/buffer.c
            c/osif/ipc.c
            c/builtins/ls_data.c
            c/builtins/ls_collector.c
            c/core/json_writer.c
            c/core/data_stream.c
            c/core/data_wire.c
            c/core/data_wire_decode.c
            c/core/data_value.c
            c/module/blake3.c)
        target_include_directories(test_ai_observation PRIVATE c)
        add_test(NAME ai_observation COMMAND test_ai_observation)

        add_executable(test_ai_observation_bounds
            tests/test_ai_observation_bounds.c
            c/core/ai_observation.c
            c/core/data_filter.c
            c/osif/buffer.c
            c/osif/ipc.c
            c/builtins/ls_data.c
            c/builtins/ls_collector.c
            c/core/json_writer.c
            c/core/data_stream.c
            c/core/data_wire.c
            c/core/data_wire_decode.c
            c/core/data_value.c
            c/module/blake3.c)
        target_include_directories(test_ai_observation_bounds PRIVATE c)
        target_compile_definitions(test_ai_observation_bounds PRIVATE
            AI_OBSERVATION_REF_MAX=2048)
        add_test(NAME ai_observation_bounds COMMAND test_ai_observation_bounds)

        add_executable(test_wire_sealed_import
            tests/test_wire_sealed_import.c
            c/core/data_wire.c
            c/core/data_wire_decode.c
            c/core/data_wire_import.c
            c/core/data_stream.c
            c/core/data_value.c
            c/osif/buffer.c
            c/osif/ipc.c
            c/module/blake3.c)
        target_include_directories(test_wire_sealed_import PRIVATE c)
        add_test(NAME wire_sealed_import COMMAND test_wire_sealed_import)
    endif()

    add_executable(test_shared_snapshot
        tests/test_shared_snapshot.c
        c/core/fschange.c
        c/core/world.c
        c/core/json_writer.c)
    target_include_directories(test_shared_snapshot PRIVATE c)
    add_test(NAME shared_snapshot COMMAND test_shared_snapshot)

    add_executable(test_world_linear
        tests/test_world_linear.c
        c/core/world.c
        c/core/json_writer.c)
    target_include_directories(test_world_linear PRIVATE c)
    add_test(NAME world_linear COMMAND test_world_linear)

    add_executable(test_array_index
        tests/test_array_index.c
        c/core/env.c)
    target_include_directories(test_array_index PRIVATE c)
    add_test(NAME array_index COMMAND test_array_index)
    add_test(NAME array_index_integration
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_array_index.sh
                     ${CMAKE_BINARY_DIR})

    add_executable(test_agent_process_portability
        tests/test_agent_process_portability.c
        c/core/execution_ir.c
        c/core/agent_cgroup.c)
    target_include_directories(test_agent_process_portability PRIVATE c)
    add_test(NAME agent_process_portability COMMAND test_agent_process_portability)
    set_tests_properties(agent_process_portability PROPERTIES TIMEOUT 90)

    add_executable(test_agent_process_ir
        tests/test_agent_process_ir.c
        c/core/execution_ir.c
        c/core/agent_process.c
        c/core/agent_cgroup.c)
    target_include_directories(test_agent_process_ir PRIVATE c)
    target_compile_definitions(test_agent_process_ir PRIVATE
        JASH_AGENT_PROCESS_TEST_CLOSE_RANGE_FALLBACK=1
        JASH_AGENT_PROCESS_TEST_DRAIN_DELAY_US=5000)
    add_test(NAME agent_process_ir COMMAND test_agent_process_ir)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_agent_process_children
            tests/test_agent_process_children.c
            c/core/execution_ir.c
            c/core/agent_cgroup.c)
        target_include_directories(test_agent_process_children PRIVATE c)
        target_link_options(test_agent_process_children PRIVATE
            -Wl,--wrap=open -Wl,--wrap=openat)
        add_test(NAME agent_process_children COMMAND test_agent_process_children)
    endif()
endif()

# jef_verify: standalone CLI verifier (links ONLY libjef — no jash). Proves
# third-party verification of real jash chain/proof artifacts. (ROADMAP W3b)
add_executable(jef_verify libjef/jef_verify_main.c)
target_link_libraries(jef_verify jef)
set_target_properties(jef_verify PROPERTIES EXCLUDE_FROM_ALL TRUE)

if(JASH_STATIC_AGENT_RUNTIME)
    set_target_properties(jef jef_verify PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION FALSE)
    target_link_options(jef_verify PRIVATE -static)
endif()

# ── Main executable ────────────────────────────────────────────────────────
# Safety: build as jash-dev so link failures never clobber build/jash
# (the shell binary the tool depends on). Copy to jash only on success.
if(JASH_TINALINUX)
    list(REMOVE_ITEM JASH_SOURCES c/core/jash_ai.c)
    list(APPEND JASH_SOURCES c/osif/tina/ef0_ai.c)
endif()
add_executable(jash ${JASH_SOURCES})
if(JASH_MINIX_BEAR_SURFACE_INCLUDE)
    target_include_directories(jash BEFORE PRIVATE
        "${JASH_MINIX_BEAR_SURFACE_INCLUDE}")
endif()
add_dependencies(jash jash_gitversion)
set_target_properties(jash PROPERTIES OUTPUT_NAME jash-dev)

# ── ADR-0016 Wave 1: link-layer diet ────────────────────────────────────────
# §3.1 The plugin ABI is an explicit export list, NOT -rdynamic.
#      -rdynamic exported all ~565 globals, bloating .dynsym/.dynstr and
#      blocking LTO internalization of the entire .text.
if(JASH_BLIBC)
    # A static guest binary has no dynamic symbol table or relative relocations.
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(jash PRIVATE
        -Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/c/plugin_exports_macos.txt)
else()
    target_link_options(jash PRIVATE
        -Wl,--dynamic-list=${CMAKE_SOURCE_DIR}/c/plugin_exports.list)
    # §3.3 DT_RELR packed relative relocations (1316 × 24B → ~1KB bitmap).
    #      Needs glibc ≥ 2.36 at runtime; the binary already requires 2.38.
    target_link_options(jash PRIVATE -Wl,-z,pack-relative-relocs)
    # §3.4 Never pick up NEEDED entries we don't use.
    target_link_options(jash PRIVATE -Wl,--as-needed)
endif()

# §3.2 (unwind tables) is applied globally near the top of this file —
# it must precede add_executable to take effect.

# §3.4 Truthful link line: libm only. dl/pthread live inside libc on
#      glibc ≥ 2.34 and inside libSystem on Darwin; curl was never linked
#      (the AI layer shells out to the curl binary).
#      NetBSD keeps pthread in libpthread.so — link it there.
if(JASH_BLIBC)
    target_link_options(jash PRIVATE -nostartfiles -nodefaultlibs -nostdlib
        -no-pie -static ${JASH_BLIBC_DEFSYM})
    target_link_libraries(jash "${JASH_BLIBC_LIB}" gcc)
else()
    target_link_libraries(jash m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_link_libraries(jash pthread)
endif()

# Persistent JSONL runtime for agents. No plugin export list, dynamic
# module loader, or human-shell source is part of this target, and it is not
# installed while the protocol remains intentionally minimal.
set(JASH_AGENT_BINARY_NAME "jash-agentd" CACHE STRING
    "Agent runtime target/output name for downstream packaging")
if(NOT JASH_AGENT_BINARY_NAME MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR "invalid JASH_AGENT_BINARY_NAME")
endif()
add_executable(${JASH_AGENT_BINARY_NAME} ${JASH_AGENT_SOURCES})
add_dependencies(${JASH_AGENT_BINARY_NAME} jash_gitversion)
target_compile_definitions(${JASH_AGENT_BINARY_NAME} PRIVATE
    JASH_AGENT_ONLY=1 JASH_AGENT_RUNTIME_NAME="${JASH_AGENT_BINARY_NAME}")
target_include_directories(${JASH_AGENT_BINARY_NAME} PRIVATE c c/sys c/osif libjef)
if(JASH_BLIBC)
    # The agent runtime is a separate executable from /bin/jash. Apply the
    # same Tina/blibc start, static link and exact symbol aliases; otherwise
    # a successful shell build masks an agentd linked to host libm/glibc.
    target_link_options(${JASH_AGENT_BINARY_NAME} PRIVATE
        -nostartfiles -nodefaultlibs -nostdlib -no-pie -static ${JASH_BLIBC_DEFSYM})
    target_link_libraries(${JASH_AGENT_BINARY_NAME} "${JASH_BLIBC_LIB}" gcc)
else()
    target_link_libraries(${JASH_AGENT_BINARY_NAME} m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_link_libraries(${JASH_AGENT_BINARY_NAME} pthread)
endif()
if(BUILD_TESTING)
    find_package(Python3 3.9 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME bash_lineno_contract
            COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_SOURCE_DIR}/tests/test_bash_lineno.py
                --jash $<TARGET_FILE:jash>)
    endif()
    add_test(NAME agent_jobs_protocol
        COMMAND ${CMAKE_COMMAND} -E env
            JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
            bash ${CMAKE_SOURCE_DIR}/tests/test_agent_jobs_protocol.sh)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 3.9 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME sigil_log_retention
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_TEST_BINARY=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_sigil_log_retention.py)
            add_test(NAME literal_dispatch
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_literal_dispatch.py)
            add_test(NAME ai_tool_preview
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ai_tool_preview.py)
            add_test(NAME prediction_scope
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_prediction_scope.py)
            add_test(NAME ls_json_collector
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ls_json_collector.py)
            add_test(NAME ls_data_wire
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ls_data_wire.py)
            add_test(NAME ai_typed_observation
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ai_typed_observation.py)
            add_test(NAME ai_auto_session
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ai_auto_session.py)
            add_test(NAME ai_ref_promotion
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ai_ref_promotion.py)
            add_test(NAME python_local_client
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_python_local_client.py)
            add_test(NAME ai_tool_adapters
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_ai_tool_adapters.py)
            add_test(NAME jash_mcp
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_jash_mcp.py)
            add_test(NAME werror_required
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_werror_required.sh)
            add_test(NAME agent_contract_inventory
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_agent_contract_inventory.py)
            add_test(NAME agent_json_roundtrip
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_agent_json_roundtrip.py)
            add_test(NAME agent_mutations_json
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_agent_mutations_json.py)
            add_test(NAME agent_recovery_refs
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_agent_recovery_refs.py)
            foreach(gate IN ITEMS osif_native proc_observation state_native speculate_native grep_semantic_data assert_native ls_extended expect_native change_native context_native impact_native contract_native graph_native debug_native sigil_transitions_native structured_recipes analysis_native digest_native mem_native structured_gap sigil_queries_native structured_data_budget sigil_actions_native handoff_native recall_native structured_dashboard fs_contract_schemas structured_guidance history_native mesh_native_data structured_learning recovery_extra_native memory_native trash_native structured_models coordination_data proc_snapshot_data serve_delivery structured_cli_adapters awk_native_data process_control_data recovery_native df_baseline_data structured_sdk process_native_data shell_admission management_admission structured_calc diff_native_data tread_native_data structured_adapters grep_native_data repo_identity du_native_data shell_native_profiles management_builtins agent_mutations_data query_native_cancel discovery_native_data shell_structured discovery_builtins query_native_data builtin_mode_catalog serve_capture_bytes builtin_json_integrity structured_pipe ls_argv_boundaries df_osif_errors agent_queries_json agent_state_evidence agent_contract_matrix agent_local_sessions agent_storage_recovery agent_atomic_mutations agent_builtin_cancel agent_local_cancel agent_job_disconnect agent_listener_shutdown agent_artifact_store agent_maintenance_transactions)
                add_test(NAME ${gate}
                    COMMAND ${CMAKE_COMMAND} -E env
                        JASH=$<TARGET_FILE:jash>
                        JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                        ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_${gate}.py)
                set_tests_properties(${gate} PROPERTIES LABELS "builtin-contracts")
            endforeach()
            if(TARGET test_digest_engine_unit)
                add_test(NAME digest_project
                    COMMAND ${CMAKE_COMMAND} -E env
                        DIGEST_BINARY=$<TARGET_FILE:jash>
                        DIGEST_UNIT_BINARY=$<TARGET_FILE:test_digest_engine_unit>
                        ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_digest_project.py)
                set_tests_properties(digest_project PROPERTIES LABELS "builtin-contracts")
            endif()
            set_tests_properties(agent_contract_inventory data_value data_json data_emit
                data_stream data_wire data_filter ls_data ls_collector query_cancel
                memory_store_cooperative chain_tokenize gold_delta_checked agent_process_children
                agent_mutations_json agent_recovery_refs
                ls_data_wire ls_json_collector ls_pretty
                PROPERTIES LABELS "builtin-contracts")
            if(TARGET test_linux_mounts)
                set_tests_properties(linux_mounts PROPERTIES LABELS "builtin-contracts")
            endif()
            # Declared semantic evidence must actually execute in the labelled
            # qualification run. Keep the original shell gates and their
            # assertions; master-suite registration alone is not evidence.
            foreach(legacy_gate IN ITEMS awk breathe context_ask diff distill expect expect_waves
                    gold_contract graph grep_gold impact jinv099_101_sigil_recovery
                    jinv104_grep_include_union jinv106_sigil_boundary_trust
                    jinv127_grep_cluster ls_delta ls_json mesh mesh_claim mesh_fleet
                    revert rewind_scope speculate speculate_scope state trash_toggle
                    undo_waves weave)
                add_test(NAME contract_legacy_${legacy_gate}
                    COMMAND ${CMAKE_COMMAND} -E env
                        JASH=$<TARGET_FILE:jash> JASH_SIGIL=0
                        bash ${CMAKE_SOURCE_DIR}/tests/test_${legacy_gate}.sh)
                set_tests_properties(contract_legacy_${legacy_gate} PROPERTIES
                    LABELS "builtin-contracts" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            endforeach()
            add_test(NAME builtin_conformance
                COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_builtin_conformance.py)
            set_tests_properties(builtin_conformance PROPERTIES LABELS "builtin-contracts")
            # Six separately selectable gates for the Linux W09–W14 slice.
            foreach(data_gate IN ITEMS Budget Config Transport FD Buffers Artifacts)
                add_test(NAME agent_data_${data_gate}
                    COMMAND ${CMAKE_COMMAND} -E env
                        JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                        JASH_VERIFIER_BIN=$<TARGET_FILE:jef_verify>
                        ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_agent_data_plane.py ${data_gate})
            endforeach()
            add_test(NAME agent_binary_streams
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_agent_binary_streams.py)
            add_test(NAME agent_artifact_pipeline
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_agent_artifact_pipeline.py)
            # CTest must work after a default testing build, including the
            # standalone verifier used by the new cross-artifact gates.
            set_target_properties(jef_verify PROPERTIES EXCLUDE_FROM_ALL FALSE)
            add_test(NAME assert_dependencies
                COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_assert_dependencies.py
                    --binary $<TARGET_FILE:jash>)
            set_tests_properties(assert_dependencies PROPERTIES LABELS "builtin-contracts")
            add_test(NAME autonomy_sandbox
                COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_autonomy_sandbox.py)
            set_tests_properties(autonomy_sandbox PROPERTIES SKIP_RETURN_CODE 77)
            add_test(NAME autonomy_workflow
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_BUILD_DIR=${CMAKE_BINARY_DIR}
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    JASH_VERIFIER_BIN=$<TARGET_FILE:jef_verify>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_autonomy_workflow.py)
            add_test(NAME libjef_job_spec
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_BUILD_DIR=${CMAKE_BINARY_DIR}
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    JASH_VERIFIER_BIN=$<TARGET_FILE:jef_verify>
                    PYTHON_BIN=${Python3_EXECUTABLE}
                    JASH_SKIP_BUILD=1
                    bash ${CMAKE_SOURCE_DIR}/tests/test_libjef_job_spec.sh)
            add_test(NAME libjef_input_bounds
                COMMAND ${CMAKE_COMMAND} -E env
                    CC=${CMAKE_C_COMPILER}
                    JEF_SANITIZE=1
                    JASH_BIN=$<TARGET_FILE:jash>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_libjef_input_bounds.py)
            set_tests_properties(libjef_input_bounds PROPERTIES TIMEOUT 600)
            add_test(NAME suite_runner
                COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_suite_runner.py)
            add_test(NAME autonomy_publication
                COMMAND ${CMAKE_COMMAND} -E env
                    JASH_BUILD_DIR=${CMAKE_BINARY_DIR}
                    JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                    JASH_VERIFIER_BIN=$<TARGET_FILE:jef_verify>
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_SOURCE_DIR}/tests/test_autonomy_publication.py)
            add_test(NAME agent_resources
                COMMAND ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER}
                    ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_agent_resources.py)
            foreach(resource_test IN ITEMS agent_resource_protocol autonomy_resources)
                add_test(NAME ${resource_test}
                    COMMAND ${CMAKE_COMMAND} -E env
                        JASH_BUILD_DIR=${CMAKE_BINARY_DIR}
                        JASH_AGENTD_BIN=$<TARGET_FILE:${JASH_AGENT_BINARY_NAME}>
                        JASH_VERIFIER_BIN=$<TARGET_FILE:jef_verify>
                        ${Python3_EXECUTABLE} -B
                        ${CMAKE_SOURCE_DIR}/tests/test_${resource_test}.py)
            endforeach()
            set_tests_properties(agent_resources agent_resource_protocol autonomy_resources
                PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()
if(JASH_STATIC_AGENT_RUNTIME)
    target_link_options(${JASH_AGENT_BINARY_NAME} PRIVATE -static)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(${JASH_AGENT_BINARY_NAME} PRIVATE
        -Wl,-z,pack-relative-relocs -Wl,--as-needed)
endif()

# §3.5 The release artifact is stripped as part of the build, not by hand.
#      Benchmarks and size claims measure jash-release; build/jash keeps
#      symbols for addr2line/debugging.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(JASH_STRIP_FLAGS -x)   # ld64 strip needs -x to keep exported dynamic symbols
endif()
add_custom_command(TARGET jash POST_BUILD
    COMMAND ${CMAKE_STRIP} ${JASH_STRIP_FLAGS} -o $<TARGET_FILE_DIR:jash>/jash-release $<TARGET_FILE:jash>
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:jash> $<TARGET_FILE_DIR:jash>/jash
    COMMENT "Stripping release artifact -> jash-release; copying jash-dev → jash")

# ── Modules: build .modjash payloads for bundled modules ─────────────────────
# The sigil module is a feature-anchor; without it, JASH_SIGIL>=1 degrades to
# a warning. Build it by default so a clean clone can attest. (P0-2)
set(JASH_MODULES sigil undo ai slm-mesh)
foreach(mod ${JASH_MODULES})
    set(_mod_src ${CMAKE_SOURCE_DIR}/modules/${mod}/${mod}_module.c)
    set(_mod_out ${CMAKE_SOURCE_DIR}/modules/${mod}/${mod}.modjash)
    if(EXISTS ${_mod_src})
        set(_mod_shared_deps ${CMAKE_SOURCE_DIR}/c/module/modjash.h)
        if(mod STREQUAL "sigil")
            file(GLOB _sigil_native_deps CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/modules/sigil/*.inc)
            list(APPEND _mod_shared_deps ${_sigil_native_deps}
                ${CMAKE_SOURCE_DIR}/c/core/repo_identity.h
                ${CMAKE_SOURCE_DIR}/c/module/sigil_native.h
                ${CMAKE_SOURCE_DIR}/c/builtins/semantic_builtin.h
                ${CMAKE_SOURCE_DIR}/c/builtins/semantic_value.h
                ${CMAKE_SOURCE_DIR}/c/builtins/value_output.h
                ${CMAKE_SOURCE_DIR}/c/core/data_value.h
                ${CMAKE_SOURCE_DIR}/c/core/data_wire.h
                ${CMAKE_SOURCE_DIR}/c/core/data_emit.h
                ${CMAKE_SOURCE_DIR}/c/core/data_json.h
                ${CMAKE_SOURCE_DIR}/libjef/jef_verify.h)
        endif()
        add_custom_command(OUTPUT ${_mod_out}
            COMMAND ${CMAKE_COMMAND} -E env bash
                ${CMAKE_SOURCE_DIR}/modules/build_module.sh
                ${CMAKE_SOURCE_DIR}/modules/${mod}
            DEPENDS ${_mod_src}
                    ${_mod_shared_deps}
                    ${CMAKE_SOURCE_DIR}/modules/build_module.sh
            COMMENT "Building ${mod}.modjash")
        list(APPEND JASH_MODULE_ARTIFACTS ${_mod_out})
    endif()
endforeach()
if(JASH_MODULE_ARTIFACTS)
    add_custom_target(modules ALL DEPENDS ${JASH_MODULE_ARTIFACTS})
endif()

# Install
install(PROGRAMS $<TARGET_FILE_DIR:jash>/jash-release DESTINATION bin RENAME jash)
