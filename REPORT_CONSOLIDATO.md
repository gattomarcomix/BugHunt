# BabyC: report consolidato (valutazioni, correzioni, test)

Ambiente di tutte le misure: sandbox Linux a 1 core, gcc 13 `-O2`, Python 3.12.3. Tre modalità di esecuzione: **albero** (default), **VM lineare** (`BABYC_EXEC=vm`), **bytecode compatto** (`BABYC_EXEC=vm BABYC_BYTECODE_VM=1`). I tempi della VM sono "senza kernel" (`BABYC_NO_KERNEL=1`), con confronti A/B a run alternati.

## 1. Come applicare le correzioni

```
cd babyc                                                  # radice dello snapshot
patch -p1 < fixes/t1_w32_perf/fix_01_nested_light_and_seed_bound.patch   # solo su W32
# oppure, su W33, nell'ordine:
patch -p1 < fixes/t2_w33_va_list_runner/fix_01_va_list_cursor_alloc_and_runner.patch
patch -p1 < fixes/t3_w33_ptr_deref/fix_02_ptr_to_ptr_deref_declared_type.patch
make -C src -B -j4 babyc                                  # ricompilazione COMPLETA
cp ../new_tests/*.c ../new_tests/*.expect src/tests/      # nuovi test
```
Ricompilare sempre con `make -B`: il Makefile non traccia le dipendenze dagli header (con oggetti vecchi ho visto output vuoti e falsi errori "uninitialized variable"). Ogni patch è stata provata con `--dry-run` su una copia pulita dello snapshot indicato.

## 2. Cronologia delle valutazioni

| snapshot | risultato principale | trovato / corretto |
|---|---|---|
| primi snapshot (5–8 ott) | VM lineare in costruzione; suite in albero ok, VM con molti fallimenti | `fill` non inizializzato in `IrTryFastArrayFill`, ibridi con `ParamSlot`, `IrApplyBinOpRaw` (flag passato come puntatore, SEGV), `XXX` al posto delle stringhe, output perso, debug su stderr: tutti chiusi negli snapshot successivi; il ramo non-GNU di `LightRunBody` non compilava con MSVC (fix `switch`, integrato) |
| 9 ott "BabyCG 10 consolidata" | 18/18 benchmark corretti in VM | costanti larghe nel peephole `AddI` (parametro ± costante > INT_MAX), corretto |
| W27 (10 ott) | suite 827/0 nelle tre modalità; VM 0,83× Python | bytecode compatto più lento della VM lineare (0,68×); regressione `a01_sieve` +54% |
| W29 (11 ott) | albero 852/1, VM 846/7 | percorsi dei tool rotti dal riordino di `bench/`; `heap-buffer-overflow` in `IrVmExecProd` (152 byte su blocco da 72): corretti (integrati in W30) |
| W30 | VM 922/0, bytecode 915/7 | 7 regressioni solo nel bytecode (uninit su array, bitfield); UBSan: overflow con segno e accessi disallineati |
| W31 | albero 945/1, VM 941/5 | chiamata con locali sovrascrive i locali del chiamante (`216`, `136`); crollo VM `collatz` ×30 e `primes` ×50 |
| W32 | albero 960/2, VM 958/4 | ristrutturazione dello storage; VM ×2,1 più lenta di W30. Causa: `&& IrCallDepth == 0` toglie la fast path Light a tutte le chiamate annidate, più seeding di 128 slot a ogni chiamata: **tranche 1** |
| W33 | albero 1085/3, VM 1085/3, bytecode 1056/32 | VM ×0,53 rispetto a W32 ma ×1,40 rispetto a W30; ASan: 31 errori nelle variadiche (**tranche 2**); `**p` con `double`/`long long` sbagliato nell'albero (**tranche 3**) |

## 3. Correzioni consegnate

