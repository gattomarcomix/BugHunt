# BabyC: aggiornamento per il team (2026-10-08)

Stato analizzato: `babyc_8_.zip` + deltas `coverage-push`, `713-743-fix`, `bytecode-cont` (ultimo stato, cumulativo).
Misure: sandbox a 1 core, gcc 13 `-O2`, Python 3.12.3, best of 2, `BABYC_NO_MEMO=1`. I tempi assoluti dipendono dall'host: contano i rapporti.

## 1. Sintesi

- La VM per funzione è cresciuta molto: suite in modalità VM **767/6** (da 720/53), albero **773/0**, debug su stderr rimosso.
- Sulla slice 1 (interi, chiamate) la VM ora è **corretta su 6 casi su 6** e batte Python: **1,12×** senza kernel (1,20× con).
- Sulla slice 2 (array, puntatori) la VM dà **risultati sbagliati senza alcun errore su 7 benchmark su 12**. La suite da 773 test non lo vede.
- Causa probabile del bug: costanti oltre `INT_MAX` (`% 2147483648`) mal gestite quando una funzione VM è chiamata da un'altra funzione VM (sezione 5.1).
- Questo pacchetto aggiunge 22 test di regressione veloci (sezione 6) che rendono il bug visibile subito: albero 22/22, VM 12/22.
- Con il target 1,5–2× Python: slice 1 vicina (1,1–1,3×), slice 2 ancora non misurabile finché il bug sopra non è chiuso.

## 2. Cosa è stato consegnato (catena dei delta)

Ogni delta sostituisce `ir_lower.c`, quindi sono cumulativi:

| stato | note | VM | albero |
|---|---|---|---|
| `babyc_8_.zip` | coincide con il delta *ptr-return-coverage* (diff zero) | 720/53 | 773/0 |
| + *coverage-push* | costanti larghe nel Light, shift unsigned, `*p=` | 750/23 | – |
| + *713-743-fix* | zero-extend unsigned in `StoreLocal`, `&scalar` | 755/18 | – |
| + *bytecode-cont* (**ultimo**) | uninit su elementi di array, null deref | **767/6** | 773/0 |

Per riprodurre l'ultimo stato: scompattare `babyc_8_.zip`, copiare `ir_lower.c` del delta *bytecode-cont* in `src/`, `make -C src`. Non si può compilare il delta da solo (manca il resto dei sorgenti).

## 3. Suite di regressione (773 test)

Residui in modalità VM (6):

| test | sintomo |
|---|---|
| `061_initializers` | SEGV |
| `219_uninit_use`, `765_lin_uninit_diag` | errore uninit senza riga/colonna corrette (appare `1:18`) |
| `508_bitfield_sizeof` | manca l'errore `sizeof applied to a bitfield` (stampa 0) |
| `535_bitfield_byvalue_chain` | stampa 0 invece di 16 |
| `772_fptr_typedef_local` | `'fp' is undefined` |

## 4. Prestazioni su codice verosimile (18 programmi, `bench_real/`)

Rapporto = tempo Python / tempo BabyC (sopra 1 BabyC è più veloce). `vm-nk` = VM con `BABYC_NO_KERNEL=1`: è la colonna che conta.

