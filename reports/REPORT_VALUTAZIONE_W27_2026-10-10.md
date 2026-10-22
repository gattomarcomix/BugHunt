# BabyC: valutazione della baseline w27 (2026-10-10)

Ambiente: sandbox Linux a 1 core, gcc 13 `-O2`, Python 3.12.3, `BABYC_NO_MEMO=1`, senza kernel (`BABYC_NO_KERNEL=1`).
Rapporto = tempo Python / tempo BabyC (sopra 1 BabyC è più veloce). I rapporti usano un riferimento Python unico, **best of 7** (`data/python_best_of_7.json`).

## 1. Sintesi

- Correttezza solida: suite **827/0 in tutte e tre le modalità** (albero, VM lineare, bytecode compatto); anche con il dispatch `switch` usato da MSVC (827/0 in VM e bytecode). I miei 23 test di regressione passano 23/23 in tutte le modalità.
- C'è un nuovo motore bytecode compatto (`BABYC_BYTECODE_VM=1`, opt-in). **Per ora è più lento della VM lineare** (0,68× contro 0,83× di Python su 18 programmi).
- Progresso reale ma modesto sulla VM lineare: `a02_qsort` 1,9× più veloce, `a03_heapsort` 2,4×, `a04_matmul` 2,3× (run alternati). Le funzioni con parametro puntatore ora entrano nella VM.
- **Regressione reale:** `a01_sieve` +54% (0,203 -> 0,312 s) con lo **stesso** numero di istruzioni VM (49.682.489); `a10_bsearch` +9%.
- Target 1,5–2× Python: slice 1 a 1,03×, slice 2 a 0,75×, complessivo 0,83×. Lontano.
- **Correzione a un mio numero precedente:** lo snapshot precedente risultava 1,02× complessivo perché nella corsa di allora Python era il 34% più lento del valore stabile. Con lo stesso riferimento Python, quello snapshot è **0,76×**. I confronti tra snapshot vanno fatti con un solo riferimento Python o in tempi assoluti.

## 2. Cosa contiene il lavoro (dai documenti dello snapshot)

- Motore bytecode compatto opt-in: array globali multidimensionali fino a 4D, load unsigned indicizzati, cache dei bytecode, fast-path aritmetici, costanti larghe in side table.
- Test differenziali (`tools/test_bytecode_vm.py`, `tools/fuzz4.py`) che richiedono `runs >= 1` per attribuire copertura al bytecode.
- Integrati il mio fix `AddI` sulle costanti larghe (`Tok < 2`) e la suite benchmark `bench/real`.
- Valutazione interna onesta (`VM_COVERAGE_AND_PROJECT_REASSESSMENT`): solo 7 benchmark su 19 eseguono davvero il bytecode compatto; raccomanda di sospendere le ottimizzazioni del dispatch finché la copertura non è affidabile.

## 3. Verifiche

| controllo | esito |
|---|---|
| build GCC 13 | OK, 3 avvisi (`ir_call.c:320` format-security; 2 `write` ignorati in `platform.c`) |
| suite, albero | 827 passati, 0 falliti, 22 saltati |
| suite, `EXEC=vm` | 827/0 |
| suite, `EXEC=vm` + `BABYC_BYTECODE_VM=1` | 827/0 |
| suite con dispatch `switch` (`BABYC_LIGHT_SWITCH`), VM e bytecode | 827/0 in entrambe |
| `new_tests/` (23), albero / VM / bytecode | 23/23 in tutte |
| `bench_real` (18), albero / VM / bytecode | 18/18 corretti in tutte |

I salti calcolati (`&&label`) sono solo in `ir_lower.c`, protetti da `#if defined(__GNUC__)`. Windows/MSVC non compilato in questa sessione.

## 4. Prestazioni (18 programmi)

Tempi in secondi; rapporti con Python best of 7.

