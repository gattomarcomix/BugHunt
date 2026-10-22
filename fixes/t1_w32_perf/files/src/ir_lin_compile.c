/* ir_lin_compile.c — extract compile-side lower from ir_lower.c */
#include "picoc.h"
#include "ir_lin_compile.h"
#include "ir_lin_tabs.h"
#include "ir_lin_ptr.h"
#include "ir_lin_light.h"
#include "ir_runtime.h"
#include "ir_lin.h"
#include "ir_storage.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern struct ParseState *LinParser;
extern int LinCompileActive;
extern int LowerDepth;
extern int LinExecDepth;
extern int LinSkippedCall;
extern long LinSkipCallCount;
extern int LinRetSeen;
extern int LinStepAbort;
extern int LinDiv0;
extern long long LinRetVal;
extern unsigned char CallSkip[128];
/* CompLocal* declared in ir_storage.h */
extern long long LinWideImm[IR_LIN_STMT_MAX];
extern unsigned char LinWideOn[IR_LIN_STMT_MAX];
extern int LinWideMax;
extern int GlobSaved;
extern unsigned GlobDirty;

int AllocReg(int *Next);
int Emit(IrLinInst *Code, int Cap, int *N, IrLinInst In);
int EmitLine(IrLinInst *Code, int Cap, int *N, int Line);
int LowerRec(IrExpr *E, IrLinInst *Code, int Cap, int *N, int *Next, int *Dest);
void CompileStampName(int slot, const char *name);
void CompileStampPtrSlot(int slot, struct ValueType *FT, int is_fptr);
void IdxMetaPush(IrExpr *E);
void IrLiveMarkInit(IrCallFrame *Fr, int slot);
int IrKernelStmtScalarOnly(IrStmt *S, int depth);
int IrKernelFpEnabledFor(IrStmt *S);
int IrShapeFpEnabled(void);
int IrExecIsVmProd(void);
int IrVmStrictOn(void);
int IrFnBodyIntegerEligible(IrStmt *Body);
int IrLinPeephole(IrLinInst *Code, int N);
void IrVmProdNotice(void);
int IrVmProfileOn(void);
struct Value *IrGlobalIdentSoft(struct ParseState *Parser, const char *name);

typedef struct LinLoop {
    int CanCont;
    int Brk[12];
    int NBrk;
    int ContPc;
    int Conts[12];
    int NCont;
} LinLoop;
static LinLoop Loops[8];
int LoopSp;

typedef struct LinLab {
    const char *Name;
    int Pc;
    int Patch[8];
    int NPatch;
} LinLab;
LinLab Labs[16];
int NLab;

/* Compiler frame implementation (nested FnCodeCompile / IrLowerStmt). */
typedef struct CompileFrame {
    FnCodeSlot Tabs;
    long long WideImm[IR_LIN_STMT_MAX];
    unsigned char WideOn[IR_LIN_STMT_MAX];
    int WideMaxSav;
    LinLoop LoopsSav[8];
    int LoopSpSav;
    LinLab LabsSav[16];
    int NLabSav;
    unsigned char CallSkipSav[128];
    int LinSkippedCallSav;
    int LinRetSeenSav;
    long long LinRetValSav;
    struct ParseState *LinParserSav;
    /* Nested FnCodeCompile zeros CompLocal*; save/restore parent dims/types. */
    unsigned char CompNDims[IR_CALL_MAX_LOCALS];
    int CompDims[IR_CALL_MAX_LOCALS][IR_MAX_ARRAY_DIMS];
    int CompLeafEs[IR_CALL_MAX_LOCALS];
    struct ValueType *CompFromType[IR_CALL_MAX_LOCALS];
    int Valid;
} CompileFrame;

#define COMPILE_FRAME_MAX 8
static CompileFrame CompileFrames[COMPILE_FRAME_MAX];
static int CompileFrameSp;

int CompileFramePush(void)
{
    CompileFrame *F;
    int wm;
    if (CompileFrameSp >= COMPILE_FRAME_MAX)
        return 0;
    F = &CompileFrames[CompileFrameSp++];
    FnCodeSnapshot(&F->Tabs);
    wm = LinWideMax;
    if (wm > IR_LIN_STMT_MAX) wm = IR_LIN_STMT_MAX;
    F->WideMaxSav = wm;
    if (wm > 0)
    {
        memcpy(F->WideImm, LinWideImm, (size_t)wm * sizeof(long long));
        memcpy(F->WideOn, LinWideOn, (size_t)wm);
    }
    memcpy(F->LoopsSav, Loops, sizeof(Loops));
    F->LoopSpSav = LoopSp;
    memcpy(F->LabsSav, Labs, sizeof(Labs));
    F->NLabSav = NLab;
    memcpy(F->CallSkipSav, CallSkip, sizeof(CallSkip));
    F->LinSkippedCallSav = LinSkippedCall;
    F->LinRetSeenSav = LinRetSeen;
    F->LinRetValSav = LinRetVal;
    F->LinParserSav = LinParser;
    memcpy(F->CompNDims, CompLocalNDims, sizeof(CompLocalNDims));
    memcpy(F->CompDims, CompLocalDims, sizeof(CompLocalDims));
    memcpy(F->CompLeafEs, CompLocalLeafEs, sizeof(CompLocalLeafEs));
    memcpy(F->CompFromType, CompLocalFromType, sizeof(CompLocalFromType));
    F->Valid = 1;
    return 1;
}

void CompileFramePop(void)
{
    CompileFrame *F;
    int wm;
    if (CompileFrameSp <= 0)
        return;
    F = &CompileFrames[--CompileFrameSp];
    if (!F->Valid)
        return;
    LinTabsRestore(&F->Tabs);
    wm = F->WideMaxSav;
    if (wm > IR_LIN_STMT_MAX) wm = IR_LIN_STMT_MAX;
    if (wm > 0)
    {
        memcpy(LinWideImm, F->WideImm, (size_t)wm * sizeof(long long));
        memcpy(LinWideOn, F->WideOn, (size_t)wm);
    }
    LinWideMax = wm;
    memcpy(Loops, F->LoopsSav, sizeof(Loops));
    LoopSp = F->LoopSpSav;
    memcpy(Labs, F->LabsSav, sizeof(Labs));
    NLab = F->NLabSav;
    memcpy(CallSkip, F->CallSkipSav, sizeof(CallSkip));
    LinSkippedCall = F->LinSkippedCallSav;
    LinRetSeen = F->LinRetSeenSav;
    LinRetVal = F->LinRetValSav;
    LinParser = F->LinParserSav;
    memcpy(CompLocalNDims, F->CompNDims, sizeof(CompLocalNDims));
    memcpy(CompLocalDims, F->CompDims, sizeof(CompLocalDims));
    memcpy(CompLocalLeafEs, F->CompLeafEs, sizeof(CompLocalLeafEs));
    memcpy(CompLocalFromType, F->CompFromType, sizeof(CompLocalFromType));
    F->Valid = 0;
}


/* KERNEL_FOR gate: scalar locals/params only — no Index/global/Call.
 * IrKernelStmtOk allows Index (isort/adj); those stay on ARRAY_ACCUM/INSERT. */
static int IrKernelExprScalarOnly(IrExpr *E, int depth)
{
    if (E == NULL || depth > 24) return 0;
    switch (E->Kind)
    {
        case IrExConst: return 1;
        case IrExIdent:
            return (E->LocalSlot >= 0 || E->ParamSlot >= 0);
        case IrExBinOp:
            /* Shifts need unsigned/wide flags; kernel tree can miss param ushort. */
            if (E->Op == TokenShiftLeft || E->Op == TokenShiftRight)
                return 0;
            return IrKernelExprScalarOnly(E->Left, depth + 1)
                && IrKernelExprScalarOnly(E->Right, depth + 1);
        case IrExAssign:
            if (E->Op == TokenShiftLeftAssign || E->Op == TokenShiftRightAssign
                || E->Op == TokenArithmeticExorAssign
                || E->Op == TokenArithmeticAndAssign
                || E->Op == TokenArithmeticOrAssign)
                return 0;
            return IrKernelExprScalarOnly(E->Left, depth + 1)
                && IrKernelExprScalarOnly(E->Right, depth + 1);
        case IrExUnary:
            if (E->Op == TokenAmpersand || E->Op == TokenAsterisk)
                return 0;
            if (E->Op == TokenIncrement || E->Op == TokenDecrement)
                return 0;
            return IrKernelExprScalarOnly(E->Left, depth + 1);
        case IrExTernary:
            if (E->Args == NULL || E->ArgCount < 1) return 0;
            return IrKernelExprScalarOnly(E->Left, depth + 1)
                && IrKernelExprScalarOnly(E->Right, depth + 1)
                && IrKernelExprScalarOnly(E->Args[0], depth + 1);
        default:
            return 0; /* Index/Call/Member/Cast refused */
    }
}

int IrKernelStmtScalarOnly(IrStmt *S, int depth)
{
    int i;
    if (S == NULL || depth > 16) return 0;
    switch (S->Kind)
    {
        case IrStExpr:
            return S->Expr != NULL && IrKernelExprScalarOnly(S->Expr, 0);
        case IrStBlock:
            for (i = 0; i < S->StmtCount; i++)
                if (!IrKernelStmtScalarOnly(S->Stmts[i], depth + 1))
                    return 0;
            return 1;
        case IrStIf:
            if (S->Expr == NULL || !IrKernelExprScalarOnly(S->Expr, 0))
                return 0;
            if (S->ThenStmt && !IrKernelStmtScalarOnly(S->ThenStmt, depth + 1))
                return 0;
            if (S->ElseStmt && !IrKernelStmtScalarOnly(S->ElseStmt, depth + 1))
                return 0;
            return 1;
        case IrStBreak:
        case IrStContinue:
            return 1;
        default:
            return 0;
    }
}

static LinLab *LabGet(const char *Name)
{
    int i;
    if (Name == NULL) return NULL;
    for (i = 0; i < NLab; i++)
        if (Labs[i].Name == Name || (Labs[i].Name && strcmp(Labs[i].Name, Name) == 0))
            return &Labs[i];
    if (NLab >= 16) return NULL;
    Labs[NLab].Name = Name;
    Labs[NLab].Pc = -1;
    Labs[NLab].NPatch = 0;
    return &Labs[NLab++];
}

static int EmitBreak(IrLinInst *Code, int Cap, int *N)
{
    IrLinInst In;
    if (LoopSp <= 0) return 0;
    if (Loops[LoopSp - 1].NBrk >= 12) return 0;
    Loops[LoopSp - 1].Brk[Loops[LoopSp - 1].NBrk++] = *N;
    In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
    return Emit(Code, Cap, N, In);
}
static int EmitCont(IrLinInst *Code, int Cap, int *N)
{
    IrLinInst In;
    int sp;
    for (sp = LoopSp - 1; sp >= 0; sp--)
        if (Loops[sp].CanCont) break;
    if (sp < 0) return 0;
    In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
    if (Loops[sp].ContPc >= 0)
        In.Imm = Loops[sp].ContPc;
    else
    {
        if (Loops[sp].NCont >= 12) return 0;
        Loops[sp].Conts[Loops[sp].NCont++] = *N;
    }
    return Emit(Code, Cap, N, In);
}
static void Patch(IrLinInst *Code, int *List, int N, int Pc)
{
    int i;
    for (i = 0; i < N; i++)
        Code[List[i]].Imm = Pc;
}
static void PushLoop(int CanCont)
{
    LinLoop *L = &Loops[LoopSp++];
    L->CanCont = CanCont;
    L->NBrk = 0;
    L->NCont = 0;
    L->ContPc = -1;
}
static void SealCont(IrLinInst *Code, int Pc)
{
    LinLoop *L = &Loops[LoopSp - 1];
    L->ContPc = Pc;
    Patch(Code, L->Conts, L->NCont, Pc);
}
static void SealBrk(IrLinInst *Code, int Pc)
{
    Patch(Code, Loops[LoopSp - 1].Brk, Loops[LoopSp - 1].NBrk, Pc);
    LoopSp--;
}

