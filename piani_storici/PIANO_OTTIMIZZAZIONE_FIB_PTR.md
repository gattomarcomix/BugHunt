# BabyC: piano di ottimizzazione di `fib_ptr` (VM)

Data: 2026-10-07 · Baseline: `babyc-FULL-gidx-verified` + fix `ir_fast.c`
Misure: gcc 13 `-O2`, sandbox, `BABYC_NO_MEMO=1`; i tempi assoluti dipendono dall'host, contano i rapporti.

## Obiettivo

| | tempo | ns per chiamata (2,69M chiamate) | vs Python (0,17 s) |
|---|---|---|---|
| oggi, VM | 0,44 s | ~163 | ~2,6× più lenta |
| traguardo | ≤ 0,15 s | ~55 | ~1,1× più veloce |
| ottimistico | ≤ 0,11 s | ~41 | ~1,5× più veloce |

Con tutti i controlli attivi e gli stessi messaggi. Il tetto del prototipo (senza controlli, senza frame veri) è ~16 ns a chiamata.

## Profilo di partenza (gprof, `EXEC=vm`, tempi approssimati)

- `ExpressionAssignInt`: ~39% (include `VariableAddressIsStringLiteral`, 29%). È lo store di `calls++` sul percorso generico, una volta per chiamata.
- `Step`: ~45%, 37,7M esecuzioni (~14 istruzioni VM per chiamata, ognuna una chiamata C).
- `IrApplyBinOp`/`IrApplyBinOpRaw`: 9,4M chiamate (~3,5 per `fib`).
- `IrGlobalVal` 5,4M e `TableGet` 2,7M: ricerche per nome delle globali.
- `FnCodeRun`: trascurabile.

## Regole

- Un passo alla volta; dopo ciascuno la suite deve restare **742/0 con `EXEC=vm`**, con output e messaggi identici.
- Ogni passo riporta tre colonne: albero, VM prima, VM dopo; 5 run alternati e ns/chiamata.
- Se un passo guadagna meno del 5%, si ripete il profilo prima di proseguire.
- Si elimina solo ciò che è dimostrabilmente ridondante; nel dubbio il controllo resta.

## Passi

### 1. Globali scalari dirette (guadagno atteso ~35–40%)

- A compile time, ogni `LOADG`/`STOREG` risolve il `struct Value *` della globale e lo salva nella tabella dei side data. Niente più ricerca per nome a runtime.
- Lo store scrive direttamente con il restringimento di tipo noto (`int`, `short`, `char`, `long long`, con segno).
- Ridondante e quindi eliminabile: il controllo "indirizzo è un letterale stringa" (un `int` globale non lo è) e `BabycHeapCheckWrite` (non è memoria heap).
- Da mantenere: il flag "inizializzata" della globale, impostato al primo store.
- Stima: 0,44 s → ~0,27 s.

### 2. Operazioni binarie senza chiamata di funzione (~5–10%)

- Specializzare gli opcode (`ADD`, `SUB`, confronti) con le varianti 32/64 bit con e senza segno, inline.
- Divisione, modulo e shift restano sul percorso con controllo (div0 con riga, shift con segno e larghezza). Qui rientra il fix di `IrApplyBinOpRaw` (flag passato come puntatore).
- Il tracciamento del segno a runtime (`LinRegU[]`, letto e scritto a ogni `IrLinBin`) si decide invece a compile time dai tipi dichiarati.

### 3. Dispatch stretto (~25–30% del totale)

- Un solo ciclo `VmRun` con `ip`, registri e finestra in variabili locali, senza chiamare `Step` per istruzione.
- `switch` standard; `computed goto` sotto `#ifdef __GNUC__` (MSVC non lo ha).
- Stima cumulata dopo i passi 1–3: ~0,15–0,18 s.

### 4. Meno istruzioni per chiamata (~20–30% del dispatch)

- Superistruzioni: confronto con salto (`BLTI r, imm, L`), `ADDI`, incremento di globale fuso (`INCG`), `RET` con valore.
- Obiettivo: da ~14 a ~8–9 istruzioni per `fib`.
- Verificare con il bytecode stampato che la fusione non salti nessun controllo.

### 5. Percorso di chiamata

- `CALL` salva solo `pc`, base e puntatore al codice in una pila di ritorni; gli argomenti sono già nei primi registri del callee.
- Uninit: i parametri sono sempre inizializzati; per i locali, analisi di definite-assignment a compile time e controllo di bit solo dove non dimostrato.
- Verificare quanto costano oggi i salvataggi e ripristini di statici a ogni ingresso (`LinRetSeen`, `LinStepAbort`, `LinDiv0`, `FnActive`).

## Verifica e decisione

- **Dopo il passo 1:** se non si scende sotto ~0,30 s, il profilo del passo 1 va rifatto prima di toccare altro.
- **Dopo i passi 1–3:** se ≥ 0,20 s, ripetere il profilo (con `perf` se disponibile) invece di proseguire alla cieca.
- **Dopo il passo 5:** riportare il rapporto con Python. 1,1× è il traguardo, 1,5× l'ottimistico; non è garantito.
- Controllo incrociato: `fib_global`, `fibc`, `call_loop` e `gcd`/`collatz` non devono peggiorare.

## Rischi

- gprof perturba i tempi: usare `perf` e conteggio di istruzioni quando possibile.
- Le superistruzioni complicano gli errori: la tabella `pc → (riga, colonna, nome)` deve coprirle.
- Dipendenza dal Passo 0 del piano array/puntatori (bug del VM): senza quel fix i guadagni misurati non sono fidabili.

## Da decidere insieme

- Si accetta ~0,15 s (1,1× Python) come traguardo, o si pretende 1,5× a ogni costo?
- Un build con `computed goto` solo GCC va bene, con `switch` su MSVC?
