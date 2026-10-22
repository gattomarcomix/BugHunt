# BabyC: report di analisi della baseline `babyc-FULL-gidx-verified`

Data: 2026-10-07 · Ambiente: sandbox Linux, gcc 13 `-O2`, best of 3, `BABYC_NO_MEMO=1`.
I tempi assoluti dipendono dall'host: contano i rapporti.

## 1. Sintesi

- La VM per funzione (`FnCode`, finestra di registri, chiamate dentro la VM) ora **esiste e dà un guadagno reale**: 1,8–2,8× sull'albero con i controlli attivi.
- Il pacchetto **non è accettabile come baseline**: in modalità albero dava risultati sbagliati su 33 test e su `isort` (numeri diversi a ogni run). Cause trovate e corrette con 8 righe in `ir_fast.c`: la suite in modalità albero passa a **742/0** (con le righe di debug su stderr filtrate).
- Con `EXEC=vm` restano 26 test falliti. Per quasi tutti ho individuato la causa (sezione 4); una classe non è isolata.
- `VERIFY.txt` conferma solo `fibc` e due programmi di `s3`; la suite non risulta eseguita.

## 2. Prestazioni (VM vs albero, stdout identico dove indicato)

| caso | albero | VM | VM/albero | note |
|---|---|---|---|---|
| fib_ptr | 1,21 s | 0,44 s | 2,75× | Python 0,17 s: VM ancora ~2,6× più lenta |
| fib_global | 1,18 s | 0,47 s | 2,53× | |
| call_loop | 0,85 s | 0,46 s | 1,85× | sotto la soglia 2× |
| fibc | 0,099 s | 0,043 s | 2,28× | Python: VM ~1,8× più lenta |
| gcd / collatz | | | 2,7× / 1,8× | VM più veloce di Python (0,89× / 0,84×) |

Prima di questa baseline `EXEC=vm` aveva lo stesso tempo dell'albero (`lowered=0`).

## 3. Bug in modalità albero (`ir_fast.c`)

### 3.1 `fill` non inizializzato in `IrTryFastArrayFill`

- La patch ha aggiunto `else if (Rhs->Kind == IrExBinOp) ;` per far passare `i*k+c` al percorso "ibrido", valido solo per array **globali**.
- Per un array **locale** il flusso cade nel percorso generico, che gestisce solo costanti e `i`, e scrive `(int)fill` con `fill` mai assegnato.
- Il compilatore lo segnalava: `ir_fast.c:4288: 'fill' may be used uninitialized`.
- Minimo: `int a[5]; for(i=0;i<5;i++) a[i]=i*10;` produce `511 511 511 511 511`. Vale per `i+3`, `i*i` e per elementi `int`/`short`/`char` (con `long long` il percorso rifiuta per dimensione, quindi è sano).
- Effetti: `isort` casuale (atteso 29665), e i test 163, 177, 244, 334, 399, 516, 731, 732 e simili.

### 3.2 Gli ibridi trattano `LocalSlot < 0` come "globale"

- Un parametro array o puntatore ha `ParamSlot`, non `LocalSlot`: finisce in `IrGlobalVal` e dà `'a' is undefined`.
- Effetti: 244, 263, 379, 452, 454, 583, 680.

### Fix (appendice A)

Guardie `ParamSlot < 0 && IrVarTableDefined(...)` nei due blocchi ibridi, e `if (Rhs->Kind == IrExBinOp) return 0;` prima del percorso generico.

Risultato: suite in modalità albero **742 passati / 0 falliti / 21 skip** (prima 709/33).

## 4. Bug con `EXEC=vm`

