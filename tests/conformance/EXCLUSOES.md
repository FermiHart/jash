# EXCLUSÕES — o que ficou de fora do corpus de conformidade, e por quê

Data: 2026-07-27 · corpus: `tests/conformance/corpus.txt` · oráculo: `/bin/bash` 3.2.57

## A regra

Um caso só pode ficar de fora se for **legitimamente incomparável** — se a
resposta "certa" depender de algo que não é o shell (relógio, PID, entropia,
identidade do binário), ou se o oráculo não souber julgar.

**Nenhum caso foi removido porque o jash erra nele.** Um caso que o jash erra
é o dado mais valioso do corpus e permanece contado como falha. As 119
divergências medidas em 2026-07-27 continuam todas dentro do denominador.

Se algum dia alguém apagar um caso daqui para "subir o número", este arquivo é
a prova de que isso é falsificação da medida — exatamente o defeito que o
projeto inteiro persegue.

---

## 1. Não-determinismo (a resposta muda a cada execução)

| Construção | Justificativa |
|---|---|
| `$$`, `$PPID`, `$!` | PID difere entre os dois processos por definição |
| `$RANDOM` | entropia |
| `$SECONDS`, `date`, `time`, `times` (valores) | relógio |
| `mktemp`, `/dev/urandom` | entropia |

`times` e `umask` **estão** no corpus, mas só o `rc` é comparado
(`times > /dev/null; echo $?`), o que é determinístico.

## 2. Identidade do shell (o jash nunca prometeu se passar por bash)

| Construção | Justificativa |
|---|---|
| `$0` | é o caminho do binário; são binários diferentes |
| `$BASH_VERSION`, `$BASH`, `$BASH_SOURCE` | jash não é bash e não deve mentir |
| `$SHELL` | vem do ambiente, não do shell |
| `$-` | string de flags de opção; os conjuntos de opções são diferentes por design |
| `$LINENO`, `$BASH_LINENO` | numeração interna do parser |

## 3. O oráculo disponível é bash 3.2 e não sabe julgar (LACUNA DE COBERTURA)

macOS só traz bash 3.2.57 (última versão sob GPLv2). As construções abaixo são
**erro de sintaxe no oráculo**, então compará-las mediria "os dois falham?", não
conformidade:

`${v^^}` · `${v,,}` · `${v:offset:-len}` (comprimento negativo) ·
`declare -A` / arrays associativos · `mapfile` / `readarray` · `&>>` · `|&` ·
`coproc` · `${!prefixo@}` · `printf '%(fmt)T'` · `local -n` (nameref)

**Isto não é um pass. É um buraco.** São ~10 famílias de expansão que o corpus
não mede. Instalar um bash 5 e reexecutar é o próximo passo óbvio, e pode
derrubar o número global. Enquanto isso não acontecer, o 89,4% deve ser lido
como "89,4% do subconjunto que o bash 3.2 consegue arbitrar".

## 4. Exige terminal de controle (o corpus roda com `-c` e stdin em /dev/null)

`select` · job control (`fg`, `bg`, `%1`, `set -m`) · expansão de histórico
(`!!`, `!$`) · readline/edição de linha · `trap ... WINCH`

O jash tem uma persona de surface para readline (INV-065) que é testada em
outro lugar; não é conformidade de linguagem.

## 5. Builtins exclusivos do jash (não há contra-parte para comparar)

`sigil` · `tx` · `undo` · `timeline` · `speculate` · `rewind` · `checkpoint` ·
`revert` · `trash` · `context` · `brain` · `goal` · `kv` · `recall` · `digest` ·
`state` · `json` · `contract` · `expect` · `cost` · `llm-invoice` · `abbr` ·
`theme` · `statusbar` · `juxconfig` · `osif` · `module`

Não é uma questão de conformidade: são a proposta de valor do jash. Medi-los
exige outro instrumento (contrato próprio), não o bash.

## 6. Ferramentas do jash que sombreiam o coreutils (escopo de outro arquivo)

`ls` `cp` `mv` `rm` `touch` `grep` `cat` `wc` `sed` `awk` `xargs` `du` `df` `ps`
são **builtins** no jash. Este corpus mede a **linguagem**, não as ferramentas —
`tests/conformance/run_conformance.sh` (underscore) já mede as ferramentas
contra o sistema.

