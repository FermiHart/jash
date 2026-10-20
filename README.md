<p align="center">
  <img src="media/jash-hero-banner.svg" alt="JASH — 0.1α Subsumption · The Sovereign Agent Exocortex & Verifiable Shell" width="100%">
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Sovereign_Public_License_1.0-14b8a6?style=for-the-badge" alt="Sovereign Public License 1.0"></a>
  <a href="CODEMAP.md"><img src="https://img.shields.io/badge/Language-Pure_C11_(Zero_Deps)-38bdf8?style=for-the-badge" alt="Pure C11"></a>
  <a href="CODEMAP.md#2-builtins"><img src="https://img.shields.io/badge/Builtins-171_In--Process-a855f7?style=for-the-badge" alt="171 Builtins"></a>
  <a href="SPEC.md"><img src="https://img.shields.io/badge/Attestation-BLAKE3_%2B_Ed25519%2FDSSE-fbbf24?style=for-the-badge" alt="BLAKE3 + Ed25519"></a>
  <a href="tests/test_suite.sh"><img src="https://img.shields.io/badge/Gates-281%2F281_%C2%B7_236%2F236_A--Z-22c55e?style=for-the-badge" alt="281/281 Gates"></a>
</p>

<h3 align="center">
  <b>Stop letting AI agents run blind on a 1989 shell.</b><br>
  <sub>Give them predictive intelligence, a typed JSON data plane, time-travel undo, and mathematical proof.</sub>
</h3>

<p align="center">
  🎬 <a href="media/jash-social-launch.mp4"><b>Watch the 72s HD Launch Video (with Audio)</b></a> &nbsp;·&nbsp;
  🗺️ <a href="CODEMAP.md"><b>Explore the Living CODEMAP</b></a> &nbsp;·&nbsp;
  🧠 <a href="CLAUDE.md"><b>Agent Exocortex Guide (CLAUDE.md)</b></a> &nbsp;·&nbsp;
  🔐 <a href="SPEC.md"><b>Cryptographic Spec (libjef)</b></a> &nbsp;·&nbsp;
  🔌 <a href="integrations/claude-code/"><b>Claude Code &amp; MCP Kit</b></a>
</p>

---

## 🎥 Watch Jash in Action (72-Second Launch Showcase)

> **Click the showcase below** to open the full **72-second HD video with stereo soundtrack** ([`media/jash-social-launch.mp4`](media/jash-social-launch.mp4)), demonstrating all 8 disruptive subsystems + the Frontier LLM Testimonials live:

<p align="center">
  <a href="media/jash-social-launch.mp4">
    <img src="media/jash-showcase.gif" alt="Watch the 72-second JASH Launch Video" width="100%">
  </a>
</p>

| Timestamp | Scene | Subsystem Demonstrated |
| :---: | :--- | :--- |
| `00:00 – 00:11` | **The 1989 vs. 2026 Crisis** | Why running autonomous AI agents on legacy `bash` causes data loss, token floods & hallucinations |
| `00:11 – 00:17` | **01 / 08 · Smart Predictive UX** | Frecency + context-aware inline **Ghost-Text Autocomplete** (`Tab` accept), live syntax highlighting & `?` AI |
| `00:17 – 00:23` | **02 / 08 · Native JSON Data Plane** | **171 in-process builtins** (`ls --json`, `stat --json`, `ls --data` JTV1 streams) + `JASH_AGENT=1` envelope |
| `00:23 – 00:29` | **03 / 08 · Project Digest & AST Graph** | Deterministic BLAKE3 CAS briefing (`digest`) + built-in C/C++ AST parser (`graph build`/`stats`, **-92.4% tokens**) |
| `00:29 – 00:35` | **04 / 08 · Blast-Radius & Time-Travel** | `explain` previews destructive/network steps without running; `rewind auto` reverses `rm` in 1ms |
| `00:35 – 00:41` | **05 / 08 · Context Governor & Memory** | Salience-ranked auto-capping (`JASH_LLM_BUDGET`), content-inode `memo` cache & `recall` session queries |
| `00:41 – 00:47` | **06 / 08 · Sovereign Attestation** | Append-only BLAKE3 ratchet chain (`sigil`) + Ed25519/DSSE proof verified with **pubkey only** (`jef_verify`) |
| `00:47 – 00:53` | **07 / 08 · OSIF 2.0 Portability** | L0 syscall + L1 typed OS provider contract (`osif check` Tier A 8/8) across **8 kernels** + `osif port <newos>` |
| `00:53 – 00:59` | **08 / 08 · 100% CI-Gated Parity** | `236/236` A-Z POSIX/Bash checks, `281/281` master gates, ASan+UBSan clean, Claude Code & MCP drop-in |
| `00:59 – 01:12` | **Frontier LLM Acclaim & Finale** | Testimonials from **Claude Code**, **GPT-6.6**, **Gemini 4 Argon**, and **GLM-5.3** on why Jash transformed their loops |

