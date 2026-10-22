#!/bin/bash
# Smoke ASan/UBSan su un insieme di programmi C, nelle tre modalita' (albero, VM lineare, bytecode compatto).
# Uso: tools/asan_smoke.sh <dir_src_babyc> <dir_con_programmi_c> [dir_build_asan]
# Esempio: tools/asan_smoke.sh babyc/src new_tests /tmp/asan_babyc
# Esce con 1 se trova almeno un errore del sanitizer.
set -u
SRC=${1:?dir src}; PROGS=${2:?dir programmi}; BUILD=${3:-/tmp/asan_babyc}
rm -rf "$BUILD"; cp -r "$SRC" "$BUILD"; cd "$BUILD" && rm -f *.o babyc
sed -i 's/-O2/-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer/' Makefile
make -j4 babyc >/dev/null 2>&1 || { echo "build ASan fallita"; exit 2; }
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=0
bad_total=0
for m in "X=1" "BABYC_EXEC=vm" "BABYC_EXEC=vm BABYC_BYTECODE_VM=1"; do
  n=0; bad=0
  for f in "$PROGS"/*.c; do
    n=$((n+1))
    out=$(cd "$PROGS" && env $m timeout 60 "$BUILD/babyc" "$f" 2>&1 >/dev/null | grep -a -E "ERROR: AddressSanitizer|runtime error" | head -1)
    if [ -n "$out" ]; then bad=$((bad+1)); echo "  $(basename "$f") [$m]: $out" | cut -c1-170; fi
  done
  echo "ASan [$m]: $n programmi, $bad con errori"
  bad_total=$((bad_total+bad))
done
[ "$bad_total" -eq 0 ] || exit 1