| classe | test | causa |
|---|---|---|
| SEGV su shift/mod/div unsigned | 070, 079, 594, 629, 721, 722 | `IrApplyBinOpRaw` chiama `IrApplyBinOp(..., Unsigned)` ma il 5° parametro è `IrExpr *E`: il flag 0/1 diventa un puntatore (ASan: `IrExprIsUnsigned(E=0x1)`). Con `Unsigned=0` si perdono segno e larghezza. Con gcc 14 è errore di compilazione. |
| riga/colonna errate nei diagnostici | 122, 123, 202, 219, 227, 417, 471 | `EmitLine` emette i marker di riga solo con `IrDebugOn()`: in produzione `ProgramFail` in `Step` usa la posizione stantia del parser (1:18) e un messaggio generico senza nome di variabile. |
| `XXX` al posto di una stringa | 074, 099, 142, 192, 371 | In `ir_lower.c` (~2165) `CallArgIsPtr` per un argomento `Ident` è deciso a compile time da `LocalIsPtr` del frame corrente; con il corpo compilato all'ingresso la variabile non è ancora assegnata, quindi `IsPtr=0`, `%s` riceve un intero e `clibrary.c` stampa `XXX`. I parametri funzionano, i locali no. Minimo: `char *s="hi"; printf("a=%d s=%s\n",42,s);`. |
| diagnostico di compilazione perso | 508 | `sizeof` su bitfield non controllato in VM (stampa 0). |
| **non isolata**: arresto silenzioso a metà | 059, 215, 231 | `231_ir_logic_sc.c` si ferma dopo il 2° blocco con rc=0; non riproducibile con 1–2 condizioni. Sospetto: legato al numero di call site verso la stessa funzione. L'ipotesi `Mode=Return` che "scappa" non ha trovato conferma. |

## 5. Altri problemi della consegna

- **Debug su stderr acceso nel prodotto**: `[babyc-cov]`, `[hy]`, `[fill-hy]`, `[call]` e `[babyc-vm]` escono sempre, nonostante i commenti "DBG disabled". La suite così com'è passa 13/742.
- `IrTryExecBodyVm` usato senza dichiarazione; `IrVmExecProd` è uno stub.
- `IrFnBodyIntegerEligible` è il gate del perimetro VM.
- Il documento `FIX_IMPLICIT_AND_SHAPE_FP` dice che le shape fast-path sono spente di default; nel codice sono accese (`BABYC_NO_SHAPE_FP`).
- Il flag `BABYC_NO_KERNEL` ora esiste e spegne tutti i kernel di loop.

## 6. Raccomandazioni

1. Applicare il fix dell'appendice A e rieseguire la suite senza filtro.
2. Togliere le `fprintf(stderr)` di debug e rendere la suite il gate di ogni consegna.
3. Compilare con `-Werror=int-conversion -Werror=maybe-uninitialized`.
4. Correggere `IrApplyBinOpRaw` (gestire segno e larghezza senza passare per un `IrExpr *`).
5. Emettere i marker di riga nella VM di produzione (o una tabella `pc → riga`) e il nome della variabile negli errori uninit.
6. Decidere `IsPtr` dai tipi dichiarati, non dallo stato runtime del frame.
7. Isolare la classe 231/059/215 prima di accendere `EXEC=vm` di default.
8. Il gate "≥ 2× sull'albero" è soddisfatto su 4 casi su 5 (`call_loop` a 1,85×); la correttezza della VM resta il vincolo principale.

## Appendice A: patch (`ir_fast.c`)

```diff
@@ -3892,7 +3892,9 @@
     /* Hybrid: global array + local index/accum (FnCode) */
     if (IrCallDepth > 0 && BaseE->LocalSlot < 0 && Dest->LocalSlot >= 0
-        && IdxE->Right->LocalSlot >= 0)
+        && IdxE->Right->LocalSlot >= 0
+        && BaseE->ParamSlot < 0 && BaseE->Ident != NULL
+        && IrVarTableDefined(Parser->pc, BaseE->Ident))
     {
@@ -4092,7 +4094,8 @@
     /* Hybrid: global array + local index; Rhs = const | i | i*k | i*k+c | i+c */
-    if (IrCallDepth > 0 && BaseE->LocalSlot < 0 && IdxE->LocalSlot >= 0)
+    if (IrCallDepth > 0 && BaseE->LocalSlot < 0 && IdxE->LocalSlot >= 0
+        && BaseE->ParamSlot < 0 && IrVarTableDefined(Parser->pc, BaseE->Ident))
     {
@@ -4179,6 +4182,7 @@
         return 1;
     }
 
+    if (Rhs->Kind == IrExBinOp) return 0; /* FIX: generic path only handles const / i */
     if (IrCallDepth <= 0 || BaseE->LocalSlot < 0 || IdxE->LocalSlot < 0)
     {
```
