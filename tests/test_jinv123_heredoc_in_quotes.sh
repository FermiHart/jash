#!/usr/bin/env bash
# JINV-123 — hunting the real trigger of "eval: unterminated quote".
# Differential against bash; jash must match.
J=./build/jash
pass=0; fail=0
ck() { # ck <label> <script>
  local lbl="$1" src="$2" bo jo brc jrc
  # JINV-127: so stdout entra no veredito; stderr e UX (avisos que so o jash
  # emite nao podem reprovar paridade funcional). rc continua comparado.
  bo=$(bash -c "$src" 2>/dev/null); brc=$?
  jo=$($J   -c "$src" 2>/dev/null); jrc=$?
  if [ "$bo" = "$jo" ] && [ "$brc" = "$jrc" ]; then
    echo "ok   $lbl"; pass=$((pass+1))
  else
    echo "FAIL $lbl"
    echo "     bash rc=$brc [$bo]"
    echo "     jash rc=$jrc [$jo]"
    fail=$((fail+1))
  fi
}

# quotes as data (already known good, keep as regression floor)
ck "sq: quoted marker"      "echo '<<\"EOF\"'"

# multi-line quoted argument
ck "multiline sq"           "x='linha um
linha dois'
echo \"\$x\" | wc -l"

# backtick as data inside single quotes, multi-line
ck "sq backtick data"       "echo 'call \`foo()\` now'"
ck "sq backtick multiline"  "echo 'um \`foo()\` dois
tres \`bar()\` quatro'"

# the shape that actually failed: heredoc whose body carries backticks
ck "heredoc body backticks" "cat <<'EOF'
\`sigil_init()\` calls \`chain_resume()\`
EOF"

# unquoted heredoc delimiter: backticks ARE command substitution (bash parity)
ck "heredoc unquoted subst" "cat <<EOF 2>&1
\`echo hi\`
EOF"

# markdown table row with pipes inside a quoted string
ck "sq pipes table"         "echo '| a | b |'"

echo "=== JINV-123 hunt: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