---

## 🌟 Co-Designed with the World's Frontier LLMs

> **How Jash Was Built:** Instead of guessing what AI agents need from first principles, **F E R M I ∞ H A R T** interviewed the frontier models while they operated inside real autonomous engineering harnesses — **Claude Code**, **GPT-6.6**, **Gemini 4 Argon**, **GLM-5.3**, **Qwen-3 Max**, and **DeepSeek-R2** — asking each model:
>
> *"What costs you the most tokens, causes your worst failures, or would make your life radically better if your shell did it natively?"*
>
> Every disruptive primitive in Jash is the direct answer to those sessions:

<p align="center">
  <img src="media/jash-llm-testimonials.svg" alt="Frontier LLM Testimonials — Claude Code, GPT-6.6, Gemini 4 Argon, GLM-5.3, Qwen-3 Max, DeepSeek-R2" width="100%">
</p>

---

## ⚡ Why Jash Exists

`bash` was designed in **1989** for humans typing at 40 words per minute on serial teletypes. Today, autonomous AI agents fire **1,000 commands per minute** through it — **completely blind**:

1. **Irreversible Destruction:** One hallucinated `rm -rf`, `mv`, or `sed -i` wipes out hours of uncommitted engineering work with zero rollback path.
2. **200K Context Window Floods:** Unbounded `cat`, `find`, `grep`, or build logs dump tens of megabytes of raw text into LLM context windows, burning API budgets and evicting reasoning history.
3. **Fragile Regex Scraping:** Agents waste thousands of tokens writing brittle `awk`/`sed` pipelines to parse human-formatted tables (`ls -la`, `ps aux`, `df -h`) that differ between GNU and BSD.
4. **Zero Cryptographic Accountability:** Plaintext logs can be forged, truncated, or bypassed. When an agent mutates a production workspace, you have **no mathematical proof** of what actually ran.

**Jash replaces the dumb pipe with a Sovereign Agent Exocortex — while remaining a blazing-fast, intelligent interactive shell for humans.**

| Capability | `bash` / `zsh` | `fish` / `nushell` | **JASH (`0.1α Subsumption`)** |
| :--- | :---: | :---: | :---: |
| **POSIX / Bash Script Compatibility** | ✅ | ❌ | ✅ **236/236 CI-Gated A-Z Checks** |
| **Smart Predictive Ghost-Text UX (`jux` / `predict` / `?`)** | ❌ | ✅ | ✅ **Frecency + Context + Inline AI** |
| **Time-Travel Undo (`rewind` / `undo` / `tx`)** | ❌ | ❌ | ✅ **Native & Automatic on `rm`/`mv`/`cp`** |
| **Cryptographic Proof (`sigil` BLAKE3 + Ed25519/DSSE)** | ❌ | ❌ | ✅ **Native + Standalone `libjef`** |
| **Context Window Protection (`recall` + Salience Governor)** | ❌ | ❌ | ✅ **Auto-Capped + Overflow Spill** |
| **171 In-Process Builtins with Schema'd `--json` & `--data`** | ❌ | Partial | ✅ **0.18ms Zero-Fork + 31 JSON Schemas** |
| **Project Briefing & C/C++ Living AST Graph (`digest` / `graph`)** | ❌ | ❌ | ✅ **Built-in C11 Engine (-92% Tokens)** |
| **Blast-Radius Preview & Contracts (`explain` / `expect`)** | ❌ | ❌ | ✅ **Native Pre/Post-Conditions** |
| **OSIF 2.0 Portability (`syscall` L0 + `osinfo` L1)** | ❌ | ❌ | ✅ **8 Kernels · 2-File Port Scaffolder** |
| **Runtime Footprint & Dependencies** | Legacy C | Heavy Rust / C++ | **100% Pure C11 (~Zero Deps)** |

