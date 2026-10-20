# BearJash — Makefile (CMake + Ninja wrapper)
#
# Usage:
#   make              build jash (Release)
#   make debug        build jash (Debug)
#   make run          build and run
#   make clean        clean build artifacts

BUILD_DIR  := build
BUILD_TYPE ?= Release
CMAKE      := cmake
NINJA      := ninja
PYTHON     ?= python3
AGENT_BUILD_DIR ?= build-agent-soak
AGENT_RUNTIME_NAME ?= jash-agentd
MUSL_CC    ?= $(shell command -v musl-gcc 2>/dev/null || command -v x86_64-linux-musl-gcc 2>/dev/null)
SOAK_BUILD_ID := $(shell git rev-parse --short HEAD)$(shell test -z "$$(git status --porcelain)" || printf '+dirty')

# JINV-001 — o build tem que poder FALHAR.
# Sem pipefail, `ninja ... | tail -1` devolve o status do `tail` (que sempre
# sucede), então um build QUEBRADO saía com 0, o `@echo BUILD` rodava assim
# mesmo, e o binário ANTERIOR continuava no disco. Efeito colateral grave:
# todo "passou nos testes" ficava infalsificável — os testes rodavam contra um
# binário velho sem ninguém perceber. Prova do mecanismo: `false | tail -1`
# sai 0. Gate: tests/test_jinv001_build_honesty.sh
#
# NÃO use `.SHELLFLAGS := -o pipefail -c` aqui: o macOS traz GNU Make 3.81
# (2006) e `.SHELLFLAGS` só existe a partir do 3.82 — seria IGNORADO em
# silêncio, ou seja, exatamente a mesma classe de bug que este invariante
# existe para matar (um fix que parece aplicado e não está). A correção
# portável é não usar pipe: redirecionar para log e propagar o status.

.PHONY: all build run clean debug agent-soak

all: build

# JINV-002 — os dois caminhos de build tem que coexistir.
# O CLAUDE.md documenta `cmake -B build` (gerador default = Unix Makefiles) +
# `cmake --build`. Este Makefile forcava `-G Ninja`, entao usar um POLUIA o
# outro: o cmake abortava com "generator Ninja does not match Unix Makefiles" e
# o `2>/dev/null` escondia a mensagem. Correcao: so escolher gerador quando NAO
# existe cache (build novo), e construir via `cmake --build`, que funciona com
# qualquer gerador. Gate: tests/test_jinv001_build_honesty.sh
GEN := $(shell test -f $(BUILD_DIR)/CMakeCache.txt || { command -v ninja >/dev/null 2>&1 && echo "-G Ninja"; })

# Configure + build
# NB: `>/dev/null` e NAO `2>/dev/null` — silencia o ruido do cmake em sucesso,
# mas deixa o erro VISIVEL quando ele falha. JINV-001.
build:
	@$(CMAKE) $(GEN) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) >/dev/null
	@$(CMAKE) --build $(BUILD_DIR) --target jash > $(BUILD_DIR)/.build.log 2>&1 \
	  || { echo "  BUILD   FALHOU — últimas linhas:"; tail -25 $(BUILD_DIR)/.build.log; exit 1; }
	@tail -1 $(BUILD_DIR)/.build.log
	@echo "  BUILD   $(BUILD_DIR)/jash"

debug:
	@$(MAKE) BUILD_TYPE=Debug BUILD_DIR=build-debug

# Explicit phase gate: static producer + independent verifier, 1,000 signed
# requests. Deliberately not part of the daily unit suite.
agent-soak:
	@test -n "$(MUSL_CC)" || { echo "agent-soak requires a musl compiler"; exit 1; }
	@$(CMAKE) -S . -B $(AGENT_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
	  -DJASH_SOURCE_REVISION= \
	  -DCMAKE_C_COMPILER="$(MUSL_CC)" -DJASH_STATIC_AGENT_RUNTIME=ON \
	  -DJASH_AGENT_BINARY_NAME=$(AGENT_RUNTIME_NAME) >/dev/null
	@$(CMAKE) --build $(AGENT_BUILD_DIR) \
	  --target $(AGENT_RUNTIME_NAME) jef_verify -j2
	@$(PYTHON) tests/agent_runtime_soak.py \
	  $(AGENT_BUILD_DIR)/$(AGENT_RUNTIME_NAME) $(AGENT_BUILD_DIR)/jef_verify --requests 1000 \
	  --expected-build "$(SOAK_BUILD_ID)" \
	  --report $(AGENT_BUILD_DIR)/soak-report.json

# Run
# JINV-001/002: mesma disciplina do alvo `build` — sem gerador fixo, e o build
# tem que falhar alto. Antes daqui, `run` engolia TODO erro (>/dev/null 2>&1
# nas duas linhas) e executava o binario VELHO como se nada tivesse acontecido.
run: build
	@clear
	@$(BUILD_DIR)/jash

# Clean
clean:
	rm -rf build build-debug build-cmake