static int LowerStmtRec(IrStmt *S, IrLinInst *Code, int Cap, int *N, int *Next)
{
    IrLinInst In;
    int dest, i, br, jmp, head, end;
    if (S == NULL)
        return 1;
    /* Regs die at stmt boundary; locals hold the values. */
    *Next = 0;
    if (S->Kind == IrStCase || S->Kind == IrStDefault)
        return 1;
    /* Per-statement LINE for F7 debug (only when BABYC_DEBUG). Skip pure meta. */
    if (S->Kind != IrStLabel && S->Kind != IrStBlock
        && S->SourceLine > 0
        && !EmitLine(Code, Cap, N, S->SourceLine))
        return 0;
    if (S->Kind == IrStLabel)
    {
        LinLab *L = LabGet(S->DeclIdent);
        if (L == NULL || L->Pc >= 0) return 0;
        L->Pc = *N;
        Patch(Code, L->Patch, L->NPatch, *N);
        return 1;
    }
    if (S->Kind == IrStGoto)
    {
        LinLab *L = LabGet(S->DeclIdent);
        if (L == NULL) return 0;
        if (L->Pc >= 0)
        {
            In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = L->Pc;
            return Emit(Code, Cap, N, In);
        }
        if (L->NPatch >= 8) return 0;
        L->Patch[L->NPatch++] = *N;
        In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
        return Emit(Code, Cap, N, In);
    }
    if (S->Kind == IrStDecl)
    {
        /* Runtime-sized arrays need the tree executor's per-frame allocation
         * and dynamic sizeof metadata. Until compact bytecode has a matching
         * VLA opcode, reject this body rather than treating the bound as a
         * pointer value and passing an invalid address to host intrinsics. */
        if (S->DeclArraySize == -2)
            return 0;
        /* Name for uninit diagnostics: survive compile→run frame switch. */
        if (S->DeclIdent != NULL && S->LocalSlot >= 0 && S->LocalSlot < IR_CALL_MAX_LOCALS)
            CompileStampName(S->LocalSlot, S->DeclIdent);
        /* Stamp Decl meta (CompLocal + FnSlot incl. SlotIsFptr) before any
         * branch so Assign/Call later in this body see fptr flags
         * (fp = tab[i]; fp(10) needs Tok=5 store + indirect call). */
        if (S->LocalSlot >= 0 && S->LocalSlot < IR_CALL_MAX_LOCALS
            && (S->DeclFromType != NULL || S->DeclIsFptr || S->DeclIsPtr))
            IrStorageDescStampDecl(S->LocalSlot, S);
        /* Static struct/union BSS: IR_K_DECL_STATIC_STU. */
        if (S->DeclIsStatic && !S->DeclIsFptr && !S->DeclIsPtr
            && S->DeclArraySize <= 1
            && S->DeclFromType != NULL
            && (S->DeclFromType->Base == TypeStruct
                || S->DeclFromType->Base == TypeUnion)
            && S->DeclInitCount <= 0 && S->DeclInitVals == NULL
            && S->LocalSlot >= 0 && KernelN < 16
            && S->DeclIdent != NULL)
        {
            IrLinInst Kin;
            size_t nbytes = S->DeclFromType->Sizeof > 0
                ? (size_t)S->DeclFromType->Sizeof : 0;
            if (nbytes == 0 || nbytes > 4096)
                return 0;
            Kin.Op = IrLinKernel;
            Kin.Dest = (unsigned char)S->LocalSlot;
            Kin.A = 0; Kin.B = 0;
            Kin.Tok = IR_K_DECL_STATIC_STU;
            Kin.Imm = KernelN;
            KernelStmtTab[KernelN] = S;
            KernelN++;
            if (!Emit(Code, Cap, N, Kin)) return 0;
            return 1;
        }
        /* Static scalar-element array: IR_K_DECL_STATIC_ARR (BSS persist blob).
         * DeclIsPtr may be set (LocalVal = blob addr) — still an array Decl. */
        if (S->DeclIsStatic && !S->DeclIsFptr
            && S->DeclArraySize > 0 && !S->DeclElemIsPtr
            && S->LocalSlot >= 0 && KernelN < 16
            && S->DeclIdent != NULL)
        {
            IrLinInst Kin;
            int es = S->DeclElemSize > 0 ? S->DeclElemSize : (int)sizeof(int);
            if (es != 1 && es != 2 && es != 4 && es != 8)
                es = (int)sizeof(int);
            Kin.Op = IrLinKernel;
            Kin.Dest = (unsigned char)S->LocalSlot;
            Kin.A = (unsigned char)es;
            Kin.B = 0;
            Kin.Tok = IR_K_DECL_STATIC_ARR;
            Kin.Imm = KernelN;
            KernelStmtTab[KernelN] = S;
            KernelN++;
            /* Stamp dims for Index lower (P0 descriptor). */
            if (S->LocalSlot < IR_CALL_MAX_LOCALS)
                IrStorageDescStampDeclEs(S->LocalSlot, S, es);
            if (!Emit(Code, Cap, N, Kin)) return 0;
            return 1;
        }
        /* Scalar static local: IR_K_DECL_STATIC loads/creates persist entry. */
        if (S->DeclIsStatic && !S->DeclIsPtr && !S->DeclIsFptr
            && S->DeclArraySize <= 0 && S->LocalSlot >= 0 && KernelN < 16
            && S->DeclIdent != NULL)
        {
            IrLinInst Kin;
            Kin.Op = IrLinKernel;
            Kin.Dest = (unsigned char)S->LocalSlot;
            Kin.A = 0; Kin.B = 0;
            Kin.Tok = IR_K_DECL_STATIC;
            Kin.Imm = KernelN;
            KernelStmtTab[KernelN] = S;
            KernelN++;
            if (!Emit(Code, Cap, N, Kin)) return 0;
            return 1;
        }
        /* Array Decl: under EXEC=vm prod, run tree Decl once (alloc blob +
         * brace-init) then continue lowering the rest of the body so Index
         * / loops stay on bytecode. Under both/shadow keep skip so tree
         * owns the Decl (no double-alloc). Ptr/fptr Decl still skip. */
        /* Value array T a[N] (DeclArraySize>0, !DeclElemIsPtr, !DeclIsFptr):
         * under EXEC=vm run tree Decl once (alloc+init), then continue.
         * DeclIsPtr is often also set (LocalVal holds blob addr) — ignore it. */
        /* Array of struct: DeclFromType is the element struct type with
         * DeclArraySize>1 (ir_compile convention). Scalar struct uses
         * DeclArraySize<=1 → DECL_STRUCT. */
        /* int *a[N] or int (*tab[N])(...): array of pointer / fptr words. */
        if (S->DeclArraySize > 0 && (S->DeclElemIsPtr || S->DeclIsFptr)
            && IrExecIsVmProd() && IrVmStrictOn() && LinParser != NULL
            && S->LocalSlot >= 0 && KernelN < 16)
        {
            IrLinInst Kin;
            int es = (int)sizeof(void *);
            long long nbytes = (long long)S->DeclArraySize * (long long)es;
            if (S->DeclArraySize > 1024 || nbytes > 8192)
                return 0;
            /* Brace-init: only pure zero-fill (int *a[N]={0};). */
            if (S->DeclInitExprs != NULL)
                return 0;
            if (S->DeclInitCount > 0 || S->DeclInitVals != NULL)
            {
                int zi;
                if (S->DeclInitVals == NULL)
                    return 0;
                for (zi = 0; zi < S->DeclInitCount; zi++)
                {
                    if (S->DeclInitVals[zi] != 0)
                        return 0; /* non-zero residual */
                }
                /* zero-fill: calloc already zeros; fall through */
            }
            /* P0: array-of-ptr / fptr-array layout via descriptor (incl. FnSlot). */
            if (S->LocalSlot >= 0 && S->LocalSlot < IR_CALL_MAX_LOCALS)
                IrStorageDescStampDeclEs(S->LocalSlot, S, (int)sizeof(void *));
            KernelStmtTab[KernelN] = S;
            Kin.Op = IrLinKernel;
            Kin.Dest = 0;
            Kin.A = (unsigned char)es;
            Kin.B = 0;
            Kin.Tok = IR_K_DECL_ARRAY;
            Kin.Imm = KernelN;
            KernelN++;
            if (!Emit(Code, Cap, N, Kin))
            {
                KernelN--;
                return 0;
            }
            return 1;
        }
        if (S->DeclArraySize > 1 && !S->DeclElemIsPtr && !S->DeclIsFptr
            && IrExecIsVmProd() && LinParser != NULL)
        {
            /* Linear-safe Decl for int-scale arrays (ARRAY_ACCUM/isort).
             * Scalar struct/union (ArraySize==1 convention) → DECL_STRUCT below.
             * Resolve elem size: DeclElemSize, else array FromType, else int.
             * char[] refused → tree. */
            IrLinInst Kin;
            int es = S->DeclElemSize > 0 ? S->DeclElemSize : 0;
            if (es <= 0 && S->DeclFromType != NULL
                && S->DeclFromType->Base == TypeArray
                && S->DeclFromType->FromType != NULL
                && S->DeclFromType->FromType->Sizeof > 0)
                es = S->DeclFromType->FromType->Sizeof;
            if (es <= 0)
                es = IrResolveDeclElemSize(0, S->DeclFromType);
            if (S->LocalSlot < 0 || KernelN >= 16)
                return 0;
            if (es <= 0 || es > 255)
                return 0;
            if (S->DeclArraySize > 1024
                || (long long)S->DeclArraySize * (long long)es > 4096)
                return 0;
            if (S->DeclArraySize <= 0 || S->DeclArraySize > 16 * 1024 * 1024)
                return 0;
            /* Stash resolved es in Kin.A (1..255) for Step — avoids 0→1 bug. */
            if (es > 255)
                es = (int)sizeof(int);
            KernelStmtTab[KernelN] = S;
            Kin.Op = IrLinKernel;
            Kin.Dest = 0;
            Kin.A = (unsigned char)es;
            Kin.B = 0;
            Kin.Tok = IR_K_DECL_ARRAY;
            Kin.Imm = KernelN;
            KernelN++;
            /* Stamp compile-time dims for multidim Index lower (P0 descriptor). */
            if (S->LocalSlot >= 0 && S->LocalSlot < IR_CALL_MAX_LOCALS)
                IrStorageDescStampDeclEs(S->LocalSlot, S, es);
            if (!Emit(Code, Cap, N, Kin))
            {
                KernelN--;
                return 0;
            }
            return 1;
        }
        /* EXEC=vm: scalar pointer Decl with lowerable init (malloc/cast/ident).
         * Store bits to LocalVal + mark LocalIsPtr so p->m Load/Store work.
         * No LinSkippedCall — body can stay on bytecode. */
        /* EXEC=vm: any scalar pointer Decl (int, char, Node, ...). */
        /* Local struct/union value: owned blob for s.m under EXEC=vm. */
        /* Local struct/union value: owned blob for s.m under EXEC=vm.
         * Brace-init OK when DeclInitVals is flat int list (no nested exprs). */
        if (S->DeclFromType != NULL
            && (S->DeclFromType->Base == TypeStruct || S->DeclFromType->Base == TypeUnion)
            && !S->DeclIsPtr && S->DeclArraySize <= 1 && !S->DeclIsFptr
            && S->LocalSlot >= 0 && KernelN < 16
            && S->DeclInitExprs == NULL)
        {
            IrLinInst Kin;
            size_t nbytes = S->DeclFromType->Sizeof > 0
                ? (size_t)S->DeclFromType->Sizeof : 1;
            /* Bitfield members: blob is still Sizeof bytes. Load/StoreIndex Tok=1
             * stamps BitWidth and uses IrBitExtract/Insert in Step — same as
             * array-of-bitfield-struct path. Do not refuse the Decl. */
            if (nbytes > 16 * 1024 * 1024)
                return 0;
            /* Brace-init: vm_strict only (hybrid tree path more accurate on skip/uninit). */
            if (S->DeclInitCount > 0 || S->DeclInitVals != NULL)
            {
                if (!IrVmStrictOn())
                    return 0;
                if (nbytes % sizeof(int) != 0)
                    return 0;
                if (S->DeclInitCount > (int)(nbytes / sizeof(int)) + 8)
                    return 0;
            }
            Kin.Op = IrLinKernel;
            Kin.Dest = (unsigned char)S->LocalSlot;
            Kin.A = 0; Kin.B = 0;
            Kin.Tok = IR_K_DECL_STRUCT;
            Kin.Imm = KernelN;
            KernelStmtTab[KernelN] = S;
            KernelN++;
            /* P0: struct/union FromType stamp via descriptor. */
            if (S->LocalSlot >= 0 && S->LocalSlot < IR_CALL_MAX_LOCALS)
                IrStorageDescStampDecl(S->LocalSlot, S);
            if (!Emit(Code, Cap, N, Kin)) return 0;
            /* struct S t = make(...): DECL then by-value copy from Call RetBuf.
             * Without this, DECL zeros the blob and the Call init is dropped (514/647).
             * Sizeof>4 excluded 4-byte bitfield structs (514) — use Sizeof>0. */
            if (S->Expr != NULL && S->Expr->Kind == IrExCall
                && S->DeclFromType != NULL
                && S->DeclFromType->Sizeof > 0
                && S->DeclFromType->Sizeof <= 512)
            {
                int src_reg = -1, dst_reg = -1;
                int nbytes = (int)S->DeclFromType->Sizeof;
                if (!LowerRec(S->Expr, Code, Cap, N, Next, &src_reg))
                    return 0;
                dst_reg = AllocReg(Next);
                if (dst_reg < 0) return 0;
                In.Op = IrLinImm; In.Dest = (unsigned char)dst_reg;
                In.A = In.B = 0; In.Tok = 4; In.Imm = S->LocalSlot;
                if (!Emit(Code, Cap, N, In)) return 0;
                In.Op = IrLinStoreIndex; In.Dest = 0;
                In.A = (unsigned char)src_reg;
                In.B = (unsigned char)dst_reg;
                In.Tok = 3;
                In.Imm = nbytes;
                if (!Emit(Code, Cap, N, In)) return 0;
            }
            else if (S->Expr != NULL)
                return 0; /* non-Call init residual */
            return 1;
        }
        if (S->DeclFromType != NULL
            && (S->DeclFromType->Base == TypeStruct || S->DeclFromType->Base == TypeUnion)
            && !S->DeclIsPtr && S->DeclArraySize <= 1 && !S->DeclIsFptr)
            return 0; /* complex brace residual */
        if (S->DeclIsPtr && !S->DeclIsFptr && S->DeclArraySize <= 0
            && IrExecIsVmProd() && S->LocalSlot >= 0)
        {
            int rd = -1;
            IrCallFrame *Fr;
            int ok_init = (S->Expr == NULL);
            if (S->Expr != NULL)
                ok_init = LowerRec(S->Expr, Code, Cap, N, Next, &rd);
            else
            {
                /* bare Node *p; → null */
                rd = AllocReg(Next);
                if (rd < 0) ok_init = 0;
                else
                {
                    In.Op = IrLinImm; In.Dest = (unsigned char)rd;
                    In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
                    ok_init = Emit(Code, Cap, N, In);
                }
            }
            if (ok_init && rd >= 0)
            {
                /* P0: CompLocal + FnSlot + Frame (ptr, init, elem size, skip owned) */
                Fr = (IrCallDepth > 0) ? IrCallStack[IrCallDepth - 1] : NULL;
                IrStorageDescStampDeclFull(S->LocalSlot, S, Fr,
                    IR_STAMP_FRAME_PTR | IR_STAMP_FRAME_INIT
                    | IR_STAMP_FRAME_SKIP_OWNED | IR_STAMP_FRAME_ELEMSIZE
                    | IR_STAMP_FRAME_NAME | IR_STAMP_FRAME_TYPE);
                In.Op = IrLinStoreLocal; In.Dest = 0;
                In.A = (unsigned char)rd; In.B = 0; In.Tok = 0;
                In.Imm = S->LocalSlot;
                return Emit(Code, Cap, N, In);
            }
            /* init not lowerable: fall through to skip */
        }
        if (S->DeclIsFptr && S->LocalSlot >= 0 && S->DeclArraySize <= 0)
        {
            /* Scalar int (*fp)(...) = add; NOT int (*tab[N])(...). */
            int rd = -1;
            {
                IrCallFrame *FrFp = (IrCallDepth > 0) ? IrCallStack[IrCallDepth - 1] : NULL;
                IrStorageDescStampDeclFull(S->LocalSlot, S, FrFp,
                    IR_STAMP_FRAME_FPTR | IR_STAMP_FRAME_NAME
                    | IR_STAMP_FRAME_TYPE);
            }
            if (S->Expr != NULL)
            {
                IrExpr *RX = S->Expr;
                if (RX->Kind == IrExUnary && RX->Op == TokenAmpersand && RX->Left != NULL)
                    RX = RX->Left; /* &fred */
                if (RX->Kind == IrExIdent && RX->Ident != NULL && LinParser != NULL)
                {
                    struct Value *TV = NULL;
                    TV = IrGlobalIdentSoft(LinParser, RX->Ident);
                    if (TV != NULL && TV->Typ != NULL && TV->Typ->Base == TypeFunction)
                    {
                        rd = AllocReg(Next);
                        if (rd >= 0)
                        {
                            IrLinInst Im;
                            Im.Op = IrLinImm; Im.Dest = (unsigned char)rd;
                            Im.A = Im.B = 0; Im.Tok = 5;
                            Im.Imm = (long long)(intptr_t)TV;
                            if (!Emit(Code, Cap, N, Im))
                                return 0;
                        }
                    }
                }
                if (rd < 0 && !LowerRec(S->Expr, Code, Cap, N, Next, &rd))
                    rd = -1;
            }
            else
            {
                rd = AllocReg(Next);
                if (rd >= 0)
                {
                    IrLinInst Im;
                    Im.Op = IrLinImm; Im.Dest = (unsigned char)rd;
                    Im.A = Im.B = 0; Im.Tok = 0; Im.Imm = 0;
                    if (!Emit(Code, Cap, N, Im)) return 0;
                }
            }
            if (rd >= 0)
            {
                IrLinInst St;
                St.Op = IrLinStoreLocal; St.Dest = 0;
                St.A = (unsigned char)rd; St.B = 0; St.Tok = 5; /* fptr slot */
                St.Imm = S->LocalSlot;
                return Emit(Code, Cap, N, St);
            }
            /* Fptr Decl not fully lowerable: keep body eligible, skip this Decl. */
            return 1;
        }
        if (S->DeclIsPtr || S->DeclArraySize > 0)
        {
            /* Pointer/array decl with a lowerable call init (malloc, ...):
             * lower the call and store the pointer bits. Only skip if the
             * call cannot be lowered. */
            if (S->Expr != NULL && S->Expr->Kind == IrExCall
                && S->LocalSlot >= 0)
            {
                int dest = 0;
                if (LowerRec(S->Expr, Code, Cap, N, Next, &dest))
                {
                    /* P0: ptr/array Decl with Call init — unified frame stamp */
                    {
                        IrCallFrame *FrCall = (IrCallDepth > 0)
                            ? IrCallStack[IrCallDepth - 1] : NULL;
                        int ff = IR_STAMP_FRAME_SKIP_OWNED | IR_STAMP_FRAME_INIT
                            | IR_STAMP_FRAME_NAME | IR_STAMP_FRAME_TYPE;
                        if (S->DeclIsFptr)
                            ff |= IR_STAMP_FRAME_FPTR;
                        else
                            ff |= IR_STAMP_FRAME_PTR;
                        IrStorageDescStampDeclFull(S->LocalSlot, S, FrCall, ff);
                    }
                    In.Op = IrLinStoreLocal; In.Dest = 0;
                    In.A = (unsigned char)dest; In.B = 0; In.Tok = 0;
                    In.Imm = S->LocalSlot;
                    return Emit(Code, Cap, N, In);
                }
            }
            if (S->LocalSlot >= 0 && S->LocalSlot < 128)
                CallSkip[S->LocalSlot] = 1;
            LinSkippedCall = 1; LinSkipCallCount++;
            return 1;
        }
        /* Enum constants compile as Decl with DeclInitVals, no Expr. */
        if (S->Expr == NULL && S->LocalSlot >= 0
            && S->DeclInitVals != NULL && S->DeclInitCount > 0)
        {
            int rd = AllocReg(Next);
            int ff;
            if (rd < 0) return 0;
            In.Op = IrLinImm; In.Dest = (unsigned char)rd;
            In.A = In.B = 0; In.Tok = 0;
            In.Imm = S->DeclInitVals[0];
            if (!Emit(Code, Cap, N, In)) return 0;
            /* Nested compile must not poison live FromType (790 fused uninit). */
            ff = IR_STAMP_FRAME_INIT | IR_STAMP_FRAME_NAME;
            if (!LinCompileActive || CompileFrameSp == 0)
                ff |= IR_STAMP_FRAME_TYPE;
            if (IrCallDepth > 0 && S->LocalSlot < IR_CALL_MAX_LOCALS)
                IrStorageDescStampDeclFull(S->LocalSlot, S,
                    IrCallStack[IrCallDepth - 1], ff);
            In.Op = IrLinStoreLocal; In.Dest = 0;
            In.A = (unsigned char)rd; In.B = 0; In.Tok = 0;
            In.Imm = S->LocalSlot;
            return Emit(Code, Cap, N, In);
        }
        if (S->Expr == NULL || S->LocalSlot < 0)
        {
            /* Bare int n; — stamp name/type so BoundIsVar / uninit work.
             * No INIT: slot stays uninit until first store. */
            if (S->LocalSlot >= 0 && IrCallDepth > 0
                && S->LocalSlot < IR_CALL_MAX_LOCALS)
            {
                int ff = IR_STAMP_FRAME_NAME;
                if (!LinCompileActive || CompileFrameSp == 0)
                    ff |= IR_STAMP_FRAME_TYPE;
                if (S->DeclIdent != NULL)
                    CompileStampName(S->LocalSlot, S->DeclIdent);
                IrStorageDescStampDeclFull(S->LocalSlot, S,
                    IrCallStack[IrCallDepth - 1], ff);
            }
            return 1;
        }
        /* Decl with Call init: try lower first (FnCode path). Only skip if
         * the call cannot be lowered. */
        if (S->Expr->Kind == IrExCall)
        {
            if (LowerRec(S->Expr, Code, Cap, N, Next, &dest))
            {
                In.Op = IrLinStoreLocal; In.Dest = 0;
                In.A = (unsigned char)dest; In.B = 0; In.Tok = 0;
                In.Imm = S->LocalSlot;
                return Emit(Code, Cap, N, In);
            }
            if (S->LocalSlot >= 0 && S->LocalSlot < 128) CallSkip[S->LocalSlot] = 1;
            LinSkippedCall = 1; LinSkipCallCount++;
            return 1;
        }
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
            return 0;
        /* Stamp DeclFromType so IrNarrowToLocalType works under pure VM.
         * INIT is runtime StoreLocal; do not mark init at compile time
         * (790 fused uninit). Nested compile skips TYPE to avoid poison. */
        if (IrCallDepth > 0 && S->LocalSlot >= 0 && S->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            int ff = IR_STAMP_FRAME_NAME | IR_STAMP_FRAME_INIT;
            if (!LinCompileActive || CompileFrameSp == 0)
                ff |= IR_STAMP_FRAME_TYPE;
            IrStorageDescStampDeclFull(S->LocalSlot, S,
                IrCallStack[IrCallDepth - 1], ff);
        }
        In.Op = IrLinStoreLocal; In.Dest = 0;
        In.A = (unsigned char)dest; In.B = 0;
        In.Tok = S->DeclIsConst ? 1 : 0;
        In.Imm = S->LocalSlot;
        return Emit(Code, Cap, N, In);
    }
    if (S->Kind == IrStExpr)
    {
        if (S->Expr == NULL)
            return 1;
        /* Dead identity x = x: no side effects, drop (general DCE). */
        if (S->Expr->Kind == IrExAssign
            && (S->Expr->Op == 0 || S->Expr->Op == TokenAssign)
            && S->Expr->Left != NULL && S->Expr->Left->Kind == IrExIdent
            && S->Expr->Right != NULL && S->Expr->Right->Kind == IrExIdent
            && S->Expr->Left->LocalSlot >= 0
            && S->Expr->Left->LocalSlot == S->Expr->Right->LocalSlot)
            return 1;
        /* User call: lower if possible; host/printf → explicit skip. */
        if (S->Expr->Kind == IrExCall)
        {
            if (LowerRec(S->Expr, Code, Cap, N, Next, &dest))
                return 1;
            if (getenv("BABYC_SKIP_TRACE"))
                fprintf(stderr, "[skip-call-stmt] %s args=%d\n",
                    S->Expr->Ident ? S->Expr->Ident :
                    (S->Expr->Left && S->Expr->Left->Ident ? S->Expr->Left->Ident : "?"),
                    S->Expr->ArgCount);
            LinSkippedCall = 1; LinSkipCallCount++;
            return 1;
        }
        if (S->Expr->Kind == IrExAssign && S->Expr->Right != NULL
            && S->Expr->Right->Kind == IrExCall
            && S->Expr->Left != NULL && S->Expr->Left->Kind == IrExIdent
            && S->Expr->Left->LocalSlot >= 0 && S->Expr->Left->LocalSlot < 128)
        {
            if (LowerRec(S->Expr, Code, Cap, N, Next, &dest))
            {
                In.Op = IrLinStoreLocal; In.Dest = 0;
                In.A = (unsigned char)dest; In.B = 0; In.Tok = 0;
                In.Imm = S->Expr->Left->LocalSlot;
                return Emit(Code, Cap, N, In);
            }
            CallSkip[S->Expr->Left->LocalSlot] = 1;
            LinSkippedCall = 1; LinSkipCallCount++;
            return 1;
        }
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
        {
            return 0;
        }
        if (S->Expr->Kind == IrExAssign && S->Expr->Left != NULL
            && S->Expr->Left->Kind == IrExIdent && S->Expr->Left->LocalSlot >= 0)
        {
            int ls = S->Expr->Left->LocalSlot;
            In.Op = IrLinStoreLocal; In.Dest = 0;
            In.A = (unsigned char)dest; In.B = 0; In.Tok = 0;
            In.Imm = ls;
            if (!Emit(Code, Cap, N, In)) return 0;
            if (IrCallDepth > 0 && ls < IR_CALL_MAX_LOCALS
                && (S->Expr->Left->TypeFlags & IR_TF_PTR))
            {
                IrStorageDesc Dp;
                IrStorageDescFromType(NULL, 1, 0, &Dp);
                IrStorageDescApplyFrame(IrCallStack[IrCallDepth - 1], ls, &Dp,
                    NULL, IR_STAMP_FRAME_PTR | IR_STAMP_FRAME_INIT);
            }
            return 1;
        }
        return 1;
    }
    if (S->Kind == IrStBlock)
    {
        for (i = 0; i < S->StmtCount; i++)
        {
            *Next = 0;
            if (!LowerStmtRec(S->Stmts[i], Code, Cap, N, Next))
                return 0;
        }
        return 1;
    }
    if (S->Kind == IrStIf && S->Expr != NULL)
    {
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
        {
            return 0;
        }
        br = *N;
        In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)dest; In.B = 0;
        In.Tok = 0; In.Imm = 0;
        if (!Emit(Code, Cap, N, In)) return 0;
        if (!LowerStmtRec(S->ThenStmt, Code, Cap, N, Next)) return 0;
        jmp = *N;
        In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
        if (!Emit(Code, Cap, N, In)) return 0;
        Code[br].Imm = *N;
        if (!LowerStmtRec(S->ElseStmt, Code, Cap, N, Next)) return 0;
        Code[jmp].Imm = *N;
        return 1;
    }
    if (S->Kind == IrStWhile && S->Expr != NULL)
    {
        /* Insert-shift shape: kernel (if enabled) or tight expanded (CSE a[j]). */
        if (IrExecIsVmProd() && IrCallDepth > 0 && IrShapeFpEnabled())
        {
            IrExpr *Cond = S->Expr, *L, *R, *Bound = NULL, *Cmp = NULL;
            IrStmt *Body = S->BodyStmt;
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            int jSlot = -1, aSlot = -1, keySlot = -1, body_n = 0, ok_shape = 0;
            int strict_gt = 1;
            if (Cond->Kind == IrExBinOp && Cond->Op == TokenLogicalAnd
                && Cond->Left && Cond->Right)
            {
                L = Cond->Left; R = Cond->Right;
                {
                    IrExpr *sides[2] = { L, R };
                    int si;
                    for (si = 0; si < 2; si++)
                    {
                        IrExpr *E = sides[si];
                        if (E->Kind == IrExBinOp && E->Op == TokenGreaterEqual
                            && E->Left && E->Left->Kind == IrExIdent
                            && E->Right && E->Right->Kind == IrExConst
                            && E->Right->ConstVal == 0)
                        { Bound = E; jSlot = E->Left->LocalSlot; break; }
                    }
                }
                Cmp = (Bound == L) ? R : (Bound == R) ? L : NULL;
                if (Bound && Cmp && Cmp->Kind == IrExBinOp
                    && (Cmp->Op == TokenGreaterThan || Cmp->Op == TokenGreaterEqual)
                    && Cmp->Left && Cmp->Left->Kind == IrExIndex
                    && Cmp->Right && Cmp->Right->Kind == IrExIdent)
                {
                    IrExpr *Ix = Cmp->Left;
                    strict_gt = (Cmp->Op == TokenGreaterThan);
                    keySlot = Cmp->Right->LocalSlot;
                    if (Ix->Left && Ix->Left->Kind == IrExIdent
                        && Ix->Right && Ix->Right->Kind == IrExIdent
                        && Ix->Right->LocalSlot == jSlot)
                        aSlot = Ix->Left->LocalSlot;
                }
                if (Body && Body->Kind == IrStBlock)
                    body_n = Body->StmtCount;
                else if (Body)
                    body_n = 1;
                if (jSlot >= 0 && aSlot >= 0 && keySlot >= 0
                    && body_n >= 2 && body_n <= 4
                    && aSlot < IR_CALL_MAX_LOCALS
                    && (Fr->LocalArraySize[aSlot] > 0 || IrExecIsVmProd()))
                    ok_shape = 1;
            }
            /* INSERT_SHIFT kernel only for pure 2-stmt body
             * (a[j+1]=a[j]; j--). Extra stmts (moves++) are not in the
             * kernel; emitting it caused runtime abort → tree fallback →
             * vm_strict fail. Tight expanded handles body_n 2..4. */
            if (ok_shape && body_n == 2 && IrKernelFpEnabledFor(S) && KernelN < 16
                && (Fr->LocalVal[aSlot] != 0 || IrExecIsVmProd()))
            {
                IrLinInst Kin;
                KernelStmtTab[KernelN] = S;
                Kin.Op = IrLinKernel; Kin.Dest = 0; Kin.A = 0; Kin.B = 0;
                Kin.Tok = IR_K_INSERT_SHIFT; Kin.Imm = KernelN;
                KernelN++;
                return Emit(Code, Cap, N, Kin);
            }
            /* Tight expanded: only pure 2-stmt shift body. body_n>2 (moves++)
             * falls through to generic while so every stmt is lowered. */
            if (ok_shape && body_n == 2 && LoopSp < 8 && IdxN < 30)
            {
                int rj, ra, rk, rc, rj1, br0, br1, head_pc, end_pc;
                IrExpr *IxLoad = NULL;
                /* Build synthetic Index expr for a[j] meta from Cmp */
                if (Cmp && Cmp->Left && Cmp->Left->Kind == IrExIndex)
                    IxLoad = Cmp->Left;
                PushLoop(1);
                head_pc = *N;
                /* rj = j */
                rj = AllocReg(Next);
                if (rj < 0) { LoopSp--; return 0; }
                In.Op = IrLinLoadLocal; In.Dest = (unsigned char)rj;
                In.A = In.B = 0; In.Tok = 0; In.Imm = jSlot;
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                /* if (!(j >= 0)) goto end  == BrLT j, 0 is wrong; use Imm 0 + Bin >= */
                {
                    int z = AllocReg(Next);
                    if (z < 0) { LoopSp--; return 0; }
                    In.Op = IrLinImm; In.Dest = (unsigned char)z;
                    In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
                    if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                    rc = AllocReg(Next);
                    if (rc < 0) { LoopSp--; return 0; }
                    In.Op = IrLinBin; In.Dest = (unsigned char)rc;
                    In.A = (unsigned char)rj; In.B = (unsigned char)z;
                    In.Tok = TokenGreaterEqual; In.Imm = 0;
                    if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                    br0 = *N;
                    In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)rc;
                    In.B = 0; In.Tok = 0; In.Imm = 0;
                    if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                }
                /* ra = a[j]  (single load — CSE) */
                if (IxLoad == NULL || IdxN >= 32) { LoopSp--; return 0; }
                ra = AllocReg(Next);
                if (ra < 0) { LoopSp--; return 0; }
                In.Op = IrLinLoadIndex; In.Dest = (unsigned char)ra;
                In.A = (unsigned char)rj; In.B = 0; In.Tok = 0; In.Imm = IdxN;
                IdxMetaPush(IxLoad);
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                /* rk = key */
                rk = AllocReg(Next);
                if (rk < 0) { LoopSp--; return 0; }
                In.Op = IrLinLoadLocal; In.Dest = (unsigned char)rk;
                In.A = In.B = 0; In.Tok = 0; In.Imm = keySlot;
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                /* rc = (a[j] > key) or >= */
                rc = AllocReg(Next);
                if (rc < 0) { LoopSp--; return 0; }
                In.Op = IrLinBin; In.Dest = (unsigned char)rc;
                In.A = (unsigned char)ra; In.B = (unsigned char)rk;
                In.Tok = strict_gt ? TokenGreaterThan : TokenGreaterEqual;
                In.Imm = 0;
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                br1 = *N;
                In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)rc;
                In.B = 0; In.Tok = 0; In.Imm = 0;
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                /* a[j+1] = ra */
                rj1 = AllocReg(Next);
                if (rj1 < 0) { LoopSp--; return 0; }
                In.Op = IrLinAddI; In.Dest = (unsigned char)rj1;
                In.A = (unsigned char)rj; In.B = 0; In.Tok = 0; In.Imm = 1;
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                if (IdxN >= 32) { LoopSp--; return 0; }
                In.Op = IrLinStoreIndex; In.Dest = 0;
                In.A = (unsigned char)ra; In.B = (unsigned char)rj1;
                In.Tok = 0; In.Imm = IdxN;
                IdxMetaPush(IxLoad); /* same array base meta */
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                /* j-- */
                {
                    int tmp = AllocReg(Next);
                    if (tmp < 0) { LoopSp--; return 0; }
                    In.Op = IrLinAddLocal; In.Dest = (unsigned char)tmp;
                    In.A = (unsigned char)jSlot; In.B = 0; In.Tok = 0; In.Imm = -1;
                    if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                }
                SealCont(Code, *N);
                In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = head_pc;
                if (!Emit(Code, Cap, N, In)) { LoopSp--; return 0; }
                end_pc = *N;
                Code[br0].Imm = end_pc;
                Code[br1].Imm = end_pc;
                SealBrk(Code, end_pc);
                return 1;
            }
        }
        /* Generic while lower. */
        if (LoopSp >= 8) return 0;
        PushLoop(1);
        head = *N;
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
        {
            return 0;
        }
        br = *N;
        In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)dest; In.B = 0;
        In.Tok = 0; In.Imm = 0;
        if (!Emit(Code, Cap, N, In)) return 0;
        if (!LowerStmtRec(S->BodyStmt, Code, Cap, N, Next)) return 0;
        SealCont(Code, *N);
        In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = head;
        if (!Emit(Code, Cap, N, In)) return 0;
        end = *N;
        Code[br].Imm = end;
        SealBrk(Code, end);
        return 1;
    }
    if (S->Kind == IrStFor)
    {
        int head, br, end;
        /* EXEC=vm: array-accum native kernel. */
        if (IrExecIsVmProd() && KernelN < 16 && IrKernelFpEnabledFor(S)
            && S->FastCond.Active && S->FastInc.Active
            && S->FastCond.VarName && S->FastInc.VarName
            && strcmp(S->FastCond.VarName, S->FastInc.VarName) == 0)
        {
            IrStmt *Body = S->BodyStmt;
            IrExpr *Add, *Dest, *IdxE = NULL;
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            int aSlot = -1, ok_shape = 0;
            if (Body && Body->Kind == IrStBlock && Body->StmtCount == 1)
                Body = Body->Stmts[0];
            if (Body && Body->Kind == IrStExpr && Body->Expr
                && Body->Expr->Kind == IrExAssign
                && Body->Expr->Left && Body->Expr->Right)
            {
                Dest = Body->Expr->Left;
                Add = Body->Expr->Right;
                if (Dest->Kind == IrExIdent && Add->Kind == IrExBinOp
                    && Add->Op == TokenPlus)
                {
                    if (Add->Left && Add->Left->Kind == IrExIndex
                        && Add->Right && Add->Right->Kind == IrExIdent
                        && Add->Right->LocalSlot == Dest->LocalSlot)
                        IdxE = Add->Left;
                    else if (Add->Right && Add->Right->Kind == IrExIndex
                             && Add->Left && Add->Left->Kind == IrExIdent
                             && Add->Left->LocalSlot == Dest->LocalSlot)
                        IdxE = Add->Right;
                    if (IdxE && IdxE->Left && IdxE->Left->Kind == IrExIdent
                        && IdxE->Right && IdxE->Right->Kind == IrExIdent
                        && IdxE->Right->Ident
                        && strcmp(IdxE->Right->Ident, S->FastInc.VarName) == 0)
                    {
                        aSlot = IdxE->Left->LocalSlot;
                        /* Param int* p[i]: ARRAY_ACCUM only knows LocalVal —
                         * expand LoadIndex (param path). Global/local OK. */
                        if (IdxE->Left->ParamSlot >= 0)
                            ok_shape = 0;
                        else if (aSlot >= 0 && aSlot < IR_CALL_MAX_LOCALS
                            && ((Fr->LocalArraySize[aSlot] > 0 && Fr->LocalVal[aSlot] != 0)
                                || (IrExecIsVmProd() && IdxE->Left->Ident != NULL)))
                            ok_shape = 1;
                        else if (aSlot < 0 && IdxE->Left->Ident != NULL
                                 && IdxE->Left->ParamSlot < 0)
                            ok_shape = 1; /* global */
                    }
                }
            }
            if (ok_shape)
            {
                IrLinInst Kin;
                if (S->InitStmt != NULL
                    && !LowerStmtRec(S->InitStmt, Code, Cap, N, Next))
                    ; /* fall through */
                else
                {
                    KernelStmtTab[KernelN] = S;
                    Kin.Op = IrLinKernel; Kin.Dest = Kin.A = Kin.B = 0;
                    Kin.Tok = IR_K_ARRAY_ACCUM; Kin.Imm = KernelN;
                    KernelN++;
                    if (Emit(Code, Cap, N, Kin))
                        return 1;
                    KernelN--;
                }
            }
        }
        /* EXEC=vm: array-fill native kernel. */
        if (IrExecIsVmProd() && !IrVmStrictOn() && KernelN < 16 && IrKernelFpEnabledFor(S)
            && S->FastCond.Active && S->FastInc.Active
            && S->FastCond.VarName && S->FastInc.VarName
            && strcmp(S->FastCond.VarName, S->FastInc.VarName) == 0)
        {
            IrStmt *Body = S->BodyStmt;
            IrExpr *Dest, *Rhs;
            int ok_shape = 0;
            if (Body && Body->Kind == IrStBlock && Body->StmtCount == 1)
                Body = Body->Stmts[0];
            if (Body && Body->Kind == IrStExpr && Body->Expr
                && Body->Expr->Kind == IrExAssign
                && Body->Expr->Left && Body->Expr->Right)
            {
                Dest = Body->Expr->Left;
                Rhs = Body->Expr->Right;
                if (Dest->Kind == IrExIndex && Dest->Left
                    && Dest->Left->Kind == IrExIdent
                    && Dest->Right && Dest->Right->Kind == IrExIdent
                    && Dest->Right->Ident
                    && strcmp(Dest->Right->Ident, S->FastInc.VarName) == 0
                    && (Rhs->Kind == IrExConst
                        || (Rhs->Kind == IrExIdent && Rhs->Ident
                            && strcmp(Rhs->Ident, S->FastInc.VarName) == 0)))
                    ok_shape = 1;
                /* Complex BinOp fills (isort a[i]=(i*k+c)%m) stay expanded LDX. */
            }
            if (ok_shape)
            {
                IrLinInst Kin;
                if (S->InitStmt != NULL
                    && !LowerStmtRec(S->InitStmt, Code, Cap, N, Next))
                    ; /* fall through */
                else
                {
                    KernelStmtTab[KernelN] = S;
                    Kin.Op = IrLinKernel; Kin.Dest = Kin.A = Kin.B = 0;
                    Kin.Tok = IR_K_ARRAY_FILL; Kin.Imm = KernelN;
                    KernelN++;
                    if (Emit(Code, Cap, N, Kin))
                        return 1;
                    KernelN--;
                }
            }
        }
        /* EXEC=vm: ptr-walk for (p=head; p; p=p->next) s+=p->v → IrLinKernel.
         * Body must be single acc += p->field (not printf).
         * Gate on IrKernelFpEnabledFor(S): under BABYC_NO_KERNEL IrTryFastPtrWalk
         * returns 0 and the return was ignored → silent no-op (list_walk → 0).
         * Fall through to generic for lower when kernels are off. */
        if (IrExecIsVmProd() && KernelN < 16 && IrCallDepth > 0
            && IrKernelFpEnabledFor(S))
        {
            IrExpr *Cond = S->Expr;
            IrStmt *Body = S->BodyStmt;
            IrExpr *BE;
            int ok_shape = 0;
            if (Body && Body->Kind == IrStBlock && Body->StmtCount == 1)
                Body = Body->Stmts[0];
            BE = (Body && Body->Kind == IrStExpr) ? Body->Expr : NULL;
            if (Cond && Cond->Kind == IrExBinOp
                && Cond->Op == TokenNotEqual
                && Cond->Left && Cond->Left->Kind == IrExIdent
                && Cond->Left->LocalSlot >= 0
                && Cond->Right && Cond->Right->Kind == IrExConst
                && Cond->Right->ConstVal == 0
                && S->IncStmt != NULL
                && BE != NULL && BE->Kind == IrExAssign
                && (BE->Op == TokenAddAssign || BE->Op == 0 || BE->Op == TokenAssign)
                && BE->Left && BE->Left->Kind == IrExIdent
                && BE->Right != NULL
                && (BE->Right->Kind == IrExMember
                    || (BE->Right->Kind == IrExBinOp && BE->Right->Op == TokenPlus
                        && BE->Right->Right && BE->Right->Right->Kind == IrExMember)))
            {
                ok_shape = 1;
            }
            if (ok_shape)
            {
                IrLinInst Kin;
                if (S->InitStmt != NULL
                    && !LowerStmtRec(S->InitStmt, Code, Cap, N, Next))
                    ; /* fall through */
                else
                {
                    KernelStmtTab[KernelN] = S;
                    Kin.Op = IrLinKernel; Kin.Dest = Kin.A = Kin.B = 0;
                    Kin.Tok = IR_K_PTR_WALK; Kin.Imm = KernelN;
                    KernelN++;
                    if (Emit(Code, Cap, N, Kin))
                        return 1;
                    KernelN--;
                }
            }
        }
        /* Small const-trip for-unroll (≤4): life neighbors dy/dx -1..1.
         * Prefer FastCond Bound when Active (reliable for compiled for). */
        if (IrExecIsVmProd()
            && S->FastCond.Active && S->FastInc.Active
            && S->FastCond.VarName && S->FastInc.VarName
            && strcmp(S->FastCond.VarName, S->FastInc.VarName) == 0
            && !S->FastCond.BoundIsVar
            && S->BodyStmt != NULL)
        {
            long long lo = 0, hi = S->FastCond.Bound;
            int slot = S->FastInc.VarSlot;
            int ok_fc = 0;
            if (slot < 0 && S->FastInc.VarName != NULL && IrCallDepth > 0)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int li;
                for (li = 0; li < IR_CALL_MAX_LOCALS; li++)
                    if (Fr->LocalName[li] != NULL
                        && strcmp(Fr->LocalName[li], S->FastInc.VarName) == 0)
                    { slot = li; break; }
            }
            /* Init assign const → lo; Bound is exclusive for < and inclusive for <=.
             * FastCond stores Bound as the RHS of the comparison; Op in FastCond.Op. */
            if (S->InitStmt && S->InitStmt->Kind == IrStExpr && S->InitStmt->Expr
                && S->InitStmt->Expr->Kind == IrExAssign
                && S->InitStmt->Expr->Right)
            {
                IrExpr *Rv = S->InitStmt->Expr->Right;
                if (Rv->Kind == IrExConst)
                { lo = Rv->ConstVal; ok_fc = 1; }
                else if (Rv->Kind == IrExUnary && Rv->Op == TokenMinus
                         && Rv->Left && Rv->Left->Kind == IrExConst)
                { lo = -Rv->Left->ConstVal; ok_fc = 1; }
                else if (Rv->Kind == IrExBinOp && Rv->Op == TokenMinus
                         && Rv->Right && Rv->Right->Kind == IrExConst)
                {
                    /* -1 often lowered as (0-1) or (x-1) with x const 0 */
                    long long L = 0;
                    if (Rv->Left == NULL) L = 0;
                    else if (Rv->Left->Kind == IrExConst) L = Rv->Left->ConstVal;
                    else { L = 0; /* keep try */ }
                    if (Rv->Left == NULL || Rv->Left->Kind == IrExConst)
                    { lo = L - Rv->Right->ConstVal; ok_fc = 1; }
                }
            }
if (ok_fc)
            {
                if (S->FastCond.Op == TokenLessThan)
                    hi = S->FastCond.Bound - 1;
                else if (S->FastCond.Op == TokenLessEqual)
                    hi = S->FastCond.Bound;
                else if (S->FastCond.Op == TokenGreaterThan)
                { /* countdown not handled */ ok_fc = 0; }
                else
                    ok_fc = 0;
            }
            if (ok_fc && slot >= 0 && hi >= lo && (hi - lo) <= 3)
            {
                /* Only leaf fors (no nested loop in body): neighbors inner dx. */
                {
                    IrStmt *st[32];
                    int sp = 0, nested = 0;
                    st[sp++] = S->BodyStmt;
                    while (sp > 0 && !nested)
                    {
                        IrStmt *B = st[--sp];
                        int si;
                        if (B == NULL) continue;
                        if (B->Kind == IrStFor || B->Kind == IrStWhile || B->Kind == IrStDo)
                        { nested = 1; break; }
                        if (B->Kind == IrStBlock)
                            for (si = 0; si < B->StmtCount && sp < 32; si++)
                                st[sp++] = B->Stmts[si];
                        if (B->ThenStmt && sp < 32) st[sp++] = B->ThenStmt;
                        if (B->ElseStmt && sp < 32) st[sp++] = B->ElseStmt;
                        if (B->BodyStmt && sp < 32) st[sp++] = B->BodyStmt;
                    }
                    if (nested)
                        goto after_const_unroll;
                }
                long long v;
                int ok_all = 1;
                int saved_n = *N;
                for (v = lo; v <= hi; v++)
                {
                    IrLinInst In;
                    int tmp, body_end;
                    if (LoopSp >= 8) { ok_all = 0; break; }
                    tmp = AllocReg(Next);
                    if (tmp < 0) { ok_all = 0; break; }
                    In.Op = IrLinImm; In.Dest = (unsigned char)tmp;
                    In.A = In.B = 0; In.Tok = 0; In.Imm = v;
                    if (!Emit(Code, Cap, N, In)) { ok_all = 0; break; }
                    In.Op = IrLinStoreLocal; In.Dest = 0;
                    In.A = (unsigned char)tmp; In.B = 0; In.Tok = 0;
                    In.Imm = slot;
                    if (!Emit(Code, Cap, N, In)) { ok_all = 0; break; }
                    PushLoop(1);
                    if (!LowerStmtRec(S->BodyStmt, Code, Cap, N, Next))
                    {
                        /* SealBrk pops; if body failed before any seal, pop manually */
                        if (LoopSp > 0) LoopSp--;
                        ok_all = 0; break;
                    }
                    body_end = *N;
                    SealCont(Code, body_end);
                    SealBrk(Code, body_end); /* pops LoopSp */
                }
                if (ok_all)
                {
                    return 1;
                }
                *N = saved_n;
            }
        }
        after_const_unroll: (void)0;
        if (IrExecIsVmProd()
            && S->InitStmt != NULL && S->Expr != NULL && S->IncStmt != NULL
            && S->BodyStmt != NULL)
        {
            IrStmt *Init = S->InitStmt;
            IrExpr *Cond = S->Expr;
            IrStmt *Inc = S->IncStmt;
            long long lo = 0, hi = 0;
            int slot = -1, ok_u = 0;
            if (Init->Kind == IrStExpr && Init->Expr
                && Init->Expr->Kind == IrExAssign
                && (Init->Expr->Op == 0 || Init->Expr->Op == TokenAssign)
                && Init->Expr->Left && Init->Expr->Left->Kind == IrExIdent
                && Init->Expr->Right)
            {
                IrExpr *Rv = Init->Expr->Right;
                long long cv = 0;
                int got = 0;
                if (Rv->Kind == IrExConst)
                { cv = Rv->ConstVal; got = 1; }
                else if (Rv->Kind == IrExUnary && Rv->Op == TokenMinus
                         && Rv->Left && Rv->Left->Kind == IrExConst)
                { cv = -Rv->Left->ConstVal; got = 1; }
                if (got)
                {
                    slot = Init->Expr->Left->LocalSlot;
                    lo = cv;
                    ok_u = 1;
                }
            }
            if (ok_u && Cond->Kind == IrExBinOp
                && Cond->Left && Cond->Left->Kind == IrExIdent
                && Cond->Right && Cond->Right->Kind == IrExConst)
            {
                /* slot may still be -1; bind from Cond */
                if (slot < 0)
                    slot = Cond->Left->LocalSlot;
                if (slot >= 0 && (Cond->Left->LocalSlot == slot || Cond->Left->LocalSlot < 0))
                {
                    if (Cond->Op == TokenLessEqual)
                        hi = Cond->Right->ConstVal;
                    else if (Cond->Op == TokenLessThan)
                        hi = Cond->Right->ConstVal - 1;
                    else
                        ok_u = 0;
                }
                else
                    ok_u = 0;
            }
            else
                ok_u = 0;
            if (ok_u)
            {
                IrExpr *Ie = (Inc->Kind == IrStExpr) ? Inc->Expr : NULL;
                /* LocalSlot may be unset on Init Ident; prefer Cond/Inc/FastInc. */
                if (slot < 0 && Cond->Left && Cond->Left->LocalSlot >= 0)
                    slot = Cond->Left->LocalSlot;
                if (slot < 0 && Ie && Ie->Left && Ie->Left->LocalSlot >= 0)
                    slot = Ie->Left->LocalSlot;
                if (slot < 0 && S->FastInc.Active && S->FastInc.VarSlot >= 0)
                    slot = S->FastInc.VarSlot;
                if (slot < 0
                    || !(Ie && Ie->Kind == IrExUnary
                         && Ie->Op == TokenIncrement
                         && Ie->Left && Ie->Left->Kind == IrExIdent
                         && (Ie->Left->LocalSlot == slot || Ie->Left->LocalSlot < 0)))
                    ok_u = 0;
            }
            if (getenv("BABYC_UNROLL_DEBUG") && ok_u)
                fprintf(stderr, "[unroll] candidate slot=%d lo=%lld hi=%lld\n", slot, lo, hi);
            else if (getenv("BABYC_UNROLL_DEBUG"))
            {
                IrExpr *Ie = (Inc && Inc->Kind == IrStExpr) ? Inc->Expr : NULL;
                fprintf(stderr, "[unroll] miss initK=%d condK=%d condOp=%d "
                    "incK=%d incOp=%d slot=%d cslot=%d\n",
                    Init && Init->Kind == IrStExpr,
                    Cond && Cond->Kind == IrExBinOp,
                    Cond && Cond->Kind == IrExBinOp ? (int)Cond->Op : -1,
                    Ie != NULL, Ie ? (int)Ie->Op : -1,
                    slot,
                    Cond && Cond->Left && Cond->Left->Kind == IrExIdent
                        ? Cond->Left->LocalSlot : -99);
            }
            if (ok_u && hi >= lo && (hi - lo) <= 3 && !IrVmStrictOn())
            {
                long long v;
                int ok_all = 1;
                int saved_n = *N;
                for (v = lo; v <= hi; v++)
                {
                    IrLinInst In;
                    int tmp, body_end;
                    if (LoopSp >= 8) { ok_all = 0; break; }
                    tmp = AllocReg(Next);
                    if (tmp < 0) { ok_all = 0; break; }
                    In.Op = IrLinImm; In.Dest = (unsigned char)tmp;
                    In.A = In.B = 0; In.Tok = 0; In.Imm = v;
                    if (!Emit(Code, Cap, N, In)) { ok_all = 0; break; }
                    In.Op = IrLinStoreLocal; In.Dest = 0;
                    In.A = (unsigned char)tmp; In.B = 0; In.Tok = 0;
                    In.Imm = slot;
                    if (!Emit(Code, Cap, N, In)) { ok_all = 0; break; }
                    PushLoop(1);
                    if (!LowerStmtRec(S->BodyStmt, Code, Cap, N, Next))
                    {
                        /* SealBrk pops; if body failed before any seal, pop manually */
                        if (LoopSp > 0) LoopSp--;
                        ok_all = 0; break;
                    }
                    body_end = *N;
                    SealCont(Code, body_end);
                    SealBrk(Code, body_end); /* pops LoopSp */
                }
                if (ok_all)
                    return 1;
                *N = saved_n; /* discard partial emit */
            }
        }


        /* EXEC=vm: KERNEL_FOR for scalar tight loops (fib-style).
         * IrKernelStmtOk rejects Index/Call/global — matches Body-IR gate. */
        /* EXEC=vm KERNEL_FOR: scalar-only body (no Index) + same cond/inc var.
         * isort/array stay on INSERT_SHIFT / ARRAY_ACCUM. Light path runs
         * IrTryKernelForLight so LocalVal vs LightLocal is correct. */
        if (IrExecIsVmProd() && KernelN < 16
            && S->FastCond.Active && S->FastInc.Active
            && S->FastCond.VarName && S->FastInc.VarName
            && strcmp(S->FastCond.VarName, S->FastInc.VarName) == 0)
        {
            IrStmt *Body = S->BodyStmt;
            int nested = 0;
            if (Body && Body->Kind == IrStBlock && Body->StmtCount == 1)
                Body = Body->Stmts[0];
            if (Body && (Body->Kind == IrStFor || Body->Kind == IrStWhile
                         || Body->Kind == IrStDo))
                nested = 1;
            if (!nested && Body != NULL && IrKernelStmtScalarOnly(Body, 0)
                && (S->InitStmt == NULL || IrKernelStmtScalarOnly(S->InitStmt, 0)))
            {
                IrLinInst Kin;
                int ok_init = 1;
                if (S->InitStmt != NULL
                    && !LowerStmtRec(S->InitStmt, Code, Cap, N, Next))
                    ok_init = 0;
                if (ok_init)
                {
                    KernelStmtTab[KernelN] = S;
                    Kin.Op = IrLinKernel;
                    Kin.Dest = 0;
                    Kin.A = 0;
                    Kin.B = 0;
                    Kin.Tok = IR_K_KERNEL_FOR;
                    Kin.Imm = KernelN;
                    /* A = paramIndex+1 when bound is a param (fib n). */
                    if (S->FastCond.BoundIsVar)
                    {
                        if (S->FastCond.BoundSlot >= 0
                            && S->FastCond.BoundSlot < IR_CALL_MAX_PARAMS
                            && S->FastCond.BoundSlot != S->FastInc.VarSlot)
                            Kin.A = (unsigned char)(S->FastCond.BoundSlot + 1);
                        else if (S->FastCond.BoundSlot < 0)
                            Kin.A = 1; /* param0 hint (fib n) — light only */
                    }
                    KernelN++;
                    if (Emit(Code, Cap, N, Kin))
                        return 1;
                    KernelN--;
                }
            }
        }
        /* Strided global-array fill: for (j = start; j <= b; j += st) g[j] = c.
         * Sieve inner loop. Unit-stride ARRAY_FILL misses j += i. */
        if (IrExecIsVmProd() && KernelN < 16 && IrKernelFpEnabledFor(S)
            && S->InitStmt && S->Expr && S->IncStmt && S->BodyStmt)
        {
            IrStmt *Bd = S->BodyStmt;
            IrStmt *Ic = S->IncStmt;
            IrExpr *Cn = S->Expr;
            IrExpr *As, *Idx, *Rhs;
            int jslot, match = 0;
            if (Bd->Kind == IrStBlock && Bd->StmtCount == 1) Bd = Bd->Stmts[0];
            if (Bd && Bd->Kind == IrStExpr && Bd->Expr
                && Bd->Expr->Kind == IrExAssign && Bd->Expr->Left && Bd->Expr->Right)
            {
                As = Bd->Expr;
                Idx = As->Left;
                Rhs = As->Right;
                if (Idx->Kind == IrExIndex && Idx->Left && Idx->Right
                    && Idx->Left->Kind == IrExIdent && Idx->Left->LocalSlot < 0
                    && Idx->Left->ParamSlot < 0 && Idx->Left->Ident
                    && Idx->Right->Kind == IrExIdent && Idx->Right->LocalSlot >= 0
                    && Rhs->Kind == IrExConst)
                {
                    jslot = Idx->Right->LocalSlot;
                    /* cond: j <= bound  or  j < bound */
                    if (Cn->Kind == IrExBinOp
                        && (Cn->Op == TokenLessEqual || Cn->Op == TokenLessThan)
                        && Cn->Left && Cn->Left->Kind == IrExIdent
                        && Cn->Left->LocalSlot == jslot
                        && Cn->Right
                        && (Cn->Right->Kind == IrExConst || Cn->Right->Kind == IrExIdent))
                    {
                        /* inc: j = j + stride  (stride Ident or Const) */
                        if (Ic->Kind == IrStExpr && Ic->Expr
                            && Ic->Expr->Kind == IrExAssign && Ic->Expr->Left
                            && Ic->Expr->Left->Kind == IrExIdent
                            && Ic->Expr->Left->LocalSlot == jslot
                            && Ic->Expr->Right && Ic->Expr->Right->Kind == IrExBinOp
                            && Ic->Expr->Right->Op == TokenPlus
                            && Ic->Expr->Right->Left && Ic->Expr->Right->Left->Kind == IrExIdent
                            && Ic->Expr->Right->Left->LocalSlot == jslot
                            && Ic->Expr->Right->Right
                            && (Ic->Expr->Right->Right->Kind == IrExIdent
                                || Ic->Expr->Right->Right->Kind == IrExConst))
                            match = 1;
                    }
                }
            }
            if (match)
            {
                IrLinInst Kin;
                int saved_n = *N;
                if (LowerStmtRec(S->InitStmt, Code, Cap, N, Next))
                {
                    KernelStmtTab[KernelN] = S;
                    Kin.Op = IrLinKernel; Kin.Dest = Kin.A = Kin.B = 0;
                    Kin.Tok = IR_K_STRIDE_FILL; Kin.Imm = KernelN;
                    KernelN++;
                    if (Emit(Code, Cap, N, Kin))
                        return 1;
                    KernelN--;
                }
                *N = saved_n;
            }
        }
        /* Scan-accum: for (i=s; i<=b; i++) if (!g[i]) { c++; sum+=i; } */
        if (IrExecIsVmProd() && KernelN < 16 && IrKernelFpEnabledFor(S)
            && S->InitStmt && S->Expr && S->IncStmt && S->BodyStmt
            && S->FastInc.Active)
        {
            IrStmt *Bd = S->BodyStmt;
            IrExpr *Cn = S->Expr;
            int islot = -1, match = 0;
            if (Cn->Kind == IrExBinOp
                && (Cn->Op == TokenLessEqual || Cn->Op == TokenLessThan)
                && Cn->Left && Cn->Left->Kind == IrExIdent
                && Cn->Left->LocalSlot >= 0
                && Cn->Right && Cn->Right->Kind == IrExConst
                && S->FastInc.VarSlot == Cn->Left->LocalSlot)
            {
                islot = Cn->Left->LocalSlot;
                if (Bd->Kind == IrStBlock && Bd->StmtCount == 1 && Bd->Stmts)
                    Bd = Bd->Stmts[0];
                if (Bd && Bd->Kind == IrStIf && Bd->Expr && Bd->ThenStmt
                    && Bd->ElseStmt == NULL
                    && Bd->Expr->Kind == IrExUnary && Bd->Expr->Op == TokenUnaryNot
                    && Bd->Expr->Left && Bd->Expr->Left->Kind == IrExIndex
                    && Bd->Expr->Left->Left && Bd->Expr->Left->Left->Kind == IrExIdent
                    && Bd->Expr->Left->Left->LocalSlot < 0
                    && Bd->Expr->Left->Left->ParamSlot < 0
                    && Bd->Expr->Left->Right && Bd->Expr->Left->Right->Kind == IrExIdent
                    && Bd->Expr->Left->Right->LocalSlot == islot
                    && Bd->ThenStmt->Kind == IrStBlock
                    && Bd->ThenStmt->StmtCount == 2
                    && Bd->ThenStmt->Stmts)
                {
                    int saw_inc = 0, saw_add = 0, k;
                    for (k = 0; k < 2; k++)
                    {
                        IrStmt *St = Bd->ThenStmt->Stmts[k];
                        if (!St || St->Kind != IrStExpr || !St->Expr) continue;
                        if (St->Expr->Kind == IrExUnary && St->Expr->Op == TokenIncrement
                            && St->Expr->Left && St->Expr->Left->Kind == IrExIdent
                            && St->Expr->Left->LocalSlot >= 0)
                            saw_inc = 1;
                        else if (St->Expr->Kind == IrExAssign
                            && St->Expr->Left && St->Expr->Left->Kind == IrExIdent
                            && St->Expr->Left->LocalSlot >= 0
                            && St->Expr->Right && St->Expr->Right->Kind == IrExBinOp
                            && St->Expr->Right->Op == TokenPlus
                            && St->Expr->Right->Left && St->Expr->Right->Left->Kind == IrExIdent
                            && St->Expr->Right->Left->LocalSlot == St->Expr->Left->LocalSlot
                            && St->Expr->Right->Right && St->Expr->Right->Right->Kind == IrExIdent
                            && St->Expr->Right->Right->LocalSlot == islot)
                            saw_add = 1;
                    }
                    if (saw_inc && saw_add) match = 1;
                }
            }
            if (match)
            {
                IrLinInst Kin;
                int saved_n = *N;
                if (LowerStmtRec(S->InitStmt, Code, Cap, N, Next))
                {
                    KernelStmtTab[KernelN] = S;
                    Kin.Op = IrLinKernel; Kin.Dest = Kin.A = Kin.B = 0;
                    Kin.Tok = IR_K_SCAN_ACCUM; Kin.Imm = KernelN;
                    KernelN++;
                    if (Emit(Code, Cap, N, Kin))
                        return 1;
                    KernelN--;
                }
                *N = saved_n;
            }
        }
        /* FastCond/Inc ok: lower Init/Expr/Body/Inc (F6 shadow/vm). */
        if (LoopSp >= 8) return 0;
        if (!LowerStmtRec(S->InitStmt, Code, Cap, N, Next)) return 0;
        PushLoop(1);
        head = *N;
        if (S->Expr == NULL)
            br = -1;
        else
        {
            if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
            { LoopSp--; return 0; }
            br = *N;
            In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)dest; In.B = 0;
            In.Tok = 0; In.Imm = 0;
            if (!Emit(Code, Cap, N, In))
            { LoopSp--; return 0; }
        }
        if (!LowerStmtRec(S->BodyStmt, Code, Cap, N, Next))
        { LoopSp--; return 0; }
        SealCont(Code, *N);
        if (!LowerStmtRec(S->IncStmt, Code, Cap, N, Next))
        { LoopSp--; return 0; }
        In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = head;
        if (!Emit(Code, Cap, N, In))
        { LoopSp--; return 0; }
        end = *N;
        if (br >= 0) Code[br].Imm = end;
        SealBrk(Code, end);
        return 1;
    }
    if (S->Kind == IrStDo && S->Expr != NULL)
    {
        int head, br;
        if (IrExecIsVmProd() && KernelN < 16
            && S->FastCond.Active && S->FastCond.Op == TokenGreaterThan)
        {
            IrStmt *Body = S->BodyStmt;
            if (Body && Body->Kind == IrStBlock && Body->StmtCount == 2)
            {
                IrLinInst Kin;
                KernelStmtTab[KernelN] = S;
                Kin.Op = IrLinKernel; Kin.Dest = Kin.A = Kin.B = 0;
                Kin.Tok = IR_K_DO_COUNTDOWN; Kin.Imm = KernelN;
                KernelN++;
                if (Emit(Code, Cap, N, Kin))
                    return 1;
                KernelN--;
            }
        }
        if (LoopSp >= 8) return 0;
        PushLoop(1);
        head = *N;
        if (!LowerStmtRec(S->BodyStmt, Code, Cap, N, Next))
        { LoopSp--; return 0; }
        SealCont(Code, *N);
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
        { LoopSp--; return 0; }
        br = *N;
        In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)dest; In.B = 0;
        In.Tok = 0; In.Imm = 0;
        if (!Emit(Code, Cap, N, In))
        { LoopSp--; return 0; }
        In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = head;
        if (!Emit(Code, Cap, N, In))
        { LoopSp--; return 0; }
        Code[br].Imm = *N;
        SealBrk(Code, *N);
        return 1;
    }
    if (S->Kind == IrStBreak)
        return EmitBreak(Code, Cap, N);
    if (S->Kind == IrStContinue)
        return EmitCont(Code, Cap, N);
    if (S->Kind == IrStSwitch && S->Expr != NULL && S->BodyStmt != NULL
        && S->BodyStmt->Kind == IrStBlock)
    {
        int sel, ci, ncase, defpc, endjmp;
        int casePc[16], caseVal[16], caseAt[16];
        IrStmt *B = S->BodyStmt;
        if (LoopSp >= 8) return 0;
        ncase = 0;
        defpc = -1;
        for (ci = 0; ci < B->StmtCount; ci++)
        {
            IrStmt *T = B->Stmts[ci];
            if (T == NULL) continue;
            if (T->Kind == IrStCase)
            {
                if (ncase >= 16) return 0;
                if (T->Expr == NULL) return 0;
                caseVal[ncase] = ci; /* index into body, expr lowered later */
                caseAt[ncase] = ci;
                ncase++;
            }
            else if (T->Kind == IrStDefault)
                defpc = ci;
        }
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &sel)) return 0;
        for (ci = 0; ci < ncase; ci++)
        {
            int one, eq, brz;
            if (!LowerRec(B->Stmts[caseAt[ci]]->Expr, Code, Cap, N, Next, &one))
                return 0;
            eq = AllocReg(Next);
            if (eq < 0) return 0;
            In.Op = IrLinBin; In.Dest = (unsigned char)eq;
            In.A = (unsigned char)sel; In.B = (unsigned char)one;
            In.Tok = TokenEqual; In.Imm = 0;
            if (!Emit(Code, Cap, N, In)) return 0;
            brz = *N;
            In.Op = IrLinBrZ; In.Dest = 0; In.A = (unsigned char)eq; In.B = 0;
            In.Tok = 0; In.Imm = 0;
            if (!Emit(Code, Cap, N, In)) return 0;
            casePc[ci] = *N;
            In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
            if (!Emit(Code, Cap, N, In)) return 0;
            Code[brz].Imm = *N;
            (void)casePc;
        }
        endjmp = *N;
        In.Op = IrLinJmp; In.Dest = In.A = In.B = 0; In.Tok = 0; In.Imm = 0;
        if (!Emit(Code, Cap, N, In)) return 0;
        PushLoop(0);
        {
            int k, nx = 0;
            int marks[17];
            for (k = 0; k < ncase; k++) marks[k] = -1;
            for (ci = 0; ci < B->StmtCount; ci++)
            {
                IrStmt *T = B->Stmts[ci];
                if (T == NULL) continue;
                if (T->Kind == IrStCase)
                {
                    for (k = 0; k < ncase; k++)
                        if (caseAt[k] == ci) Code[casePc[k]].Imm = *N;
                    continue;
                }
                if (T->Kind == IrStDefault)
                {
                    Code[endjmp].Imm = *N;
                    defpc = *N;
                    continue;
                }
                if (!LowerStmtRec(T, Code, Cap, N, Next)) return 0;
            }
            if (Code[endjmp].Imm == 0)
                Code[endjmp].Imm = *N;
            (void)nx; (void)marks; (void)defpc;
        }
        SealBrk(Code, *N);
        return 1;
    }
    if (S->Kind == IrStReturn)
    {
        if (S->Expr == NULL)
        {
            /* void return must still terminate the linear body. */
            In.Op = IrLinRet; In.Dest = 0;
            In.A = 0; In.B = 0; In.Tok = 0; In.Imm = 0;
            return Emit(Code, Cap, N, In);
        }
        /* Unlowerable return expr must refuse the whole body. Returning 1
         * left an eligible linear body without IrLinRet; under vm_strict
         * (no tiny-body tree skip) that body ran and yielded 0
         * (511_bitfield_fn_inc: p->a++ not linearised). */
        if (!LowerRec(S->Expr, Code, Cap, N, Next, &dest))
            return 0;
        In.Op = IrLinRet; In.Dest = 0;
        In.A = (unsigned char)dest; In.B = 0; In.Tok = 0; In.Imm = 0;
        return Emit(Code, Cap, N, In);
    }
    return 0;
}