| tranche | snapshot | file | problema | verifica |
|---|---|---|---|---|
| 1 | W32 | `ir_fncode.h`, `ir_lin_compile.c`, `ir_lin_exec.c`, `ir_storage.c` | nessuna fast path Light per chiamate annidate; seeding su 128 slot | suite invariata (960/2, 958/4); `tak` 3,75 → 1,03 s, `gcd` 1,63 → 0,77 s; media su 12 benchmark ×0,76 |
| 2 | W33 | `cstdlib_ext/stdarg.c`, `tools/run_suite.py` | `va_list` (`sizeof(FILE)` = 216 byte) copiato da cursori di 176 byte: lettura 40 byte oltre il blocco; il runner non dava gli argomenti a `1066`/`1068` | ASan test ≥ 900: 31/33/32 → 1/3/2 (albero/VM/bytecode); suite invariata |
| 3 | W33 | `ir_mem.c` | ramo `**p` di `IrResolvePtr` carica sempre `int`: `**dp` (`double`) dava 0.00, `**lp` (`long long`) 0; lettura fuori limite | test 1200 OK in albero; ASan albero **0**; suite 1087/2 in albero e VM |
| storico | W29–W30 | `ir_vm.c`, tool, `ir_lower.c` | overflow salvataggio globali, percorsi tool, peephole costanti larghe, dispatch MSVC | già integrati negli snapshot successivi |

## 4. Test e riproduttori
- `new_tests/`: 880–884 (costanti larghe, LCG su array, `printf` con chiamata come argomento, output perso, parametro ± costante), 890–907 (versioni ridotte dei 18 benchmark), 1200 (`**p` con tipi a 8 byte). Output attesi generati con gcc `-O2`.
- `new_tests/open/930`: `unsigned char` letto da array/puntatore e stampato direttamente (chiuso dagli snapshot recenti, tenuto come regressione).
- `repro/`: riproduttori con output atteso (costante larga su parametro, quicksort VM, `882`).
- `tools/asan_smoke.sh`: ASan/UBSan nelle tre modalità su una cartella di programmi.

## 5. Stato aperto (priorità)
1. **Bytecode compatto:** 29 test in più falliti in W33 (struct di array, `const`, `qsort`), `bytecode-vm-regression` 45/53, `sparse-local-snapshot` con `runs=0` (la via compatta non esegue il caso strutturale).
2. **ASan:** VM `909`, `911`, `991` in `IrExecLinLocalsRegsBody` (`ir_lin_exec.c`); bytecode `947` (SEGV) e `991`.
3. **UBSan:** overflow di interi con segno (`ir_eval.c`, `ir_lower.c`, `ir_vm.c`), accessi disallineati (`expression.c:550`, `ir_ops.c:412`, `ir_mem.c:300`), indice −1 su `char[128]` in `ir_lin_emit.c:6672`.
4. **Prestazioni:** VM lineare ×1,40 sopra W30 (`tak` ×2,6, `gcd` ×2,3, `hash` ×2,15, `qsort` ×1,8, `heapsort` ×1,7). Il target 1,5–2× Python è lontano: a W30 la VM senza kernel era a circa 0,87× Python.
5. **Sicurezza dell'albero:** `1038`/`1039` fissano nel `.expect` che l'albero non diagnostica l'uninit dietro un parametro puntatore, mentre la VM lo fa.
6. **Fix 3:** per parametri puntatore-a-puntatore il tipo non è disponibile in quel punto e resta il ripiego su `int`.
7. **Makefile:** aggiungere `-MMD -MP` per le dipendenze dagli header.

## 6. Metodo e limiti
Python best-of-7 come riferimento comune; A/B alternati tra snapshot (il sandbox varia ±20–30% tra corse diverse); suite con `tools/run_suite.py` filtrando le righe `[babyc ...]` su stderr; ASan/UBSan con `detect_leaks=0`. Non è stato compilato con MSVC. La tranche 1 si applica solo a W32 (W33 ha migliorato in altro modo la stessa zona).