---

## 🏛️ Sovereign Architecture

<p align="center">
  <img src="media/jash-architecture.svg" alt="JASH Sovereign Architecture Diagram" width="100%">
</p>

---

## 🔬 Deep Dive: 8 Subsystems No Other Shell Has

### 1. Smart Predictive Human UX & Inline AI (`jux`, `predict`, `abbr`, `?`)
Jash is engineered for both autonomous agents and sovereign human engineers. Its interactive readline engine (`c/readline/` + `c/completion/smart_suggestions.c` + `c/core/predict.c`) learns your command sequences per working directory:
- **Ghost-Text Autosuggestions:** Real-time inline completions ranked by frecency, exit-code success, and `cwd` context — accept with `Tab` or `→`.
- **Live Syntax & Balance Highlighting:** Validates commands, quotes, brackets, and operators on every keystroke before you press `Enter`.
- **Native `?` AI Mode:** Query, auto-execute, or run bounded compile-repair loops (`? on`, `? auto`, `? fix`) with hash-guarded file edits and cgroup-v2 resource budgets.

---

### 2. Native JSON Data Plane & Sealed Binary Streams (`--json`, `--data`, `JASH_AGENT=1`)
**171 builtins** execute directly inside the Jash process (zero `fork()`/`execve()` overhead) and speak structured JSON validated against 31 Draft 2020-12 schemas in [`schemas/`](schemas/):

```bash
$ jash -c 'ls --json c/core | head -n 3'
{"name":"adopt.c","path":"c/core/adopt.c","type":"file","size":13603,"mode":"-rw-rw-r--","mtime":1792435803}
{"name":"adopt.h","path":"c/core/adopt.h","type":"file","size":2936,"mode":"-rw-rw-r--","mtime":1792435803}
{"name":"agent_cancel.c","path":"c/core/agent_cancel.c","type":"file","size":3251,"mode":"-rw-rw-r--","mtime":1792435803}

$ jash -c 'stat --json LICENSE'
[{"name":"LICENSE","type":"file","size":1894,"mode":"0664","perms":"rw-rw-r--","nlink":1,"uid":1000,"gid":1000}]

$ JASH_AGENT=1 jash -c 'echo sovereign'
sovereign
# stderr -> {"rc":0,"ms":0,"tl":0,"cwd":"/home/alf/OS/Jash","cpu_ms":0,"maxrss_kb":5120,"jash":"0.1","git":"961dc80","modabi":1}
```

Need zero-escaping invocation? Pass literal `argv` arrays via `--exec-json`:
```bash
$ jash --exec-json '{"argv":["grep","SPDX-License-Identifier","LICENSE"]}'
```

---

### 3. Instant Project Digest & Living C11 Code Graph (`digest`, `graph`)
Instead of an agent running 40 `find`/`grep`/`cat` calls to orient itself in a repository, Jash ships a multi-threaded **BLAKE3 Content-Addressed Store (CAS) briefing engine** (`c/core/digest_engine.c`) and a **native C/C++ AST parser & knowledge graph** (`c/graph/`):

```bash
$ jash -c 'digest --brief libjef'
# Project Digest
**project:** libjef  |  **generator:** project-v5
**tree fingerprint:** `79332849237344ac762cd8bb8166d202567ec4aab7af4b5e738a9ff0878541eb`
**source files:** 3 | **headers:** 3 | **tests:** 1 | **docs:** 1 | **total lines:** 2901

$ jash -c 'graph build libjef && graph stats'
build: 7 files (0 reused, 7 re-parsed, 0 gone, 0 skipped) — 95 nodes, 305 edges, 6 communities
nodes: 95 (func 85, struct 0, enum 0, typedef 3, module 7)
edges: 305 (calls 212, contains 88, includes 5; EXTRACTED 305, INFERRED 0)
```
- **Delta Briefings:** `digest --diff-base prior-manifest.json` emits only what changed (`jash.project-diff.v1`) in $O(\text{changed})$.
- **Savings Ledger:** `digest --ledger` records verifiable token-savings receipts across sessions.

---

