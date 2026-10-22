# Tranche 3: risultati sbagliati su `**p` nel motore ad albero (W33)

Si somma alla tranche 2 (`fix_01`, va_list e runner). Ambiente: sandbox a 1 core, gcc 13.

## 1. Problema
Con l'ASan sui test recenti restava un `heap-buffer-overflow` in `1068_args_unary_deref` (ad albero): `showc(**pp)` con `char **pp` leggeva 4 byte da un letterale di 3. Isolato per bisezione dei comandi del test.
Causa: il ramo "multi-level `*p`" di `IrResolvePtr` (`ir_mem.c`, ~riga 868) carica la parola del puntatore interno ma imposta **sempre** `ElemBase = TypeInt` e `ElemSize = 4`, qualunque sia il tipo puntato.
Conseguenze, oltre alla lettura fuori limite, sul motore di default:

| espressione | gcc | albero W33 |
|---|---|---|
| `**dp` con `double **dp` | 2.50 | **0.00** |
| `**lp` con `long long **lp` | 1099511627776 | **0** |
| `***d3` con `double ***d3` | 7.50 | **0.00** |

Con `char`, `short`, `unsigned char`, `int` il risultato è corretto per caso (little-endian). La VM lineare e il bytecode erano già corretti.

## 2. Correzione (`fix/fix_02_ptr_to_ptr_deref_declared_type.patch`, solo `src/ir_mem.c`)
- `IrDeclTypeAfterDerefs`: ricava il tipo dichiarato dell'identificatore di base (locale via `LocalFromType`, globale via `IrGlobalValSoft`) dopo N dereferenze.
- Nel ramo multi-livello, la base dell'elemento caricato è ora il tipo reale (`char`..`long long`, puntatore, `double` tramite `TypeFP`), con ripiego sul vecchio `int` se il tipo non è noto (per esempio i parametri, che qui non portano il tipo).
- Test di regressione: `new_tests/1200_ptr_ptr_deref_declared_types.c` (+ `.expect`, atteso generato con gcc).

## 3. Verifica

| controllo | W33 | W33 + fix 01 + fix 02 |
|---|---|---|
| test 1200, albero / VM / bytecode | **DIFF** / OK / OK | OK / OK / OK |
| ASan, test ≥ 900 (argomenti passati ai test `args`), albero | 31 errori (1 dopo fix 01) | **0** |
| ASan, VM / bytecode | 33 / 32 (3 / 2 dopo fix 01) | 3 / 2 |
| suite albero | 1085/3 | **1087/2** |
| suite VM lineare | 1085/3 | **1087/2** |

Rimangono solo `bytecode-vm-regression` e `sparse-local-snapshot-regression` (via compatta del bytecode). Patch provate con `--dry-run` su W33 pulita; ricompilare con `make -B`.

## 4. Aperto per la tranche 4
- ASan in VM: `909_vm_multidim_struct_oob_out`, `911_vm_multidim_struct_oob_inn`, `991_ptr_array_elem_typed` in `IrExecLinLocalsRegsBody` (`ir_lin_exec.c`); in bytecode `947_qsort_bsearch` (SEGV) e `991`.
- Parametri puntatore a puntatore (`double **pp` come argomento) restano sul ripiego `int` nel ramo corretto qui, se il tipo non è disponibile.
- Bytecode: 29 test in più falliti, `bytecode-vm-regression` 45/53, `sparse-local-snapshot` con `runs=0`.
- Prestazioni: VM lineare ×1,4 sopra W30 (chiamate annidate).
