# BabyC: piano della fetta array e puntatori (VM)

Data: 2026-10-07 · Baseline: `babyc-FULL-gidx-verified` + fix `ir_fast.c` (vedi `REPORT_ANALISI_BABYC.md`)
Misure: gcc 13 `-O2`, sandbox; i tempi assoluti dipendono dall'host, contano i rapporti.

## Vincoli (invariati)

- Tutti i controlli restano attivi: uninit, scope, OOB, double free. Si elimina solo ciò che è **dimostrabilmente ridondante**; nel dubbio il controllo resta e i messaggi non cambiano.
- Obiettivo: VM più veloce dell'albero con i controlli accesi. Il rapporto con Python si riporta, senza promettere soglie.

## Profilo di `fib_ptr` con `EXEC=vm` (gprof, tempi approssimati)

| voce | peso | nota |
|---|---|---|
| `ExpressionAssignInt` (store di `calls++`) | ~39% | include `VariableAddressIsStringLiteral` (29%); più 5,4M ricerche per nome (`IrGlobalVal`, `TableGet`) |
| `Step` | ~45% | ~14 istruzioni VM per chiamata, ognuna una chiamata C |
| `FnCodeRun` | trascurabile | chiamata 27 volte: le chiamate stanno dentro la VM |

Stima: risolvendo le globali scalari a compile time (store diretto) il tempo scende da 0,44 s a ~0,27 s; con dispatch stretto (`switch` in un solo ciclo) a ~0,12–0,15 s, cioè **1,1–1,4× più veloce di Python su `fib`**. 1,5× è il limite ottimistico.

## Passo 0: prerequisiti (prima della fetta)

1. Applicare il fix di `ir_fast.c` (fill non inizializzato, ibridi con `ParamSlot`): suite albero 742/0.
2. Correggere il VM: `IrApplyBinOpRaw` (shift/div/mod senza segno), marker di riga nei diagnostici di produzione, `CallArgIsPtr` da tipi dichiarati, classe 231/059/215 (arresto silenzioso).
3. Togliere le `fprintf(stderr)` di debug (`[babyc-cov]`, `[hy]`, `[fill-hy]`, `[call]`, `[babyc-vm]`).
4. Globali scalari risolte a compile time con store diretto; dispatch stretto.
5. Compilare con `-Werror=int-conversion -Werror=maybe-uninitialized`; build ASan a ogni consegna.

**Gate:** suite 742/0 anche con `EXEC=vm`, stessi messaggi; `fib_ptr` e `fib_global` ≥ 2× l'albero.

## Perimetro della fetta

Dentro: array locali e globali (`int`, `short`, `char`, `long long`), parametri `int *p`, `a[i]`, `*p`, `p+i`, `&a[i]`, differenza e confronto di puntatori.
Fuori: struct e member, `malloc`/`free`, puntatori a puntatori, puntatori a funzione, stringhe di libreria (`printf` resta `CALL_HOST`).

## Progetto

1. **Opcode nativi con controllo dei limiti.** Oggi `IrLinLoadIndex` richiama l'albero a ogni accesso (percorso rapido disattivato con `if (0 && ...)`). Servono `LDX`/`STX` per dimensione e segno, con base e dimensione risolte a compile time e controllo `0 ≤ i < n` che emette lo stesso errore OOB.
2. **Puntatori derivati** (`p = &a[2]`, `p+1`): puntatore a due registri (indirizzo + limite) con controllo a ogni dereferenziazione; i parametri `int *p` ricevono la coppia dal chiamante.
3. **Uninit sugli elementi:** si mantiene la bitmap (`LocalInitMap`) con un controllo di bit in lettura; si elimina solo dove un ciclo di riempimento la rende dimostrabile.
4. **Diagnostici:** tabella `pc → (riga, colonna, nome)` per errori OOB e uninit coerenti con i `.expect`.
5. **Limiti rigidi da sollevare:** `IR_LIN_REGS=24`, `FNCODE_SLOTS=8`, `IR_LIN_STMT_MAX=2048`, `GlobTab[32]`, `IdxTab[32]`. Verificare cosa succede oltre questi numeri.

## Verifica

- Suite 742/0 con `EXEC=vm`, stessi messaggi (test OOB e uninit esistenti inclusi).
- Fuzz differenziale albero/VM su programmi con array e puntatori (`gen_random_c`, `fuzz4`).
- Build ASan eseguita a ogni consegna.
- Prestazioni con `BABYC_NO_KERNEL=1` su `isort_v2`, `array_sum`, `sieve`, `dfs_rec`, `matmul_small`: **VM ≥ 2× l'albero**. Si riporta anche il rapporto con Python.
- Report sempre con tre colonne: albero, VM con controlli, VM senza controlli (per isolare il costo).

## Rischi

- Provenienza dei puntatori: coppie di registri, cast, aritmetica.
- Array 2D.
- Restringimento dei tipi negli store.
- `list_walk` (strutture a catena) resta una fetta ancora successiva.

## Da decidere insieme

- Il perimetro include `char` array con `printf("%s")` (stringhe locali) o resta per la fetta successiva?
- Soglia "VM ≥ 2× albero" sulla fetta array: va bene o ne preferite un'altra?
- Chi guida il passo 0, che blocca tutto il resto?