### 4. Blast-Radius Preview & Time-Travel Undo (`explain`, `rewind`, `undo`, `tx`)
Look before you leap — and if an agent breaks something, reverse time instead of panicking:

```bash
$ jash -c 'explain "rm -rf build/ && curl https://api.example.com/deploy"'
═══ explain — 2 step(s), not executed ═══
  1. rm -rf build/
       → DELETE (recoverable: trash on → undo/rewind)
  2. curl https://api.example.com/deploy
       → NETWORK (may exfiltrate/fetch)
blast radius: 1 mutation(s) — 1 delete, 0 write, 1 network; 0 external/opaque
```

Every destructive builtin (`rm`, `mv`, `cp`, `sed -i`, truncating redirects) automatically drops a recovery checkpoint:
```bash
$ jash -c 'rm critical.json'
[jash] auto-checkpoint set — `rewind auto` to undo this

$ jash -c 'rewind auto && expect file critical.json && expect rc 0'
```
For multi-step atomic workflows, open a full transaction with `modules/undo`:
```bash
$ jash -c 'tx begin refactor && sed -i "s/old/new/g" src.c && tx diff && tx rollback'
```

---

### 5. Context Governor, Active Memory & Executable Contracts (`recall`, `expect`, `memo`)
- **Salience Governor (`JASH_LLM_BUDGET`):** Automatically intercepts large command outputs, ranks lines by diagnostic salience (errors, stack frames, diffs, summaries), emits only what fits the token budget, and spills the full output to `recall`.
- **Content-Inode Memoization (`memo`):** When an agent repeats an idempotent read (`cat`, `grep`, `ls`, `head`) on an unchanged file, Jash returns a 1-line attested `[jash memo]` receipt instead of re-emitting thousands of duplicate tokens.
- **Session Memory (`recall`):**
  ```bash
  $ jash -c 'recall did "cmake"'      # did I already run this? what was the exit code?
  $ jash -c 'recall failed'           # show every failing command in this session
  $ jash -c 'recall adoption'         # % of commands using Jash native differentials
  ```
- **Anti-Hallucination Contracts (`expect`):**
  ```bash
  $ jash -c 'cmake --build build --target jash && expect file build/jash && expect rc 0'
  ```

---

### 6. Cryptographically Verifiable Execution (`sigil` + Standalone `libjef`)
When `JASH_SIGIL=2` is set, every command executed by a human or AI agent is:
1. Chained into `~/.jash/chain.log` using **BLAKE3**: $h_n = \text{BLAKE3}(h_{n-1} \parallel \text{ts\_ns} \parallel \text{rc} \parallel \text{cmd})$.
2. Authenticated by a **forward-secure ratchet MAC**: $\text{tag}_n = \text{BLAKE3}_{\text{keyed}}(k_n, h_n)$, advancing $k_{n+1} = \text{BLAKE3}(k_n \parallel \texttt{"r"})$ and erasing $k_n$.
3. Signed at session exit with a sovereign **Ed25519 (RFC 8032)** keypair into `~/.jash/session.jashproof` or a standard **DSSE / in-toto** envelope (`sigil proof dsse`).

**Anyone can verify a session with zero shared secret** using [`libjef`](libjef/README.md) (`jef_verify`), a standalone C11 binary that links *only* `libjef` and zero shell code:
```bash
$ cmake --build build --target jef_verify
$ ./build/jef_verify chain ~/.jash/chain.log
$ ./build/jef_verify proof ~/.jash/session.jashproof.dsse
```

---

### 7. OSIF 2.0 — Universal Portability Across 8 Kernels (`osif`)
Jash isolates all operating-system dependencies behind **OSIF 2.0** (`c/osif/`):
- **L0 (`osif.h`):** Raw syscall ABI (`c/osif/linux/syscall.S` is hand-written freestanding System V AMD64 assembly — zero glibc wrapper dependency at L0).
- **L1 (`osinfo.h`):** 8 typed OS observation providers (`+SYSID`, `+PROC`, `+MEM`, `+MOUNTS`, `+NET`, `+UPTIME`, `+LOADAVG`, `+SPAWN`) consumed by `ps`, `df`, `uname`, and the live statusbar.