| bench | Python | VM prec. | VM w27 | bytecode w27 | VM prec. vs Py | VM w27 vs Py | bytecode vs Py | albero vs Py |
|---|---|---|---|---|---|---|---|---|
| a01_sieve | 0.296 | 0.197 | 0.330 | 0.446 | 1.51× | 0.90× | 0.66× | 0.72× |
| a02_qsort | 0.204 | 0.653 | 0.320 | 0.352 | 0.31× | 0.64× | 0.58× | 0.36× |
| a03_heapsort | 0.278 | 1.216 | 0.514 | 0.777 | 0.23× | 0.54× | 0.36× | 0.25× |
| a04_matmul | 0.255 | 0.390 | 0.181 | 0.342 | 0.65× | 1.41× | 0.75× | 0.65× |
| a05_lcs | 0.269 | 0.154 | 0.169 | 0.167 | 1.75× | 1.59× | 1.61× | 0.58× |
| a06_knapsack | 0.243 | 0.177 | 0.224 | 0.225 | 1.38× | 1.09× | 1.08× | 0.54× |
| a07_crc32 | 0.221 | 0.272 | 0.286 | 0.431 | 0.81× | 0.77× | 0.51× | 0.57× |
| a08_text | 0.209 | 0.497 | 0.409 | 0.501 | 0.42× | 0.51× | 0.42× | 0.47× |
| a09_life | 0.279 | 0.630 | 0.584 | 0.978 | 0.44× | 0.48× | 0.29× | 0.30× |
| a10_bsearch | 0.316 | 0.466 | 0.558 | 0.754 | 0.68× | 0.57× | 0.42× | 0.52× |
| a11_bfs | 0.207 | 0.739 | 0.803 | 0.759 | 0.28× | 0.26× | 0.27× | 0.26× |
| a12_fact | 0.283 | 0.173 | 0.192 | 0.359 | 1.63× | 1.47× | 0.79× | 0.64× |
| i01_collatz | 0.370 | 0.424 | 0.402 | 0.396 | 0.87× | 0.92× | 0.94× | 0.43× |
| i02_nqueens | 0.247 | 0.174 | 0.177 | 0.180 | 1.42× | 1.39× | 1.37× | 0.33× |
| i03_primes | 0.202 | 0.278 | 0.262 | 0.270 | 0.73× | 0.77× | 0.75× | 0.35× |
| i04_gcd_table | 0.286 | 0.316 | 0.289 | 0.302 | 0.91× | 0.99× | 0.95× | 0.38× |
| i05_hash_unsigned | 0.280 | 0.201 | 0.200 | 0.203 | 1.40× | 1.40× | 1.38× | 0.68× |
| i06_tak | 0.296 | 0.364 | 0.337 | 0.342 | 0.82× | 0.88× | 0.87× | 0.21× |

Medie geometriche:

| | slice 1 (interi) | slice 2 (array) | tutti |
|---|---|---|---|
| VM lineare, snapshot precedente | 0.99× | 0.67× | 0.76× |
| VM lineare, w27 | 1.03× | 0.75× | 0.83× |
| bytecode compatto, w27 | 1.01× | 0.56× | 0.68× |
| albero, w27 | 0.37× | 0.46× | 0.43× |

### Confronto A/B a run alternati (stessa macchina)

| bench | snapshot prec. | w27 | w27 / prec. |
|---|---|---|---|
| a01_sieve | 0,203 | 0,312 | **1,54× (peggio)** |
| a10_bsearch | 0,465 | 0,508 | 1,09× (peggio) |
| a02_qsort | 0,591 | 0,319 | 0,54× (meglio) |
| a03_heapsort | 1,240 | 0,512 | 0,41× (meglio) |
| a04_matmul | 0,402 | 0,174 | 0,43× (meglio) |

Per `a01_sieve` e `a10_bsearch` i contatori `BABYC_VM_PROFILE` coincidono nei due snapshot (stessi `lin_inst`, stesse ammissioni): le istruzioni sono le stesse e più lente, quindi il rallentamento è nel gestore, non nell'ammissione. Sospetto sugli store indicizzati `char`/unsigned (non verificato).

## 5. Problemi e osservazioni

1. **Bytecode compatto più lento della VM lineare.** Esempi: `a03` 0,777 s contro 0,514, `a09_life` 0,978 contro 0,584, `a07_crc32` 0,431 contro 0,286, `a04_matmul` 0,342 contro 0,181. Quando non entra nel bytecode ricade sulla VM lineare e coincide con essa.
2. **Regressione `a01_sieve`** (sezione 4); riproduzione: `tools/compare_snapshots.py <A> <B> bench_real a01_sieve`.
3. **Punti deboli:** `a11_bfs` 0,28×, `a09_life` 0,50×, `a10_bsearch` 0,57×, `a08_text` 0,60×, `i06_tak` 0,92× (il costo per chiamata resta il limite).
4. **Avviso `format-security`** in `ir_call.c:320`: stringa di formato non letterale, da controllare.
5. Il rumore del sandbox sposta i rapporti di ±20–30% tra corse diverse: confrontare con run alternati o in tempi assoluti.

## 6. Prossimi passi

1. Spiegare e correggere la regressione di `a01_sieve` (gate: ≤ 0,21 s con lo stesso `lin_inst`).
2. Tenere la VM lineare come percorso principale; riprendere il bytecode compatto solo quando abbia tempi ≤ VM lineare sui 18 benchmark.
3. Attaccare `bfs`, `life`, `bsearch`, `text` e il costo per chiamata (`tak`) con profilo per funzione.
4. Fissare la metodologia: Python best of 7, run alternati per i confronti tra snapshot.

## 7. Contenuto del pacchetto

- `bench_real/`: 18 programmi C+Python e runner a tre colonne (`tree-nk`, `vm-nk`, `bc-nk`).
- `new_tests/`: 23 test di regressione (id 880–907) con `.expect`.
- `data/`: risultati JSON di w27 e dello snapshot precedente, e Python best of 7.
- `tools/compare_snapshots.py` (A/B a run alternati) e `tools/ratios_common_python.py` (rapporti con riferimento Python comune).

## 8. Limiti

Sandbox a 1 core; Python 3.12.3; solo GCC su Linux. Non è stato eseguito ASan/UBSan. Strutture, liste e heap non sono coperti da `bench_real`.