int IrLowerStmt(IrStmt *S, IrLinInst *Code, int Cap, int *NOut)
{
    int next = 0, n = 0;
    int nested = (LowerDepth > 0);
    int pushed = 0;
    /* Nested lower (e.g. FnCodeCompile while parent tables live): isolate. */
    if (nested)
        pushed = CompileFramePush();
    LowerDepth++;
    LoopSp = 0;
    NLab = 0;
    memset(CallSkip, 0, sizeof(CallSkip));
    GlobN = 0;
    IdxN = 0;
    IdxDirty = 0;
    GlobSaved = 0;
    GlobDirty = 0;
    /* LINE opcodes emitted per-stmt inside LowerStmtRec when BABYC_DEBUG. */
    if (!LowerStmtRec(S, Code, Cap, &n, &next))
    {
        LowerDepth--;
        if (pushed)
            CompileFramePop();
        return 0;
    }
    {
        int li;
        for (li = 0; li < NLab; li++)
            if (Labs[li].Pc < 0)
            {
                LowerDepth--;
                if (pushed)
                    CompileFramePop();
                return 0;
            }
    }
    if (LoopSp != 0)
    {
        LowerDepth--;
        if (pushed)
            CompileFramePop();
        return 0;
    }
    *NOut = n;
    LowerDepth--;
    /* Nested: restore parent. Top-level: leave tables for subsequent Step. */
    if (pushed)
        CompileFramePop();
    return 1;
}


