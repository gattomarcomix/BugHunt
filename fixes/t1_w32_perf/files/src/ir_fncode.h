/* ir_fncode.h — FnCode multi-slot cache (linear body per function).
 *
 * PERF POLICY: access FnCodeSlot fields DIRECTLY (S->N, S->Code[i], …).
 * Do NOT add non-inline getters/setters for hot fields. A past refactor
 * introduced per-instruction accessors and caused severe slowdowns.
 * Cross-TU helpers are OK only on cold paths (compile / install once).
 */
#ifndef BABYC_IR_FNCODE_H
#define BABYC_IR_FNCODE_H

#include <stddef.h>
#include "interpreter.h"
#include "ir_runtime.h"
#include "ir_lin.h"

/* §2 FnCode multi-slot cache: each integer-eligible function keeps its own
 * lowered code + Call/Glob side tables so main→add1 does not clobber main. */
#define FNCODE_SLOTS 8
typedef struct FnCodeSlot {
    IrStmt *Body;
    int N;
    signed char Elig; /* -1 unknown, 0 no, 1 yes */
    signed char MonoElig; /* -1 unk, 0 no, 1 IrMono shape */
    signed char MemoElig; /* -1 unk, 0 no, 1 pure memo */
    signed char PtrIndexedElig; /* -1 unk, 0 tree fallback, 1 direct p[i] safe */
    signed char NestedLightSafe; /* 0 unknown, 1 safe, -1 unsafe: callee may use the light path at IrCallDepth>0 */
    IrLinInst Code[IR_LIN_STMT_MAX];
    int CallN;
    struct Value *CallFnTab[IR_LIN_CALL_SLOTS];
    int CallIndirectSlot[IR_LIN_CALL_SLOTS];
    int CallIndirectReg[IR_LIN_CALL_SLOTS];
    int CallIndirectParam[IR_LIN_CALL_SLOTS];
    IrExpr *CallIndirectExpr[IR_LIN_CALL_SLOTS];
    int CallArgN[IR_LIN_CALL_SLOTS];
    int CallCharPos[IR_LIN_CALL_SLOTS];
    int CallArgReg[IR_LIN_CALL_SLOTS][IR_LIN_CALL_ARGS];
    unsigned char CallArgIsPtr[IR_LIN_CALL_SLOTS][IR_LIN_CALL_ARGS];
    unsigned char CallArgIsFp[IR_LIN_CALL_SLOTS][IR_LIN_CALL_ARGS];
    long long CallStrBits[IR_LIN_CALL_SLOTS][IR_LIN_CALL_ARGS];
    unsigned char CallStrOn[IR_LIN_CALL_SLOTS][IR_LIN_CALL_ARGS];
    int GlobN;
    IrExpr *GlobTab[32];
    struct Value *GlobVal[32];
    int IdxN;
    IrExpr *IdxTab[32];
    signed char IdxMetaSlot[32];
    unsigned char IdxMetaKind[32];
    unsigned char IdxMetaEs[32];
    int IdxMetaN[32];
    int MaxLocal;
    int HasKernel;
    int HasArrayKernel;
    int KernelN;
    IrStmt *KernelStmtTab[16];
    /* Per-slot Decl meta captured at compile (survives nested Fr pollution).
     * Applied to the live IrCallFrame on body entry. */
    unsigned char SlotIsPtr[IR_CALL_MAX_LOCALS];
    unsigned char SlotIsFptr[IR_CALL_MAX_LOCALS];
    struct ValueType *SlotFromType[IR_CALL_MAX_LOCALS];
    const char *SlotName[IR_CALL_MAX_LOCALS];
    struct Value *Func;
    /* Per-function wide immediates (Tok=2/3 Imm = slot into this table).
     * Global LinWideImm is only the *live* install; without a per-slot copy,
     * VM→VM calls see the last-compiled function's constants
     * (e.g. x % 2147483648 → 0 when the table was overwritten). */
    long long WideImm[IR_LIN_STMT_MAX];
    unsigned char WideOn[IR_LIN_STMT_MAX];
    int WideMax;
} FnCodeSlot;


/* Live registry — direct pointers, not opaque handles. */
extern FnCodeSlot FnSlots[FNCODE_SLOTS];
extern int FnSlotN;
extern FnCodeSlot *FnActive;     /* installed tables owner */
extern FnCodeSlot *CompileSlot; /* slot under FnCodeCompile */

FnCodeSlot *FnCodeFind(IrStmt *Body);
FnCodeSlot *FnCodeFindByFunc(struct Value *FV);

void FnCodeInstall(FnCodeSlot *S);
void FnCodeSnapshot(FnCodeSlot *S);
void LinTabsRestore(const FnCodeSlot *S);

#endif /* BABYC_IR_FNCODE_H */
