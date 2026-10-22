# BabyC: valutazione di w29/W30 e correzione dei bug (2026-10-11)

Ambiente: sandbox Linux a 1 core, gcc 13 `-O2`, Python 3.12.3. Suite da 876 test (853 + 23 saltati).
Le tre modalità: **albero** (default), **VM** (`BABYC_EXEC=vm`, interprete lineare), **bytecode** (`BABYC_EXEC=vm BABYC_BYTECODE_VM=1`).

## 1. Sintesi

- La baseline w29 non riproduceva "853 pass / 0 fail": con la suite dallo zip l'albero dava **852/1**, la VM **846/7**, il bytecode **845/8**.
- Una causa era di packaging: il riordino di `bench/` ha spostato i programmi e **quattro tool puntavano ancora ai vecchi percorsi**; `perf_gate.py` stampava tabelle vuote senza segnalare errori. **Corretto**.
- Una causa era di memoria: **heap-buffer-overflow in `IrVmExecProd`** (`ir_vm.c`), lettura **e scrittura** di 152 byte su un blocco di 72 per le variabili globali scalari. **Corretto**.
- Con le due correzioni: albero **853/0**, VM **847/6**, bytecode **846/7**; ASan pulito in tutte e tre le modalità su 23 programmi (prima 6 errori nel bytecode). Prestazioni invariate (±1%, 5 benchmark a run alternati).
- **Restano aperti** (con diagnosi): `025_quicksort`, `248/252/548` (uninit su membri di array di struct), `250` (colonna dell'errore), `793` (unsigned char letto con segno) e `882` in bytecode (nessun output).

## 2. Correzioni consegnate

### Fix 1: tool con percorsi obsoleti e fallimento silenzioso (`fix/fix_01_tools_paths_and_loud_failure.patch`)

| file | cambiamento |
|---|---|
| `tools/test_string_scan_fastpath.py`, `tools/bench_string_scan_fastpath.py` | `bench/string_proc{,_v2}.c` -> `bench/cases/micro/string_scan{,_bounded}.c` |
| `tools/bench_isort_fastpath.py` | `bench/isort_v2.c` -> `bench/cases/micro/isort_decoy.c` |
| `src/tools/gate_shared.py` | `serie1/sieve`, `serie1/fibc`, `list_walk`, `array_sum`, `isort` -> nuovi percorsi in `bench/cases/{classic,micro}` |
| `src/tools/perf_gate.py` | stessi percorsi; **se un programma manca stampa `MISSING: <percorso>` ed esce con codice 2** invece di saltarlo |

Verifica: `test_string_scan_fastpath.py` -> `PASS`; `perf_gate.py --runs 1` ora stampa le righe di `fibc`, `isort`, `isort_micro`, `isort_v2`, `string_proc`, `string_proc_v2`; il test `string-scan-fastpath-regression` della suite passa.

### Fix 2: overflow di memoria nel salvataggio delle globali (`fix/fix_02_ir_vm_global_save_overflow.patch`)

- Sintomo (ASan): `READ of size 152 ... 0 bytes after 72-byte region` in `IrVmExecProd` (`ir_vm.c:1309`), riproducibile con 6 dei miei test in modalità bytecode (881, 882, 897, 898, 903, 906).
- Causa: `VmProdGlobalSave[gi] = *V->Val;` (due punti) e il ripristino `*VmProdGlobal[i]->Val = VmProdGlobalSave[i];` copiano un intero `union AnyValue` (152 byte) da/verso una globale scalare che occupa 72 byte. Il salvataggio legge fuori limite; il ripristino **scrive** fuori limite e può corrompere l'heap.
- Fix: nuova `VmGlobalBytes(V)` (dimensione reale del tipo, 1..8 byte) e tre `memcpy` limitati a quella dimensione.
- Verifica: ASan/UBSan senza errori su 23 programmi in albero, VM e bytecode (`tools/asan_smoke.sh`); suite e benchmark non regrediscono.

### Come applicare

```
cd babyc
patch -p1 < fix/fix_01_tools_paths_and_loud_failure.patch
patch -p1 < fix/fix_02_ir_vm_global_save_overflow.patch
```
(le patch sono state provate con `--dry-run` su una copia pulita di w29). In `fix/files/` ci sono i sei file già corretti.

## 3. Risultati prima e dopo

| controllo | w29 | w29 + fix |
|---|---|---|
| suite, albero | 852/1 | **853/0** |
| suite, `EXEC=vm` | 846/7 | **847/6** |
| suite, VM + bytecode | 845/8 | **846/7** |
| ASan su 23 programmi, albero / VM / bytecode | 0 / 0 / **6** errori | 0 / 0 / **0** |
| 23 test nuovi, albero / VM / bytecode | 23 / 23 / 22 | 23 / 23 / 22 |
| benchmark `i01`, `i06`, `a02`, `a05`, `a12` (VM), fix/w29 | – | 1,00 / 1,00 / 0,99 / 0,99 / 1,01 |

Resta rosso in VM: 025, 248, 250, 252, 548, 793 (in bytecode anche 882).

## 4. Bug aperti con diagnosi

1. **`025_quicksort`: "IR call stack overflow" in VM.**
   - La profondità raggiunge davvero 4096: la ricorsione non termina perché in VM `partition(0,8)` restituisce **4 invece di 8** dopo `partition(0,15)` (tracciato per confronto albero/VM).
   - Il bug compare solo quando `quicksort` stessa è compilata dalla VM (ricorsione VM->VM con una chiamata che assegna il risultato a una locale); aggiungere un `printf` con un ciclo la fa uscire dalla VM e il bug sparisce. Isolato come: rimuovere la prima `swap(pivotIndex, right)` lo fa passare.
   - Ipotesi: stato della funzione chiamata non ripristinato tra due chiamate annidate; non verificata a livello di codice.
2. **`248_array_struct_member_uninit`, `252_member_array_uninit`, `548_ir_struct_array_uninit`: manca l'errore "uninitialized variable".** In VM stampa 0 invece di fermarsi. È una perdita di diagnostica di sicurezza (regressione rispetto a w27). `250`: l'errore c'è ma la colonna è 21 invece di 26.
3. **`793_unsigned32_wrap_in_loops` e riproduttore `930`:** un elemento `unsigned char` letto da array o puntatore e passato direttamente a `printf` viene esteso con segno in VM (`a[3]` stampa 4294967243 invece di 203); se prima si assegna a una variabile è corretto. Diagnosi: `IrResolvePtr` (`ir_mem.c` ~779–812) assegna `TypeChar` (con segno) agli elementi di dimensione 1 senza guardare l'unsignedness, e ci sono più di 20 punti che eseguono `IrLoadPtrElem`. Un fix puntuale in `ir_vm.c` non basta. Strada suggerita: un unico punto in `LowerRec` (compilazione) che, per un'espressione `unsigned` di dimensione 1 o 2 (indice e deref), emetta una maschera `& 0xFF` / `& 0xFFFF`.
4. **`882_vm_printf_call_arg` in bytecode:** nessun output (atteso `64 6 68`). Non è un errore di memoria dopo il Fix 2. Stessa classe dei casi di "output perso" visti in precedenza.
5. **Il vostro inventario W30** (23 discrepanze in bytecode strict) non è stato replicato.

## 5. Prossimi passi

1. Applicare le due patch e inserire `new_tests/` (e `tools/asan_smoke.sh`) nel gate di consegna: suite in tutte e tre le modalità + ASan.
2. Chiudere i bug 1–3: sono regressioni o correttezza silenziosa nella VM, e il 2 tocca la sicurezza diagnostica.
3. Far fallire sempre i tool quando non trovano i programmi (già fatto per `perf_gate.py`; estendere agli altri).
4. Non ottimizzare il dispatch del bytecode finché 882 e la copertura non sono a posto.

## 6. Limiti

Sandbox a 1 core e solo GCC su Linux; MSVC non compilato. Il fix 2 è verificato con ASan/UBSan sui 23 programmi, non sull'intera suite sotto sanitizer.