| bench | Python s | albero-nk s | VM s | VM-nk s | albero-nk vs Py | VM-nk vs Py |
|---|---|---|---|---|---|---|
| a01_sieve | 0.302 | 0.465 | 0.233 | 0.229 | 0.65× | 1.32× |
| a02_qsort | 0.202 | 0.617 | DIFF | DIFF | 0.33× | – |
| a03_heapsort | 0.288 | 1.229 | DIFF | DIFF | 0.23× | – |
| a04_matmul | 0.252 | 0.398 | 0.161 | 0.519 | 0.63× | 0.49× |
| a05_lcs | 0.310 | 0.538 | DIFF | DIFF | 0.58× | – |
| a06_knapsack | 0.307 | 0.544 | DIFF | DIFF | 0.57× | – |
| a07_crc32 | 0.250 | 0.447 | DIFF | DIFF | 0.56× | – |
| a08_text | 0.233 | 0.473 | 0.511 | 0.627 | 0.49× | 0.37× |
| a09_life | 0.312 | 0.944 | DIFF | DIFF | 0.33× | – |
| a10_bsearch | 0.336 | 0.640 | 0.747 | 0.739 | 0.53× | 0.46× |
| a11_bfs | 0.266 | 0.894 | DIFF | DIFF | 0.30× | – |
| a12_fact | 0.309 | 0.554 | 0.191 | 0.247 | 0.56× | 1.25× |
| i01_collatz | 0.424 | 0.965 | 0.379 | 0.332 | 0.44× | 1.28× |
| i02_nqueens | 0.258 | 0.884 | 0.163 | 0.205 | 0.29× | 1.26× |
| i03_primes | 0.321 | 0.605 | 0.231 | 0.244 | 0.53× | 1.32× |
| i04_gcd_table | 0.320 | 0.922 | 0.296 | 0.315 | 0.35× | 1.02× |
| i05_hash_unsigned | 0.299 | 0.439 | 0.412 | 0.422 | 0.68× | 0.71× |
| i06_tak | 0.443 | 1.485 | 0.288 | 0.339 | 0.30× | 1.31× |

Medie geometriche (solo casi corretti):

| | albero-nk | VM | VM-nk |
|---|---|---|---|
| slice 1 (i*) | 0.41× (6/6) | 1.20× (6/6) | 1.12× (6/6) |
| slice 2 (a*) | 0.46× (12/12) | 0.92× (5/12) | 0.67× (5/12) |
| tutti | 0.44× (18/18) | 1.06× (11/18) | 0.89× (11/18) |

Lettura:
- Slice 1: `i01_collatz` 1,28×, `i02_nqueens` 1,26×, `i03_primes` 1,32×, `i06_tak` 1,31× senza kernel. `i05_hash_unsigned` (unsigned a 32 bit) resta 0,71× e `i04_gcd_table` è in pari (1,02×).
- Slice 2: dove è corretta, la VM senza kernel è in pari o lenta (`a04_matmul` 0,49×, `a08_text` 0,37×, `a10_bsearch` 0,46×); `a01_sieve` 1,32× e `a12_fact` 1,25× fanno eccezione.
- Il rapporto 1,5–2× richiede ancora il lavoro di ottimizzazione di `fib_ptr` (vedi `docs/PIANO_OTTIMIZZAZIONE_FIB_PTR.md`).

## 5. Bug aperti con causa

### 5.1 Costanti larghe nella VM (silenzioso, grave)

- Sintomo minimo: `int f(long long x){ return x % 2147483648; }` con `x = 100000000000LL`: albero **1215752192**, VM **0**. `x / 2147483648`: albero 46, VM −1539607552. `x % 2147483647` è corretto; la soglia è `INT_MAX`.
- Condizione: fallisce quando il chiamante è anch'esso compilato dalla VM (`a[i] = rnd()` nel `main` VM). Se il `main` resta sull'albero, funziona.
- Causa probabile: gli immediati oltre 32 bit stanno in una tabella globale (`LinWideImm`/`LinWideOn`, `ir_lower.c`) indicizzata dal numero di istruzione e condivisa da tutte le funzioni compilate; le chiamate dentro la VM non la cambiano. **Non ho verificato la riga esatta.**
- Effetti misurati: `a02_qsort`, `a03_heapsort`, `a05_lcs`, `a06_knapsack`, `a07_crc32`, `a09_life`, `a11_bfs` (tutti usano un LCG con `% 2147483648`) producono array a zero e checksum sbagliati.
- Test: 780, 781, 782 e le versioni ridotte 797, 798, 800, 801, 802, 804, 806.

### 5.2 Diagnostici nella VM (riga, colonna, nome)

Il bytecode di produzione non porta la posizione dei sorgenti: gli errori compaiono come `1:18` e senza il nome della variabile. Test 219, 765.

### 5.3 Altri

- `061_initializers`: SEGV (inizializzatori con parentesi graffe).
- Bitfield (508, 535) e puntatore a funzione typedef locale (772).
- `a08_text` e `a10_bsearch` sono corretti ma più lenti di Python con la VM: la slice 2 è ancora per lo più sull'albero (2–14% di istruzioni VM).