```bash
$ jash -c 'osif check'
OSIF target=linux caps=8/8  +SYSID +PROC +MEM +MOUNTS +NET +UPTIME +LOADAVG +SPAWN
  PASS  SYSID      sysid        Linux 7.0.0-34-generic x86_64
  PASS  PROC       proc_iter    467 procs visible, self=jash rss=5236K
  PASS  MEM        meminfo      31791 MB total, 13348 MB available
  PASS  MOUNTS     mounts       7 real mounts, root visible
  PASS  NET        netstat      rx=33575042675 tx=11762332561 bytes
  PASS  UPTIME     uptime       574458 seconds
  PASS  LOADAVG    loadavg      1.46 2.37 3.81
  PASS  SPAWN      spawn        echo round-trip OK (17 bytes)
Tier A — 8/8 pass · 0 fail · 0 skip · target=linux
```

Porting Jash to a new kernel takes **one command**:
```bash
$ jash -c 'osif port haiku'
# Scaffolds c/osif/haiku/{syscall.c, osinfo.c, AGENT.md} — boots immediately with graceful degradation!
```
Supported OSIF targets in [`c/osif/`](c/osif/): **`linux`**, **`macos`**, **`beyond`**, **`tina`** (TinaLinux), **`singuos`**, **`bethos`**, **`minix`**, **`netbsd`**, and **`linux01`**.

---

### 8. Guarded Agent Daemon (`jash-agentd`), Claude Code & MCP Integration
- **Claude Code Drop-In Kit ([`integrations/claude-code/`](integrations/claude-code/)):** Configures `JASH_SIGIL=2`, `JASH_LLM_BUDGET`, and operating rules so Claude Code uses Jash's native differentials (`recall`, `rewind`, `expect`, `explain`, `digest`) instead of falling back to plain `bash`.
- **Read-Only MCP Stdio Server ([`integrations/jash_mcp.py`](integrations/jash_mcp.py)):** Exposes bounded native Jash tools (`fs.list`, `fs.count`, `fs.observation.read`, `fs.source.read`) over Model Context Protocol (`2025-06-18`).
- **Source-Isolated `jash-agentd` Runtime:** Dedicated JSONL/IPC daemon with capability-gated sessions, request-ID cancellation, delegated cgroup-v2 memory/PID/CPU quotas, and signed artifact store receipts.

---

## 🚀 Build & Quickstart (60 Seconds)

```bash
# 1. Configure & build jash, jash-agentd, and the standalone libjef verifier (-Werror enforced)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jash jash-agentd jef_verify

# 2. Verify build identity & OSIF conformance
./build/jash --whoami
./build/jash -c 'osif check'

# 3. Run the POSIX/Bash A-Z compatibility battery & CODEMAP anti-rot guard
bash tests/test_az_battery.sh              # 236/236 POSIX/Bash checks
bash tests/test_codemap.sh                 # verifies CODEMAP.md is 100% in sync with source
bash tests/test_builtin_contract.sh        # verifies 171-builtin registry contract
```

---

## 🗺️ Start Here for Contributors & Agents — Read the `CODEMAP`

> **Agents & humans:** the canonical technical entry point is **[`CODEMAP.md`](CODEMAP.md)** — an auto-generated, CI-enforced map of everything Jash does with exact `file:line` pointers for every builtin, module, environment knob, and subsystem.
>
> Start with **[`CODEMAP.md`](CODEMAP.md)**, **[`SPEC.md`](SPEC.md)**, **[`FEATURES.md`](FEATURES.md)**, and **[`schemas/BUILTIN_CONTRACTS.json`](schemas/BUILTIN_CONTRACTS.json)** (`700` published structured `--json` & `--data` modes across `132` builtins).

Regenerate the map after adding a builtin, module, or `JASH_*` environment knob:
```bash
bash tools/gen_codemap.sh > CODEMAP.md
```

---

## 🛡️ License

Released under the **Sovereign Public License Version 1.0** (`SPDX-License-Identifier: LicenseRef-SPL-1.0`) — see [`LICENSE`](LICENSE).

> *"Sovereignty is a property of the artifact — an attested build and execution chain — never a restriction placed on you."*

<p align="center">
  <b>Created by F E R M I ∞ H A R T</b> (<code>&lt;contact@fermihart.com&gt;</code>)<br>
  <sub>Part of the <b>Beyond Sovereign Operating System Ecosystem</b></sub>
</p>