FnCodeSlot *FnCodeCompile(struct ParseState *Parser, IrStmt *Body,
                                   struct Value *Func)
{
    FnCodeSlot *S;
    int n = 0, ok, pushed;
    S = FnCodeFind(Body);
    if (S != NULL)
    {
        if (S->Elig == 0) return NULL;
        if (S->N > 0) return S;
    }
    else
    {
        if (FnSlotN >= FNCODE_SLOTS)
            return NULL; /* cache full → tree */
        S = &FnSlots[FnSlotN++];
        S->Body = Body;
        S->N = 0;
        S->Elig = -1;
        S->MonoElig = -1;
        S->MemoElig = -1;
        S->PtrIndexedElig = -1;
        S->NestedLightSafe = 0;
        S->Func = Func;
    }
    if (Func != NULL && S->Func == NULL)
        S->Func = Func;
    if (S->Elig < 0)
        S->Elig = IrFnBodyIntegerEligible(Body) ? 1 : 0;
    if (S->Elig != 1)
    {
        return NULL;
    }

    /* Isolate only when parent has live tables / wide / depth. */
    pushed = 0;
    if (FnActive != NULL || CallN > 0 || GlobN > 0 || IdxN > 0
        || LinExecDepth > 0 || LowerDepth > 0 || LinWideMax > 0
        || CompileFrameSp > 0)
        pushed = CompileFramePush();
    LinParser = Parser;
    CompileSlot = S;
    memset(S->SlotIsPtr, 0, sizeof(S->SlotIsPtr));
    memset(S->SlotIsFptr, 0, sizeof(S->SlotIsFptr));
    memset(S->SlotFromType, 0, sizeof(S->SlotFromType));
    memset((void *)S->SlotName, 0, sizeof(S->SlotName)); /* discard const on ptr elems */
    CallN = 0;
    {
        int zi;
        for (zi = 0; zi < IR_LIN_CALL_SLOTS; zi++)
        {
            CallIndirectSlot[zi] = -1;
            CallIndirectReg[zi] = -1;
            CallIndirectParam[zi] = -1;
            CallIndirectExpr[zi] = NULL;
        }
    }
    KernelN = 0;
    memset(CompLocalNDims, 0, sizeof(CompLocalNDims));
    memset(CompLocalDims, 0, sizeof(CompLocalDims));
    memset(CompLocalElemIsPtr, 0, sizeof CompLocalElemIsPtr);
    memset(CompLocalLeafEs, 0, sizeof(CompLocalLeafEs));
    memset(CompLocalFromType, 0, sizeof(CompLocalFromType));
    LinSkippedCall = 0;
    LinRetSeen = 0;
    LinStepAbort = 0;
    LinDiv0 = 0;
    GlobN = 0;
    IdxN = 0;
    memset(GlobVal, 0, sizeof(GlobVal)); memset(GlobRaw, 0, sizeof(GlobRaw));
    memset(LinWideOn, 0, sizeof(LinWideOn));
    LinWideMax = 0;
    memset(CallSkip, 0, sizeof(CallSkip));
    /* LoopSp/NLab reset inside IrLowerStmt */
    LinCompileActive = 1;
    /* Callee compile must not overwrite existing LocalName on the live frame
     * (both-mode: light-call to f() stamped f's names over main's k,r).
     * New stamps (NULL→name) are kept — loop-body compile of the current
     * function needs them (148/149 BoundIsVar).
     *
     * LocalInit: same class of bug as 1035 — body-local slot indices collide
     * with the caller. IrLiveMarkInit no-ops under LinCompileActive; this
     * snapshot/restore is the hard guard against any lower path that still
     * writes Fr->LocalInit[slot] directly. Cost: only at FnCodeCompile, never
     * on the IrExecLinLocals hot path. */
    {
        char *saved_names[IR_CALL_MAX_LOCALS];
        char saved_init[IR_CALL_MAX_LOCALS];
        int si;
        int have = (IrCallDepth > 0 && IrCallStack[IrCallDepth - 1] != NULL);
        IrCallFrame *Fr = have ? IrCallStack[IrCallDepth - 1] : NULL;
        int init_poison = 0;
        if (have)
        {
            for (si = 0; si < IR_CALL_MAX_LOCALS; si++)
            {
                saved_names[si] = Fr->LocalName[si];
                saved_init[si] = Fr->LocalInit[si];
            }
        }
        ok = IrLowerStmt(Body, S->Code, IR_LIN_STMT_MAX, &n);
        if (have)
        {
            for (si = 0; si < IR_CALL_MAX_LOCALS; si++)
            {
                if (saved_names[si] != NULL)
                    Fr->LocalName[si] = saved_names[si];
                if (Fr->LocalInit[si] != saved_init[si])
                {
                    init_poison = 1;
                    Fr->LocalInit[si] = saved_init[si];
                }
            }
            if (init_poison)
            {
                /* Always restore (above). Report under guard env:
                 * BABYC_LIVE_INIT_GUARD=1 → stderr once; =2 → abort. */
                const char *g = getenv("BABYC_LIVE_INIT_GUARD");
                if (g != NULL && g[0] != '0')
                {
                    fprintf(stderr,
                        "[babyc-live-init-guard] FnCodeCompile mutated caller "
                        "LocalInit (restored); fix path to use IrLiveMarkInit only\n");
                    if (g[0] == '2')
                        abort();
                }
            }
        }
    }
    LinCompileActive = 0;
    /* Sparse body: lower returned success but emitted almost nothing while
     * the source had many stmts (ptr/switch skipped as no-op return 1). */
    if (ok && n > 0 && n < 3 && !LinSkippedCall)
    {
        /* allow tiny pure helpers (return x;); refuse empty-ish larger bodies */
        ;
    }
    /* Empty body (ok, n==0, no skipped call) is a valid linear no-op.
     * Reject only real failures and bodies with an unlowerable call. */
    if (!ok || LinSkippedCall || n < 0)
    {
        if (IrVmProfileOn())
            fprintf(stderr, "[babyc-exec-profile] compile_reject=lower body_kind=%d n=%d ok=%d skipped_call=%d calls=%d\n",
                    Body ? (int)Body->Kind : -1, n, ok, LinSkippedCall, CallN);
        S->Elig = 0;
        S->N = 0;
        if (pushed)
            CompileFramePop();
        return NULL;
    }
    if (n == 0)
    {
        /* Legitimate empty function: accept as linear no-op. */
        S->Elig = 1;
        S->N = 0;
        S->MaxLocal = 0;
        S->HasKernel = 0;
        S->HasArrayKernel = 0;
        if (pushed)
            CompileFramePop();
        IrVmProdNotice();
        return S;
    }
    /* Cap body size / call fan-out: large blocks residual SEGV under FnCode. */
    if (n > 384 || CallN > 255)
    {
        if (IrVmProfileOn())
            fprintf(stderr, "[babyc-exec-profile] compile_reject=cap body_kind=%d n=%d calls=%d\n",
                    Body ? (int)Body->Kind : -1, n, CallN);
        S->Elig = 0;
        S->N = 0;
        if (pushed)
            CompileFramePop();
        return NULL;
    }
    n = IrLinPeephole(S->Code, n);
    S->N = n;
    {
        int k, mx = 0;
        for (k = 0; k < n; k++)
        {
            IrLinInst *In = &S->Code[k];
            if ((In->Op == IrLinLoadLocal || In->Op == IrLinStoreLocal)
                && In->Imm >= 0 && In->Imm + 1 > mx)
                mx = (int)In->Imm + 1;
            if (In->Op == IrLinAddStoreLocal && (int)In->A + 1 > mx)
                mx = (int)In->A + 1;
        }
        S->MaxLocal = mx;
        {
            int hi; S->HasKernel = 0; S->HasArrayKernel = 0;
            for (hi = 0; hi < S->N; hi++)
            {
                if (S->Code[hi].Op == IrLinKernel)
                {
                    S->HasKernel = 1;
                    if (S->Code[hi].Tok == IR_K_ARRAY_ACCUM
                        || S->Code[hi].Tok == IR_K_ARRAY_FILL
                        || S->Code[hi].Tok == IR_K_DECL_ARRAY
                        || S->Code[hi].Tok == IR_K_DECL_STRUCT
                        || S->Code[hi].Tok == IR_K_INSERT_SHIFT
                        || S->Code[hi].Tok == IR_K_DECL_STATIC
                        || S->Code[hi].Tok == IR_K_DECL_STATIC_ARR
                        || S->Code[hi].Tok == IR_K_DECL_STATIC_STU)
                        S->HasArrayKernel = 1;
                }
            }
        }
    }
    if (S->Func == NULL && CallN > 0)
    {
        int ci, self = 1;
        struct Value *CF0 = CallFnTab[0];
        for (ci = 1; ci < CallN; ci++)
            if (CallFnTab[ci] != CF0) { self = 0; break; }
        /* Only self-recursion: same FV and BodyIr matches this body (not host). */
        if (self && CF0 != NULL && CF0->Val != NULL
            && CF0->Val->FuncDef.Intrinsic == NULL
            && (IrStmt *)CF0->Val->FuncDef.BodyIr == Body)
            S->Func = CF0;
    }
    FnCodeSnapshot(S);

