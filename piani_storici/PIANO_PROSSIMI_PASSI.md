# BabyC: prossimi passi (bytecode per-funzione)

Data: 2026-10-06 · Baseline: ultimo `babyc.zip` · Misure: gcc 13 -O2, best of 3, sandbox (i valori assoluti dipendono dall'host, contano i rapporti)

## Vincoli (non negoziabili)

- I controlli di sicurezza restano tutti attivi: uninit, scope, OOB, double free. Si elimina solo ciò che è **dimostrabilmente ridondante**; nel dubbio il controllo resta e i messaggi d'errore non cambiano.
- Obiettivo: la VM deve essere **più veloce dell'albero**, con tutti i controlli attivi.
- Nessun tuning su un singolo bench, nessuna scorciatoia sulla forma del programma.

## Punto di partenza (misurato)

| caso | albero | Python |
|---|---|---|
| fib_ptr | 0,90 s | 0,23 s |
| call_loop | 0,71 s | – |

- `EXEC=vm` oggi ha lo stesso tempo dell'albero (`lowered=0`): ricade sull'albero, non esegue bytecode.
- Prototipo `vm_proto` (solo interi, senza controlli): fib(30) in 0,044 s. È un limite superiore, non una stima.
- Profilo `fib_ptr`: il costo è diffuso su decisioni di tipo e scope ripetute a ogni nodo, non nei controlli uninit (`BABYC_NO_UNINIT=1` non cambia il tempo).

## 1. Baseline onesta (prerequisito, piccolo)

Cosa:
- Aggiungere `BABYC_NO_KERNEL=1` che spegne i kernel di loop (oggi non esiste un flag).
- Correggere o documentare il SEGV di `EXEC=both` su `array_sum`, `isort`, `dfs_rec`, `matmul_small`.
- Allineare il codice ai doc: oggi `IrShapeFpEnabled()` è attivo di default (`BABYC_NO_SHAPE_FP`), mentre il doc dice spento (`BABYC_SHAPE_FP`).
- Aggiungere una doppia ricorsione multi-argomento vera (`fib_multiarg` fa solo 30 chiamate) e un `.py` per le varianti v2.

Verifica: lo script di bench stampa sempre tre colonne (albero, albero senza kernel, Python) per tutti i bench. Suite 760/0 invariata.

## 2. Fetta verticale `FnCode` (il lavoro centrale)

Cosa: compilare per funzione, a partire dall'albero, **solo** interi, aritmetica, branch, `if/while/for`, globali scalari e chiamate dirette a funzioni utente.
- Perimetro deciso da `IrFnBodyIntegerEligible` (oggi definita ma non chiamata). Fuori perimetro: la funzione resta sull'albero (fallback per funzione).
- Finestra di registri su una pila contigua, argomenti scritti nei primi registri del callee, loop VM iterativo, senza `IrCallFrame`.
- Chiamata a funzione non compilata: `CALL_HOST`, rientro nell'albero.
- Tipi ricavati dalle dichiarazioni (locali, parametri, globali, `FuncDef.ReturnType`), non dai flag dei nodi, che sono incompleti.
- Uninit: bitmask per frame più analisi di definite-assignment; il controllo si emette dove non è dimostrato inutile. Scope risolto a compile time.
- Div/mod per zero e ogni errore con riga da una tabella `pc → riga`; messaggi uguali agli `.expect`.
- Flag `BABYC_EXEC=vm`, spento di default, costo zero a flag spento.

Verifica (gate):
- Suite A/B ancora a 760/0 con `EXEC=vm`, e `fuzz4` con colonna VM.
- Stesso output e stessi messaggi d'errore dell'albero su tutti i test della fetta.
- Prestazioni su `fib_ptr`, `fib_global`, `fibc`, `call_loop` e sulla doppia ricorsione: **VM ≥ 2× più veloce dell'albero con tutti i controlli attivi**. Si riportano tre varianti compilate (senza controlli, con uninit, con tutto) per sapere cosa costa cosa.

## 3. Decisione e pulizia

- Con i numeri del punto 2: se il gate è raggiunto, si estende alla fetta successiva (array e puntatori, con la cella heap tracciata). Se non lo è, si usa il profilo per decidere dove sta il costo e si riconsidera prima di estendere.
- Smettere di investire nello shadow e nel soft-match: sono diagnostica sul path albero e non spostano le prestazioni.
- Allineare `HANDOFF` e `F6_CLOSE` su F1; un report unico di stato per fase, con le tre colonne del punto 1.

## Da verificare insieme

- Soglia "≥ 2×" va bene, o ne preferite un'altra?
- Il perimetro del punto 2 deve includere anche `printf` (richiede `CALL_HOST` fin dall'inizio)?
- Chi tiene i doc allineati al codice (punto 3)?
