# BabyC: valutazione dello snapshot "BabyCG 10 consolidata" (2026-10-09)

Ambiente: sandbox Linux a 1 core, gcc 13 `-O2`, Python 3.12.3, best of 2, `BABYC_NO_MEMO=1`. I tempi assoluti dipendono dall'host: contano i rapporti.
Rapporto = tempo Python / tempo BabyC (sopra 1 BabyC è più veloce). `VM-nk` = `BABYC_EXEC=vm` con `BABYC_NO_KERNEL=1`: è la colonna che conta.

## 1. Sintesi

- Netto passo avanti: **tutti i 18 benchmark "codice vero" danno ora l'output corretto con la VM** (nello snapshot precedente 11 su 18).
- Prestazioni VM-nk: slice 1 **1,31×** (prima 1,12×), slice 2 **0,90×** (prima 0,67×, con 5/12 corretti), complessivo **1,02×** (prima 0,89×).
- Suite: albero 787/0, VM 786/1 (prima 785/2). La correzione MSVC (dispatch con `switch`) è già integrata.
- Trovato e corretto un bug residuo: **parametro ± costante oltre `INT_MAX`** dà risultati sbagliati nella VM (`x + 4294967296` -> 3 invece di 4294967297). Fix di una riga in `fix/`, verificato.
- Restano aperti: funzioni con parametro puntatore inoltrato (qsort, heapsort, bfs) non ammesse dalla VM; troppe istruzioni nei cicli stretti (`life`, `matmul`); `388_lib_crash_report` rosso con `EXEC=vm`.
- Rispetto al target 1,5–2× Python: slice 1 a 1,3×, slice 2 a 0,9×.

## 2. Verifiche

| controllo | snapshot | snapshot + fix |
|---|---|---|
| suite, albero | 787/0 | 788/0 (include il test 780 aggiunto) |
| suite, `EXEC=vm` | 786/1 (`388`) | 787/1 (`388`) |
| `new_tests/` (23), albero | 23/23 | 23/23 |
| `new_tests/` (23), `EXEC=vm` | 21/23 (880, 884) | **23/23** |
| benchmark `bench_real` (18), `EXEC=vm` | 18/18 corretti | non rimisurato |

## 3. Prestazioni su codice verosimile (18 programmi)

| bench | Python s | albero-nk s | VM s | VM-nk s | albero-nk vs Py | VM-nk vs Py |
|---|---|---|---|---|---|---|
| a01_sieve | 0.502 | 0.446 | 0.213 | 0.197 | 1.13× | 2.55× |
| a02_qsort | 0.211 | 0.579 | 0.600 | 0.653 | 0.36× | 0.32× |
| a03_heapsort | 0.326 | 1.294 | 1.279 | 1.216 | 0.25× | 0.27× |
| a04_matmul | 0.344 | 0.374 | 0.147 | 0.390 | 0.92× | 0.88× |
| a05_lcs | 0.366 | 0.476 | 0.155 | 0.154 | 0.77× | 2.38× |
| a06_knapsack | 0.429 | 0.514 | 0.177 | 0.177 | 0.84× | 2.43× |
| a07_crc32 | 0.270 | 0.467 | 0.276 | 0.272 | 0.58× | 0.99× |
| a08_text | 0.304 | 0.420 | 0.385 | 0.497 | 0.72× | 0.61× |
| a09_life | 0.358 | 0.884 | 0.597 | 0.630 | 0.41× | 0.57× |
| a10_bsearch | 0.489 | 0.559 | 0.449 | 0.466 | 0.88× | 1.05× |
| a11_bfs | 0.272 | 0.805 | 0.788 | 0.739 | 0.34× | 0.37× |
| a12_fact | 0.337 | 0.433 | 0.173 | 0.173 | 0.78× | 1.94× |
| i01_collatz | 0.469 | 0.864 | 0.395 | 0.424 | 0.54× | 1.11× |
| i02_nqueens | 0.319 | 0.741 | 0.176 | 0.174 | 0.43× | 1.83× |
| i03_primes | 0.251 | 0.543 | 0.260 | 0.278 | 0.46× | 0.91× |
| i04_gcd_table | 0.389 | 0.896 | 0.319 | 0.316 | 0.43× | 1.23× |
| i05_hash_unsigned | 0.467 | 0.454 | 0.195 | 0.201 | 1.03× | 2.33× |
| i06_tak | 0.358 | 1.771 | 0.368 | 0.364 | 0.20× | 0.99× |

Medie geometriche (solo casi corretti):

| | albero-nk | VM | VM-nk |
|---|---|---|---|
| slice 1 (i*) | 0.46× (6/6) | 1.34× (6/6) | 1.31× (6/6) |
| slice 2 (a*) | 0.60× (12/12) | 1.00× (12/12) | 0.90× (12/12) |
| tutti | 0.55× (18/18) | 1.10× (18/18) | 1.02× (18/18) |