    /* Mono/memo eligibility once per slot (purity walk is expensive). */
    if (S->Func != NULL)
    {
        int np = S->Func->Val && S->Func->Val->FuncDef.NumParams > 0
            ? S->Func->Val->FuncDef.NumParams : 1;
        if (S->MonoElig < 0)
            S->MonoElig = (!IrFuncUsesFP(S->Func) && IrMonoIsEligible(Body, np)) ? 1 : 0;
        if (S->MemoElig < 0)
            S->MemoElig = (!IrFuncUsesFP(S->Func) && IrMemoEligible(S->Func, np)) ? 1 : 0;
    }
    else
    {
        S->MonoElig = 0;
        S->MemoElig = 0;
    }
    if (pushed)
        CompileFramePop();
    CompileSlot = NULL;
    IrVmProdNotice();
    return S;
}

/* integer eligibility extract */
int IrExprIntegerOk(IrExpr *E)
{
    int i;
    if (E == NULL) return 1;
    /* Structural pointer ops only — TypeFlags IR_TF_PTR is often inherited
     * from function designators / call results and is not reliable here. */
    if (E->Kind == IrExMember)
    {
        if (E->Left == NULL) return 0;
        if (E->Left->Kind == IrExIdent) return 1;
        if (E->Left->Kind == IrExUnary && E->Left->Op == TokenAsterisk
            && E->Left->Left != NULL && E->Left->Left->Kind == IrExIdent)
            return 1;
        return IrExprIntegerOk(E->Left);
    }
    if (E->Kind == IrExIndex)
    {
        IrExpr *Base = E->Left;
        IrExpr *Root;
        if (Base == NULL) return 0;
        /* Nested a[i][j]: root must be Ident (local/global array). */
        if (Base->Kind == IrExIndex)
        {
            Root = Base;
            while (Root != NULL && Root->Kind == IrExIndex)
                Root = Root->Left;
            if (Root == NULL || Root->Kind != IrExIdent)
                return 0;
            if (Root->ParamSlot >= 0)
                return 0; /* param arrays: tree (pointer decay surface) */
            if (!IrExprIntegerOk(E->Right)) return 0;
            return IrExprIntegerOk(Base);
        }
        /* s.arr[i] / (*p).arr[i]: Index of array Member (nested / arrow OK) */
        if (Base->Kind == IrExMember
            && Base->Left != NULL && Base->Ident != NULL
            && (Base->Left->Kind == IrExIdent || Base->Left->Kind == IrExMember
                || (Base->Left->Kind == IrExUnary && Base->Left->Op == TokenAsterisk
                    && Base->Left->Left != NULL
                    && Base->Left->Left->Kind == IrExIdent)))
        {
            if (!IrExprIntegerOk(E->Right)) return 0;
            return IrExprIntegerOk(Base->Left);
        }
        /* Local array, param int*, global array — LDX under VM. */
        if (!IrExprIntegerOk(E->Right)) return 0;
        return 1;
    }
    /* &x / *p: allow when operand is Ident/Index/simple ptr expr —
     * lower already emits LoadLocal / LoadIndex Tok=2. */
    if (E->Kind == IrExUnary && E->Op == TokenAmpersand)
    {
        if (E->Left == NULL) return 0;
        if (E->Left->Kind == IrExIdent) return 1;
        if (E->Left->Kind == IrExIndex)
            return IrExprIntegerOk(E->Left);
        /* &s.m / &v[i].m / &ret()->m — bitfields cannot form an address (506). */
        if (E->Left->Kind == IrExMember)
        {
            if (E->Left->BitWidth > 0)
                return 0;
            return IrExprIntegerOk(E->Left);
        }
        return 0;
    }
    if (E->Kind == IrExUnary && E->Op == TokenAsterisk)
    {
        if (E->Left == NULL) return 0;
        if (E->Left->Kind == IrExIdent) return 1;
        if (E->Left->Kind == IrExIndex)
            return IrExprIntegerOk(E->Left); /* *a[i] / *ptrs[i] */
        if (E->Left->Kind == IrExMember)
            return IrExprIntegerOk(E->Left); /* *o.ip / *s.p */
        if (E->Left->Kind == IrExBinOp || E->Left->Kind == IrExUnary)
            return IrExprIntegerOk(E->Left);
        if (E->Left->Kind == IrExCall)
            return IrExprIntegerOk(E->Left);
        return 0;
    }
    if (E->Kind == IrExIdent)
    {
        /* Pointer Ident OK under linear (LoadLocal of address bits). */
        return 1;
    }
    if (E->Kind == IrExCall)
    {
        const char *n = NULL;
        if (E->Left != NULL && E->Left->Ident != NULL)
            n = E->Left->Ident;
        else if (E->Ident != NULL)
            n = E->Ident;
        /* Call forms: name, local_fp, (*fp), tab[i], (*s.fn), (*p->fn). */
        if (E->Left != NULL)
        {
            IrExpr *Callee = E->Left;
            if (Callee->Kind == IrExUnary && Callee->Op == TokenAsterisk
                && Callee->Left != NULL)
                Callee = Callee->Left; /* (*x)(...) */
            if (Callee->Kind == IrExIdent)
            {
                /* ParamSlot formal fptr OK */
            }
            else if (Callee->Kind == IrExIndex
                     && Callee->Left != NULL && Callee->Left->Kind == IrExIdent
                     && Callee->Right != NULL
                     && IrExprIntegerOk(Callee->Right)
                     && (Callee->Left->LocalSlot >= 0
                         || (Callee->Left->LocalSlot < 0
                             && Callee->Left->ParamSlot < 0
                             && Callee->Left->Ident != NULL)))
            {
                /* tab[i](...) local or global fptr table */
            }
            else if (Callee->Kind == IrExMember
                     && Callee->Left != NULL && Callee->Ident != NULL
                     && IrExprIntegerOk(Callee->Left))
            {
                /* s.fn / p->fn / tab[i].fn — member fptr */
            }
            else
                return 0;
        }
        /* Host I/O (printf/puts/...) lowers as IrLinCall Intrinsic under
         * EXEC=vm — allowed so main() with a trailing printf stays eligible.
         * malloc/free/scanf stay out (pointer / input surface). */
        if (n != NULL
            && (strcmp(n, "scanf") == 0 || strcmp(n, "fgets") == 0
                || strcmp(n, "fopen") == 0 || strcmp(n, "fclose") == 0))
            return 0;
        if (E->Args != NULL)
            for (i = 0; i < E->ArgCount; i++)
            {
                IrExpr *A = E->Args[i];
                if (A == NULL) continue;
                /* Function designator as arg: linear packs LoadGlobal bits,
                 * not the persistent Func Value* needed by trampolines
                 * (pthread_create / CreateThread). Keep these calls on tree. */
                if (A->Kind == IrExIdent && A->LocalSlot < 0 && A->ParamSlot < 0
                    && A->Ident != NULL && LinParser != NULL && LinParser->pc != NULL)
                {
                    struct Value *GV = NULL;
                    if (VariableDefined(LinParser->pc, (char *)A->Ident)
                        && TableGet(&LinParser->pc->GlobalTable, (char *)A->Ident,
                            &GV, NULL, NULL, NULL)
                        && GV != NULL && GV->Typ != NULL
                        && GV->Typ->Base == TypeFunction)
                        return 0;
                }
                if (!IrExprIntegerOk(A)) return 0;
            }
        return 1;
    }
    if (!IrExprIntegerOk(E->Left)) return 0;
    if (!IrExprIntegerOk(E->Right)) return 0;
    if (E->Args != NULL)
        for (i = 0; i < E->ArgCount; i++)
            if (!IrExprIntegerOk(E->Args[i])) return 0;
    return 1;
}