Por isso os casos que só precisam de um consumidor neutro de stdin chamam
`/bin/cat`, `/usr/bin/wc` e `/usr/bin/grep` **por caminho absoluto**: assim o
que está sob medição é o pipe/heredoc/redirecionamento, e não a implementação
do builtin. Onde a distinção não importa (`echo`, `printf`, `test`, `[`) o
builtin é usado normalmente e é medido — são builtins de linguagem.

---

## Normalizações do harness (não são exclusões — mudam as DUAS execuções)

Estas afetam bash e jash igualmente, e existem para que a comparação isole o
shell. Uma delas mexeu no placar e por isso está declarada.

1. **`env -i` + `PATH=/usr/bin:/bin:/usr/sbin:/sbin`, `LC_ALL=C`, `LANG=C`,
   `TERM=dumb`, `HOME=<sandbox>/home`.** Sem isso, o ambiente do
   desenvolvedor entra no resultado.

2. **`JASH_SIGIL=0`.** Sem isso o jash emite em stderr
   `sigil: JASH_SIGIL=1 set but sigil.modjash not found`. É ruído de
   configuração, não comportamento de linguagem — equivalente a rodar bash com
   `--norc`. Afeta só stderr.

3. **O sandbox é resolvido para o caminho FÍSICO (`pwd -P`).**
   **Isto mudou o placar em +1 caso e por isso está declarado aqui.**
   No macOS `/tmp` e `$TMPDIR` são symlinks para `/private/...` e `TMPDIR`
   ainda termina em `/`. Sem a normalização, `cd sub; cd -` divergia
   (`/var/folders/...//T/...` no bash contra `/private/var/folders/...` no
   jash) por causa do symlink, não por causa do shell: o bash reporta o
   caminho lógico que recebeu e o jash aplica `realpath()`. Com o sandbox já
   físico, os dois concordam e o caso **passa**.
   Fica registrado o achado subjacente: **o jash resolve o caminho físico onde
   o bash preserva o lógico** — visível em `cd -`/`$OLDPWD` sempre que o
   diretório atravessa um symlink. É uma divergência real, só que dependente
   do ambiente, e o corpus não é o lugar de asseverá-la.

4. **stderr só é comparado com `CHECK_STDERR=1`** (padrão: desligado).
   O texto de diagnóstico não é obrigação de conformidade — o jash inclusive
   melhora nele de propósito (`Did you mean 'rustc'?`). O corpus afirma
   **stdout + status de saída**, que é o que compõe.
   Consequência honesta: **isto enfraquece a medida.** Um caso em que o jash
   acerta stdout e rc mas escreve um diagnóstico errado conta como PASS.
   Rodar `CHECK_STDERR=1` uma vez e triar o resultado é trabalho pendente.

---

## Casos que o jash erra e que ficaram no corpus DE PROPÓSITO

Registro explícito, para que ninguém os confunda com candidatos a exclusão:

- **`${v:?msg}` e `set -u` com variável não definida** (3 casos) — o bash 3.2
  sai com **127**, o jash com **1**. O bash 5 sai com **1**, ou seja, aqui o
  jash concorda com o bash moderno e o **oráculo é que é o desviante**. Os dois
  shells concordam no que importa (o shell aborta, stdout vazio); só o número
  difere. Mantidos como FAIL porque o oráculo declarado é o bash 3.2 — mas o
  leitor deve saber que **3 dos 119 FAILs sumiriam com um bash 5**.
- **`alias e=echo; e aliased`** — o bash não expande alias em shell
  não-interativo (rc 127); o jash expande (rc 0). O jash é mais útil, o bash é
  a especificação. Conta como divergência.
- **`printf '%q'`** — bash 3.2 dá `a\ b`, jash dá `'a b'`. Ambas são citações
  válidas e o bash 5 dá `'a b'`. Outra em que o oráculo é o desviante.
- **`$"..."`, `$[...]`, `shopt -s nullglob|dotglob`** — o jash não implementa;
  são features reais do bash e a ausência é uma lacuna real.
- **Erros de sintaxe que o jash aceita** (`;echo x`, `echo )`, `echo ${}`,
  `echo "a` sem fechar) — o bash recusa com rc 2, o jash executa. Ser
  permissivo com entrada malformada é uma divergência, não uma feature.