### 5.4 Già risolti rispetto al report precedente

`fill` non inizializzato e ibridi con `ParamSlot` (ora nel pacchetto), `IrApplyBinOpRaw` con flag di segno e larghezza, `XXX` al posto delle stringhe (`%s` con locali), arresto silenzioso in `231`/`059`/`215`, output perso in `i04_gcd_table`, debug su stderr.

## 6. Nuovi test (cartella `new_tests/`)

22 file `NNN_nome.c` + `NNN_nome.expect` (CRLF, come gli altri), output verificati con gcc `-O2`. Numerazione 780–807 (gli esistenti arrivano a 776). Tempo massimo in modalità albero: 0,34 s.

| id | contenuto | albero | VM (ultimo) |
|---|---|---|---|
| 780 `vm_wide_const_ops` | `%`, `/`, `+` con costanti > INT_MAX | OK | **DIFF** |
| 781 `vm_lcg_array_fill` | `a[i] = rnd()`, LCG `% 2147483648` | OK | **DIFF** |
| 782 `vm_printf_call_arg` | `printf("%d", rnd())` in `main` VM | OK | **DIFF** |
| 783 `vm_nested_loop_call_if` | cicli annidati + chiamata + `if` | OK | OK |
| 790–795 `real_i01…i06` | versioni ridotte degli interi | 6/6 | 6/6 |
| 796–807 `real_a01…a12` | versioni ridotte di array/puntatori | 12/12 | 5/12 |

Totale: albero 22/22, VM 12/22 (le 10 differenze sono tutte riconducibili a 5.1).

Installazione:
```
cp new_tests/78* new_tests/79* new_tests/80* <babyc>/src/tests/
python3 <babyc>/tools/run_suite.py <babyc>/src
BABYC_EXEC=vm python3 <babyc>/tools/run_suite.py <babyc>/src   # filtrare l'avviso stderr di EXEC=vm se serve
```
Nota: con `EXEC=vm` il motore stampa su stderr un avviso `experimental`; il runner della suite fonde stdout e stderr, quindi senza filtro tutti i test appaiono falliti.

## 7. Prossimi passi

1. **Chiudere 5.1** (immediati larghi nelle chiamate VM→VM). Gate: test 780–782 e le versioni ridotte `real_a*` passano in modalità VM.
2. **Aggiungere `new_tests/` alla suite** e rendere il gate di ogni consegna "albero 773+22 / VM senza regressioni".
3. **Rieseguire `bench_real/`** con `--runs 3`. Solo allora si legge il vero valore della slice 2.
4. **Posizione degli errori nella VM** (5.2) e i residui 061/508/535/772.
5. **Ottimizzazione di `fib_ptr`** (`docs/PIANO_OTTIMIZZAZIONE_FIB_PTR.md`) per avvicinarsi al target 1,5×.

Metrica proposta per il target (1,5–2× Python): media geometrica di `tempo_Python / tempo_BabyC` su `bench_real/` con `BABYC_NO_KERNEL=1`, tutti i controlli attivi, nessun benchmark sotto 0,8×.

## 8. Come riprodurre

```
# build dello stato più recente
unzip babyc_8_.zip && cp babyc-delta-bytecode-cont/ir_lower.c babyc/src/ && make -C babyc/src -j4

# benchmark verosimili
cd bench_real && python3 run_real_bench.py --babyc ../babyc/src/babyc --runs 3
```

## 9. Limiti delle misure

- Sandbox a 1 core: i tempi di Python variano 2–5× tra run singoli; usare `--runs 3` o più.
- Il numero di test ridotti verifica la correttezza ma non le prestazioni: per quelle servono i programmi di `bench_real/`.
- Strutture, liste e heap non sono coperti dalla suite `bench_real`.

## 10. Contenuto del pacchetto

- `REPORT_TEAM_2026-10-08.md`: questo documento.
- `new_tests/`: 22 test di regressione con output atteso.
- `bench_real/`: 18 programmi C+Python, runner, README e risultati (`results_cont_2026-10-08.json`).
- `docs/`: piani e report precedenti (parte di essi sono superati dalla sezione 5.4).