int IrStmtIntegerOk(IrStmt *S)
{
    int i;
    if (S == NULL) return 1;
    if (S->Kind == IrStDecl)
    {
        /* VLA storage and sizeof depend on the active runtime frame. */
        if (S->DeclArraySize == -2)
            return 0;
        /* Static scalar or BSS array of scalar elems via IrStaticLocalGet. */
        if (S->DeclIsStatic)
        {
            if (S->DeclIsFptr)
                return 0;
            /* DeclIsPtr is often set on a[N] (blob addr in LocalVal) — OK for arrays. */
            if (S->DeclIsPtr && S->DeclArraySize <= 0)
                return 0; /* scalar pointer static residual */
            if (S->DeclFromType != NULL && S->DeclFromType->Base == TypeFP)
                return 0;
            /* Static struct/union BSS (no brace-init). */
            if (S->DeclFromType != NULL
                && (S->DeclFromType->Base == TypeStruct
                    || S->DeclFromType->Base == TypeUnion)
                && S->DeclArraySize <= 1
                && S->DeclInitCount <= 0 && S->DeclInitVals == NULL)
            {
                size_t nbytes = S->DeclFromType->Sizeof > 0
                    ? (size_t)S->DeclFromType->Sizeof : 0;
                if (nbytes == 0 || nbytes > 4096)
                    return 0;
                return 1;
            }
            if (S->DeclFromType != NULL
                && (S->DeclFromType->Base == TypeStruct
                    || S->DeclFromType->Base == TypeUnion))
                return 0; /* brace / array-of-struct residual */
            if (S->DeclArraySize > 0)
            {
                int es = S->DeclElemSize > 0 ? S->DeclElemSize : 0;
                long long nbytes;
                if (es <= 0) es = (int)sizeof(int);
                if (es != 1 && es != 2 && es != 4 && es != 8)
                    return 0;
                if (S->DeclArraySize > 1024)
                    return 0;
                nbytes = (long long)S->DeclArraySize * (long long)es;
                if (nbytes <= 0 || nbytes > 4096)
                    return 0;
                return 1; /* static char/int/... a[N] BSS */
            }
            return 1; /* scalar static int/char/... */
        }
        /* DeclIsFptr: allowed under linear — LocalVal holds Value*. */
        if (S->DeclIsFptr) return 1;
        /* Value arrays first: ir_compile often sets DeclIsPtr=1 on a[N]
         * (LocalVal holds blob addr). DECL_ARRAY handles int-scale elems. */
        if (S->DeclArraySize > 0 && !S->DeclElemIsPtr)
        {
            int es = S->DeclElemSize;
            long long nbytes;
            if (es <= 0 && S->DeclFromType != NULL && S->DeclFromType->FromType != NULL)
                es = S->DeclFromType->FromType->Sizeof;
            if (es <= 0)
                es = (int)sizeof(int);
            /* char/short/int arrays → DECL_ARRAY (Kin.A = es 1..255).
             * Cap size so stack-overflow / charge tests stay on tree
             * (linear calloc does not charge the PicoC stack arena). */
            if (S->DeclFromType != NULL && S->DeclFromType->FromType != NULL)
            {
                enum BaseType eb = S->DeclFromType->FromType->Base;
                if (eb == TypePointer || eb == TypeArray
                    || eb == TypeFunction)
                    return 0;
                /* Array of struct/union: DECL_ARRAY blob, Member via Index. */
                if (eb == TypeStruct || eb == TypeUnion)
                {
                    if (es <= 0)
                        es = S->DeclFromType->FromType->Sizeof;
                    if (es <= 0 || es > 255)
                        return 0;
                    if (S->DeclArraySize > 64)
                        return 0;
                    /* fall through to nbytes cap */
                }
            }
            if (es > 255)
                return 0;
            nbytes = (long long)S->DeclArraySize * (long long)es;
            if (S->DeclArraySize > 1024 || nbytes > 4096)
                return 0;
            /* char/short bulk Index es=1/2 residual on large buffers (282
             * stack-charge tests). Allow moderate char buffers (<=192) for
             * 650/strcpy; larger stay on tree. */
            if (es < (int)sizeof(int) && S->DeclArraySize > 192)
                return 0;
            /* Array of bitfield-struct allowed: Load/Store Tok=1 + IrSafeMemberAddr
             * stamp BitWidth and use IrBitExtract/Insert (same as scalar s.bf).
             * InitMap is storage-unit grain, not per-bit — fine for assign-then-read. */
            return 1;
        }
        /* Array of pointers: int *a[N]. Only under vm_strict until Index
         * store of pointer words marks LocalInitMap (263 fields[i]). */
        if (S->DeclArraySize > 0 && S->DeclElemIsPtr && !S->DeclIsFptr
            && IrVmStrictOn())
        {
            int n = S->DeclArraySize;
            int es = (int)sizeof(void *);
            long long nbytes = (long long)n * (long long)es;
            if (n <= 0 || n > 1024 || nbytes > 8192)
                return 0;
            return 1;
        }
        /* Scalar pointer locals: lower stores bits + LocalIsPtr/LocalElemSize. */
        if (S->DeclIsPtr && !S->DeclIsFptr && S->DeclArraySize <= 0)
            return 1;
        if (S->DeclIsPtr) return 0;
        if (S->DeclFromType != NULL)
        {
            enum BaseType b = S->DeclFromType->Base;
            /* Scalar struct/union: DECL_STRUCT allocates owned blob.
             * Brace-init (bitfields) stays on tree. */
            if ((b == TypeStruct || b == TypeUnion) && S->DeclArraySize <= 1)
            {
                if (S->DeclInitCount > 0 || S->DeclInitVals != NULL)
                    return 0;
                return 1;
            }
            if (b == TypeStruct || b == TypeUnion)
                return 0;
            if (b == TypeFunction || b == TypeEnum || b == TypePointer
                || b == TypeArray)
                return 0;
        }
    }
    /* Switch lower present (case/default/break). Fallthrough is sequential
     * body walk; return-in-case terminates via IrLinRet. */
    /* Do-while: lower present but residual SEGV under EXEC=vm — tree. */
    if (S->Kind == IrStDelegate) return 0;
    if (!IrExprIntegerOk(S->Expr)) return 0;
    if (!IrStmtIntegerOk(S->ThenStmt)) return 0;
    if (!IrStmtIntegerOk(S->ElseStmt)) return 0;
    if (!IrStmtIntegerOk(S->BodyStmt)) return 0;
    if (!IrStmtIntegerOk(S->InitStmt)) return 0;
    if (!IrStmtIntegerOk(S->IncStmt)) return 0;
    if (S->Stmts != NULL)
        for (i = 0; i < S->StmtCount; i++)
            if (!IrStmtIntegerOk(S->Stmts[i])) return 0;
    return 1;
}


int IrFnBodyIntegerEligible(IrStmt *Body)
{
    return IrStmtIntegerOk(Body);
}


