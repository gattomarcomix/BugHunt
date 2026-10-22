# Report dei nuovi test (2026-10-11)

I test sono in `new_tests/` (id 880–907, formato della suite: `NNN_nome.c` + `NNN_nome.expect`, fine riga CRLF). Gli output attesi sono generati con gcc `-O2`.
`new_tests/open/` contiene test che **falliscono** oggi in VM e documentano bug aperti: non vanno nella suite finché il bug non è corretto.

## Installazione

```
cp new_tests/*.c new_tests/*.expect babyc/src/tests/
python3 babyc/tools/run_suite.py babyc/src                                   # albero
BABYC_EXEC=vm python3 babyc/tools/run_suite.py babyc/src                     # VM
BABYC_EXEC=vm BABYC_BYTECODE_VM=1 python3 babyc/tools/run_suite.py babyc/src # bytecode
bash tools/asan_smoke.sh babyc/src new_tests                                 # ASan/UBSan, 3 modalità
```
Con `EXEC=vm` il motore scrive un avviso su stderr che il runner fonde in stdout: filtrare le righe `[babyc ...]` (come fa `tools/asan_smoke.sh`).

## Elenco

| id | cosa verifica | tipo |
|---|---|---|
| 880 `vm_wide_const_ops` | `%`, `/`, `+` con costanti > INT_MAX in funzioni VM chiamate da un `main` VM | regressione (bug chiuso) |
| 881 `vm_lcg_array_fill` | `a[i] = rnd()` con LCG `% 2147483648` | regressione |
| 882 `vm_printf_call_arg` | `printf("%d", rnd())` in `main` VM | regressione, **aperto in bytecode** |
| 883 `vm_nested_loop_call_if` | cicli annidati + chiamata + `if` (output perso) | regressione (bug chiuso) |
| 884 `vm_param_wide_add_sub` | parametro ± costante > INT_MAX | regressione (fix già nello snapshot) |
| 890–895 `real_i01…i06` | versioni ridotte di collatz, nqueens, primi, gcd, hash unsigned, tak | correttezza di codice verosimile |
| 896–907 `real_a01…a12` | versioni ridotte di sieve, qsort, heapsort, matmul, LCS, knapsack, CRC32, testo, life, bsearch, BFS, fattoriale | correttezza di codice verosimile |
| open/930 `vm_unsigned_char_elem_printf` | `unsigned char` letto da array/puntatore e stampato con `%u` | **bug aperto** (VM estende con segno) |

(La numerazione 780–807 usata in precedenza è stata spostata di 100 per non collidere con i 782–791 dello snapshot.)

## Esito

| gruppo | albero | VM | bytecode |
|---|---|---|---|
| 23 test in `new_tests/` (w29) | 23/23 | 23/23 | 22/23 (882) |
| 23 test in `new_tests/` (w29 + fix) | 23/23 | 23/23 | 22/23 (882) |
| `open/930` (w29 e w29 + fix) | OK | **fallisce** | **fallisce** |
| ASan/UBSan su 23 test (w29) | 0 errori | 0 errori | **6 errori** (881, 882, 897, 898, 903, 906) |
| ASan/UBSan su 23 test (w29 + fix) | 0 | 0 | **0** |

Il fix 2 non cambia l'esito funzionale dei test, ma rimuove l'errore di memoria che emergeva solo sotto ASan: per questo `asan_smoke.sh` va nel gate.

## Valore dei test

- 880–884 e 881/882 coprono le classi di bug già trovate (costanti larghe, output perso, chiamate come argomenti di `printf`).
- 890–907 sono programmi "da codice vero" ridotti (0,03–0,4 s in albero), utili come rete di sicurezza della VM; in bytecode sono i test che hanno esposto l'overflow del Fix 2.
- `open/930` isola in 12 righe il difetto di estensione di segno dietro `793`.

## Riproduttori (`repro/`)

- `repro_vm_unsigned_char_elem.c` (+ `.expected`): atteso `203 203 204 203`; in VM stampa `4294967243 4294967243 4294967244 -53`.
- `025_quicksort.c` (+ `.expect`): in VM `IR call stack overflow` alla prima `swap` perché `partition(0,8)` restituisce 4 invece di 8.
- `882_vm_printf_call_arg.c` (+ `.expect`): in bytecode nessun output (atteso `64 6 68`).
