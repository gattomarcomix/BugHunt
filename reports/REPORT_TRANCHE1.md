# Tranche 1: crollo di prestazioni delle chiamate in VM (W31 -> W32)

Ambiente: sandbox a 1 core, gcc 13 -O2, VM lineare senza kernel (`BABYC_EXEC=vm BABYC_NO_KERNEL=1`), confronti A/B a run alternati.

## 1. Cosa ho valutato in W32 (sintesi)
Il lavoro è strutturale: descrittore unico dello storage di locali e parametri (`ir_storage.c`), helper condivisi tra albero e VM lineare, `ir_lower.c` spezzato in più file (`ir_lin_exec.c`, `ir_lin_light.c`, `ir_lin_compile.c`, `ir_fncode.c`), configurazione unificata `BABYC_PATH / ACCEL / STRICT` con alias legacy, nuovo ingresso diagnostico (`diagnostic.c`).
- Correttezza: suite albero 960/2, VM 958/4, bytecode 956/6; i fallimenti sono `097` (messaggio diagnostico), `bytecode-vm-regression`, `1038/1039` (la VM diagnostica l'uninit dietro un puntatore, l'albero no: il `.expect` fissa il comportamento dell'albero) e `964/981` solo in bytecode. I bug di W31 (`216`, `136`, `248`, `548`) sono chiusi; il collasso di `collatz`/`primes` è sparito.
- Prestazioni: la VM è in media **×2,1 più lenta di W30** (interi ×3,2): `tak` ×11, `qsort` ×10, `gcd` ×5, `heapsort` ×5, `hash` ×2,9, `nqueens` ×2,8.

## 2. Diagnosi del crollo
`i06_tak`: stessa VM (99M istruzioni contro 91M, 1 sola esecuzione di corpo) ma 3,4 s contro 0,30 s. gprof su 8,46M chiamate: `IrStorageDescSeedFrameFromFnSlot` 31%, `IrExecLinLocals` 27%, `LinTabsRestore` 15%, `FnCodeSnapshot` 12%, cioè il setup di un `IrCallFrame` completo ad ogni chiamata.
Causa: per correggere l'aliasing dei locali di `216` è stato aggiunto `&& IrCallDepth == 0` alla condizione del percorso Light (`ir_lin_exec.c`, ~riga 2425). Così **ogni chiamata annidata** perde la fast path Light e passa dal percorso pesante. In più il seeding del frame (`ir_storage.c`) cicla sempre su `IR_CALL_MAX_LOCALS` = 128 slot, anche per funzioni con 3 parametri e nessun locale.

## 3. Correzione (patch `fix/fix_01_nested_light_and_seed_bound.patch`, 4 file)
1. `IrFnCodeNestedLightSafe(CS)` (cache per slot in `FnCodeSlot.NestedLightSafe`): una chiamata annidata può usare il percorso Light se il chiamato non tocca slot locali (solo parametri, solo operazioni native del Light, niente indirizzo di locale). Non può quindi sovrascrivere i locali del chiamante: la protezione per `216` resta.
2. Il ciclo di seeding si ferma a `CS->MaxLocal + 1` invece di 128.

## 4. Verifica
| controllo | W32 | W32 + fix |
|---|---|---|
| suite albero | 960/2 | 960/2 (stessi fallimenti) |
| suite VM | 958/4 | 958/4 (stessi fallimenti) |
| i miei 23 test, albero / VM / bytecode | 23/23/23 | 23/23/23 |
| `tak`, `gcd`, `nqueens`, `hash` (s) | 3,75 / 1,63 / 0,67 / 0,53 | **1,03 / 0,77 / 0,53 / 0,42** |
| `bsearch`, `crc32`, `sieve` (s) | 1,25 / 0,50 / 0,38 | 1,07 / 0,40 / 0,30 |
| media geometrica su 12 benchmark | 1,00 | **0,76** |

Le patch si applicano con `patch -p1` dalla radice `babyc/` (provate con `--dry-run` su W32 pulita).
**Attenzione:** il Makefile non traccia le dipendenze dagli header: dopo aver applicato la patch (che cambia `FnCodeSlot`) serve una ricompilazione completa (`make -B`, o `make clean`). Con oggetti vecchi si ottengono output vuoti o falsi errori "uninitialized variable". Conviene aggiungere `-MMD -MP` al Makefile.

## 5. Ancora aperto (tranche 2)
- Rispetto a W30 restiamo ×2,2 più lenti in media: `tak` ×2,9, `gcd` ×2,4, `nqueens` ×2,5.
- `a02_qsort` (×12 rispetto a W30) e `a03_heapsort` (×5) non cambiano: hanno parametri puntatore, quindi non possono usare il Light e pagano `IrExecLinLocals`, `LinTabsRestore`, `FnCodeSnapshot` ad ogni chiamata. La prossima tranche riduce il costo del percorso pesante (snapshot e restore delle tabelle per slot).
- `1038/1039`: decidere se il `.expect` deve fissare il comportamento dell'albero (nessuna diagnostica) o quello corretto (errore uninit).