Lettura:
- **Bene:** `a01_sieve` 2,55×, `a06_knapsack` 2,43×, `a05_lcs` 2,38×, `i05_hash_unsigned` 2,33× (era 0,71×), `a12_fact` 1,94×, `i02_nqueens` 1,83×.
- **Male:** `a03_heapsort` 0,27×, `a02_qsort` 0,32×, `a11_bfs` 0,37×, `a09_life` 0,57×, `a08_text` 0,61×. `a04_matmul` è 0,88× senza kernel (2,34× con il kernel). `i03_primes` 0,91× e `i06_tak` 0,99× sono in pari.

## 4. Bug e cause

### 4.1 Parametro ± costante larga nella VM (corretto in `fix/`)

- Sintomo: `long long f(long long x){ return x + 4294967296; }` con `x=1` -> VM **3**, atteso **4294967297**. `x - 3000000000` -> VM **-1**, atteso **-2999999999**. `4294967296 + x`, `x * 1 + C`, variabile locale o globale sono corretti: solo la forma "parametro op costante".
- Causa: il peephole `LoadX; Imm k; Bin(+/-)` -> `AddI` (`ir_lower.c`) copia `Imm` dell'istruzione `Imm` anche quando è un immediato largo (`Tok` 2/3: **indice di slot** in `LinWideImm`) o un indirizzo locale (`Tok` 4). Si somma l'indice di slot invece del valore.
- Fix: aggiungere `&& Code[i+1].Tok < 2` alla condizione del peephole. Verifica: i test 880 e 884 passano in VM; suite e benchmark non regrediscono (suite 787/1 in VM).

### 4.2 Funzioni con parametro puntatore inoltrato non ammesse (aperto, prestazioni)

- Evidenza: su `a02_qsort` (`BABYC_VM_PROFILE=1`) la VM esegue solo le 100.000 chiamate a `rnd()`; `qs` resta sull'albero (`ineligible=164344`, `ptr_index_admit=0`). Vale anche per `sift` (heapsort) e `bfs` (parametro `int *maxd`). Il riproduttore minimo è `repro/repro_qs_not_admitted.c`.
- Implicazione: sono esattamente i casi per cui P1/P2 sono stati scritti. L'ammissione conservativa (`FnCodeIndexedPointerParamsSafe`) respinge chi inoltra `v` a una chiamata ricorsiva o usa il puntatore in altre forme.

### 4.3 Troppe istruzioni nei cicli stretti (aperto, prestazioni)

`a09_life` è ammessa (192.000 chiamate) ma esegue 120 milioni di istruzioni VM (~5 ns ciascuna): mancano fusioni di confronto-e-salto e di `p[i]` con aritmetica.

### 4.4 `388_lib_crash_report` (aperto)

Unico test della suite ancora rosso con `EXEC=vm`; non analizzato (`repro/388_lib_crash_report.c`).

## 5. Contenuto del pacchetto

- `fix/ir_lower.c`: file completo corretto (sostituisce `src/ir_lower.c` dello snapshot). `fix/fix_wide_imm_peephole.patch`: la differenza (10 righe).
- `fix/gia_integrate_nello_snapshot/`: patch MSVC precedente, già nello snapshot (solo riferimento).
- `repro/`: riproduttori con output atteso e osservato (`README_repro.md`).
- `new_tests/`: 23 test con `.expect`, numerazione **880–907** (la precedente 780–807 collideva con i 782–791 dello snapshot). Output verificati con gcc.
- `bench_real/`: 18 programmi C+Python, runner, README e risultati (`results_snapshot_2026-10-09.json`).

Installazione dei test: `cp new_tests/* babyc/src/tests/`, poi `python3 babyc/tools/run_suite.py babyc/src` (e con `BABYC_EXEC=vm`; filtrare l'avviso stderr di `EXEC=vm`, che il runner fonde in stdout).

## 6. Prossimi passi

1. Applicare `fix/ir_lower.c` e inserire `new_tests/` nella suite (gate: albero e VM verdi).
2. Ammettere in VM le funzioni con parametro puntatore inoltrato (`qs`, `sift`, `bfs`). Gate: `a02`, `a03`, `a11` con `VM-nk` ≥ 1,0×.
3. Ridurre le istruzioni nei cicli stretti (superistruzioni confronto+salto, accesso `p[i]` con indice calcolato). Gate: `a09_life` e `a04_matmul` (`VM-nk`) ≥ 1,0×.
4. Analizzare `388_lib_crash_report` in VM.
5. Rieseguire `bench_real` con `--runs 3` sull'hardware di riferimento.

## 7. Limiti delle misure

- Sandbox a 1 core: i tempi di Python variano 2–5× tra run singoli; usare `--runs 3` o più.
- I test ridotti verificano la correttezza, non le prestazioni.
- Strutture, liste e heap non sono coperti da `bench_real`.
- Windows/MSVC non compilato in questa sessione (il ramo `switch` è stato verificato su Linux forzandolo).
