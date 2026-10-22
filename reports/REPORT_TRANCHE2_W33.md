# Tranche 2: valutazione di W33 e correzione degli overflow nelle variadiche

Ambiente: sandbox a 1 core, gcc 13 -O2, VM lineare senza kernel, confronti A/B a run alternati. Ho ricostruito W30 e W32 dagli zip per i confronti.

## 1. Area del lavoro svolto in W33
Lavoro ampio su: variadiche (`va_start/va_arg/va_copy`, `scanf` tipizzato, ABI delle chiamate), tipi canonici (fasi 5 e 6), chiusura della modalità `STRICT` della VM lineare, contesto lineare per BabyPOSIX, rinumerazione dei test (2237 file in `tests/`, 1092 casi). I file più toccati: `ir_lin_emit.c` (2666 righe), `ir_lin_compile.c` (1322), `ir_compile.c` (1043), `ir_lin_exec.c` (812), `ir_mem.c` (775).

## 2. Risultati

| controllo | W32 | W33 |
|---|---|---|
| build GCC 13 | 0 avvisi | 0 avvisi |
| suite albero | 960/2 | 1085/3 |
| suite VM lineare | 958/4 | 1085/3 |
| suite bytecode compatto | 956/6 | **1056/32** |
| i miei 23 test, albero / VM / bytecode | 23/23/23 | 23/23/23 |

Fallimenti in albero e VM: `1066_args_canonical_types` (difetto del runner: serve passargli argomenti, vedi fix), `bytecode-vm-regression` (45 gruppi su 53) e `sparse-local-snapshot-regression` ("compact path not verified; attempts=1, runs=0"). Il bytecode ha 29 fallimenti in più, quasi tutti su struct di array, `const` e `qsort`.

Prestazioni (VM lineare senza kernel, A/B alternato):
- **W33/W32: ×0,53** in media (recuperata la maggior parte della perdita: `tak` ×4,2 più veloce, `qsort` ×6,5, `heapsort` ×3, `gcd` ×2,5).
- **W33/W30: ×1,40** (interi ×1,71, array ×1,20): `tak` ×2,6, `gcd` ×2,3, `hash` ×2,15, `qsort` ×1,8; `bfs` è invece più veloce (×0,65).

## 3. Problema trovato: overflow di memoria nelle variadiche
ASan/UBSan sui test con id ≥ 900 (185 test): **31 errori in albero, 33 in VM, 32 in bytecode**, tutti `heap-buffer-overflow` (READ di 216 byte su un blocco da 176) in `IrEvalExprTree` (`ir_eval.c:1832`, `memcpy` dell'assegnazione di struct locale).
Causa: `va_list` è una struct opaca di `sizeof(FILE)` byte (216 su glibc, `cstdlib/stdio.c`), ma i cursori variadici sono allocati con `sizeof(struct BabyCVaCursor)` = 176 byte (`cstdlib_ext/stdarg.c`). `va_copy(dst, src)` si espande in un'assegnazione di struct che copia 216 byte da un blocco di 176: lettura di 40 byte fuori limite in ogni test variadico.

## 4. Correzione (`fix/fix_01_va_list_cursor_alloc_and_runner.patch`)
1. `stdarg.c`: macro `BABYC_VA_CURSOR_ALLOC` = `max(sizeof(struct BabyCVaCursor), sizeof(FILE))` usata nelle tre allocazioni dei cursori (creazione e due percorsi di `va_copy`). Vale su tutte le piattaforme (il valore di `sizeof(FILE)` cambia con MSVC e con Win32).
2. `tools/run_suite.py`: i test `1066_args_*` e `1068_args_*` ricevono gli argomenti `- arg1 arg2 arg3 arg4`, come `032_args` e `078_args_double_ptr`. Con gli argomenti `1066` passa.

Verifica:

| controllo | W33 | W33 + fix |
|---|---|---|
| ASan, test ≥ 900, albero / VM / bytecode | 31 / 33 / 32 | **1 / 3 / 2** |
| suite albero / VM | 1085/3, 1085/3 | 1085/3, 1085/3 (stessi fallimenti) |

La patch si applica con `patch -p1` dalla radice `babyc/` (provata con `--dry-run`); dopo averla applicata serve una ricompilazione completa (`make -B`), perché il Makefile non traccia gli header. `tools/asan_smoke.sh` riesegue il controllo ASan.

## 5. Ancora aperto (prossima tranche)
- ASan rimasto: `1068_args_unary_deref` in albero (`IrLoadPtrElem`, `ir_mem.c:287`, `READ of size 4` a 0 byte oltre un blocco da 27: probabile lettura a 4 byte di un pointee `char`); in VM `909`/`911` (`vm_multidim_struct_oob_*`) e `991_ptr_array_elem_typed` in `IrExecLinLocalsRegsBody` (`ir_lin_exec.c`); in bytecode `947_qsort_bsearch` (SEGV) e `991`.
- UBSan: accessi disallineati (`expression.c:550`, `ir_ops.c:412`, `ir_mem.c:300`) e indice −1 su `char[128]` in `ir_lin_emit.c:6672`.
- Bytecode: 29 test in più falliti, `bytecode-vm-regression` 45/53 e `sparse-local-snapshot` con `runs=0`: la via compatta non esegue il caso strutturale. Il tool `test_sparse_local_snapshot.py` prende il binario come primo argomento posizionale, non `--babyc`.
- Prestazioni: restano ×1,4 sopra W30 sulla VM lineare (chiamate annidate di `tak`/`gcd`).
