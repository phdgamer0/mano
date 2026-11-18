#!/usr/bin/env bash
# Portable smoke test: assembler, simulator and every code generator.
set -euo pipefail
MANO=${1:-./mano}
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

check() { # name expected actual
    if [ "$2" != "$3" ]; then
        printf 'FAIL %s: expected %q got %q\n' "$1" "$2" "$3"
        exit 1
    fi
    echo "ok   $1"
}

"$MANO" --version | grep -q "^mano "
echo "ok   version"
check sum '*' "$("$MANO" run examples/sum.asm 2>/dev/null)"
check mult '*' "$("$MANO" run examples/mult.asm 2>/dev/null)"
check echo 'hello' "$(printf 'hello' | "$MANO" run examples/echo.asm 2>/dev/null)"
"$MANO" asm examples/mult.asm | grep -q '^SUM ' && echo "ok   symbols"

printf 'ORG 100\nBUN NOPE\nEND\n' >"$tmp/bad.asm"
if "$MANO" asm "$tmp/bad.asm" 2>/dev/null; then echo "FAIL bad source accepted"; exit 1; fi
echo "ok   rejects undefined symbol"

for t in amd64 i386 arm64 arm32; do
    for o in 0 1 2 3; do
        "$MANO" x86 --target $t -O$o examples/mult.asm -o "$tmp/m.s" 2>/dev/null
        grep -q '_start' "$tmp/m.s"
    done
    echo "ok   codegen $t"
done
echo "all smoke tests passed"
