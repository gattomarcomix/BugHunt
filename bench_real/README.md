# Suite "real code" per BabyC (VM vs albero IR vs Python)

18 programmi C verosimili, ciascuno con il suo equivalente Python, pensati per **non** assomigliare a un kernel.
Obiettivo: misurare le due fette della VM (interi/chiamate, array/puntatori) su codice che un utente scriverebbe davvero.

## Come usarla

```
python3 run_real_bench.py --babyc /percorso/babyc --runs 3
python3 run_real_bench.py --babyc /percorso/babyc --only i0      # solo slice 1
python3 run_real_bench.py --babyc /percorso/babyc --only a0,a1   # solo slice 2
```

Richiede `gcc` e `python3` (3.12 misurato). Ogni cifra è il miglior tempo su `--runs` esecuzioni (usare almeno 3: su un sandbox rumoroso un singolo run di Python può variare 2–5×).

### Colonne

| colonna | cosa è |
|---|---|
| `tree` | albero IR di default (kernel e fast-path attivi) |
| `tree-nk` | albero con `BABYC_NO_KERNEL=1` (albero "onesto") |
| `vm` | `BABYC_EXEC=vm` (kernel attivi) |
| `vm-nk` | `BABYC_EXEC=vm` + `BABYC_NO_KERNEL=1`: **la colonna che conta** |

Tutte con `BABYC_NO_MEMO=1`. Rapporto = `tempo_Python / tempo_BabyC` (> 1 = BabyC più veloce di Python).
Un caso con output diverso da gcc o crash **non entra nelle medie** e viene marcato `DIFF` / `rcNN`.
La media geometrica è riportata per slice e totale.

## Regole di progettazione (contro i bench "da kernel")

- Corpi di ciclo misti: chiamate di funzione, rami dipendenti dai dati, più array per iterazione.
- Dati pseudo-casuali (LCG deterministico); risultato ridotto a checksum verificato contro **gcc -O2 e Python**.
- Nessun ciclo del tipo "fill / somma di un array" isolato; dove compare, sta dentro un algoritmo.
- Carichi tarati perché Python impieghi ~0,3–0,5 s (l'avvio dell'interprete, ~15 ms, non pesa).
- Si usa solo il sottoinsieme che la VM/IR dichiara di supportare: niente struct né heap (vedi "Non coperto").

## I benchmark

### Slice 1: interi, globali, chiamate (`i*`)

| file | cosa stressa |
|---|---|
| `i01_collatz` | `long long`, `%`/`/`, ciclo con ramo imprevedibile, funzione chiamata 60k volte |
| `i02_nqueens` | ricorsione con operazioni bit a bit, `void`, globale scalare (`count++`) |
| `i03_primes` | divisione per tentativi, `return` in mezzo al ciclo, `d*d <= n` |
| `i04_gcd_table` | ricorsione di coda, 640k chiamate, `long long` accumulatore |
| `i05_hash_unsigned` | `unsigned int`, shift, xor, moltiplicazione con wrap a 32 bit, globale |
| `i06_tak` | ricorsione profonda e annidata (chiamate come argomenti) |

### Slice 2: array, puntatori (`a*`)

| file | cosa stressa |
|---|---|
| `a01_sieve` | array `char` globale, ciclo con passo variabile, due fasi |
| `a02_qsort` | quicksort ricorsivo su `int *v`, parametri puntatore, scambi |
| `a03_heapsort` | `sift` con indici derivati, early `return`, array via puntatore |
| `a04_matmul` | indicizzazione 1D di matrici `i*N+j`, triplo ciclo |
| `a05_lcs` | DP con tabella piatta, due stringhe `char`, confronti e max |
| `a06_knapsack` | DP 1D con ciclo in discesa, array paralleli |
| `a07_crc32` | tabella `unsigned int`, shift/xor, puntatore a `int` come buffer |
| `a08_text` | `char *` che cammina su una stringa, inversione sul posto, istogramma, `int *out` |
| `a09_life` | griglia piatta, funzione con 8 vicini e controlli di limiti, due buffer |
| `a10_bsearch` | ricerca binaria su `int *v`, istogramma, 150k query |
| `a11_bfs` | coda e distanze su griglia, ripetuto da più sorgenti, `int *maxd` |
| `a12_fact` | aritmetica a lunghezza arbitraria su array di limbi (carry, `/` e `%`) |

## Risultati di riferimento

Build: `babyc-FULL-gidx-verified` con le due correzioni a `ir_fast.c` (vedi `REPORT_ANALISI_BABYC.md`). Sandbox 1 core, gcc 13, Python 3.12.3, best of 2.

| | tree | tree-nk | vm | vm-nk |
|---|---|---|---|---|
| slice 1 (6 casi) | 0,31× | 0,31× | 0,66× (4/6 corretti) | 0,66× (4/6) |
| slice 2 (12 casi) | 0,62× | 0,39× | 0,62× (11/12) | 0,37× (11/12) |
| tutti | 0,49× | 0,36× | 0,63× | **0,43×** |

Lettura: a oggi BabyC è **più lento** di Python sulla media (≈ 2,3× con la VM onesta). Dove la VM funziona bene (`i01_collatz` 1,26×, `i03_primes` 1,09×) supera Python di poco; `i06_tak` è 0,53× (la ricorsione densa resta il punto debole).
Con `kernel` attivi, `a04_matmul`, `a05_lcs`, `a12_fact` fanno 1,9–2,6×, ma `NO_KERNEL` mostra che dipendono da fast-path e non dalla VM.

## Cosa hanno fatto emergere (da correggere)

1. **`i04_gcd_table` con `EXEC=vm`: output perso.** Il programma esce con rc=0 ma senza stampare nulla. Riproduttore minimo: `repro_vm_output_lost.c` (cicli annidati con chiamata e `if (g == 1) cop++`). Con `fflush(stdout)` esplicito l'output ricompare; `stdbuf` non lo risolve (la piattaforma imposta già il buffering). È la classe "arresto silenzioso" già vista in `231_ir_logic_sc`. Causa non isolata.
2. **SEGV con `EXEC=vm`** in `i05_hash_unsigned` e `a07_crc32` (shift/xor su `unsigned int`): la causa nota è `IrApplyBinOpRaw` che passa il flag di segno come puntatore `IrExpr *`.
3. **`i02_nqueens` non viene eseguita dalla VM** (`vm_inst_pct=0%`: solo 20 istruzioni VM): ricorsione `void` con bit a bit e globale; motivo da verificare.
4. **`EXEC=vm` sugli `a*`** rimane quasi sempre sull'albero (2–14% di istruzioni VM): è il lavoro della fetta array/puntatori, non un difetto.

## Non coperto (fetta successiva)

Strutture e membri, liste collegate (`list_walk`), `malloc`/`free`, stringhe di libreria, puntatori a puntatori e a funzione. Quando la fetta 3 sarà in lavorazione conviene aggiungere: lista concatenata con nodi in array (senza heap) poi heap vero, tabella hash con catene, parser/tokenizer.

## Come riportare i numeri

Sempre le quattro colonne e la media geometrica, mai solo `vm`: i kernel nascondono il lavoro della VM. Il gate suggerito per la fetta array è sulla colonna `vm-nk`.
