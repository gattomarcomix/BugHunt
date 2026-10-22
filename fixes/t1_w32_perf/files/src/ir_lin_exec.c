/* ir_lin_exec.c — Step / IrExecLinLocals extracted from ir_lower.c */
#include "picoc.h"
#include "ir_lin_exec.h"
#include "ir_storage.h"
#include "ir_lin_tabs.h"
#include "ir_lin_light.h"
#include "ir_fncode.h"
#include "ir_runtime.h"
#include "ir_lin.h"
#include "interpreter.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int IrFnCodeNestedLightSafe(FnCodeSlot *CS);

extern struct ParseState *LinParser;
extern unsigned char LinRegU[IR_LIN_REGS];
extern int LinStepAbort;
extern int LinRetSeen;
extern long long LinRetVal;
extern int LinDiv0;
extern int LinExecDepth;
extern void *IdxGlobBase[32];
extern long long IdxSave[32];
extern void *IdxAddr[32];
extern int IdxEs[32];
extern int IdxEb[32];

extern long long GlobSave[32];
extern int GlobSaved;
extern char IrLinStructRetBuf[512];
extern const char *LinHostCallName;
extern const char *CallNameTab[IR_LIN_CALL_SLOTS];
extern unsigned GlobDirty;

/* ir_lower.c helpers used by Step — correct signatures */
int LinBreakHas(int Line);
void LinBreakClear(int Line);
void LinBreakAdd(int Line);
void LinPrintArg(long long *Locals, int NLocals, const char *Arg);
void LinPrintLocals(long long *Locals, int NLocals);
int LinTok2Unsigned(const IrLinInst *In);
void IrLinStructAssignBlob(IrCallFrame *Fr, int slot, void *Src, int sz);
int LinFramePush(void);
void LinFramePop(void);
void IrLinCaretOnLastBracket(struct ParseState *P, IrExpr *IX);
int IrSafeMemberAddr(struct ParseState *Parser, IrExpr *E, void **Addr, int *Es, int *IsUnsigned);

struct ValueType *IrLinLocalIndexedType(IrCallFrame *Fr, int Slot);
int IrDebugOn(void);
FnCodeSlot *FnCodeCompile(struct ParseState *Parser, IrStmt *Body, struct Value *Func);
int IsRel(enum LexToken Op);
int LinTryParamIndexedAccess(const IrLinInst *In, long long *R, int *Ip);

void Step(const IrLinInst *In, long long *R, long long *Locals, int NLocals, int *Ip)
{
    switch (In->Op)
    {
        case IrLinLine:
            /* Analysis §10.7: keep Parser line in sync for ProgramFail caret
             * on the linear/prod path (not only debug). */
            if (LinParser != NULL && In->Imm > 0)
                LinParser->Line = (int)In->Imm;
            if (IrDebugOn())
            {
                static int StepArm, NextArm, NextDepth, SeenDeep, FinishDepth;
                static int StoppedMain; /* stop once at main entry */
                const char *InFile = getenv("BABYC_DEBUG_IN");
                int Hit = 0;
                if (getenv("BABYC_FINISH") != NULL && IrCallDepth > FinishDepth)
                { SeenDeep = 1; FinishDepth = IrCallDepth; }
                /* stop at first line of main (depth 1) unless disabled */
                if (!StoppedMain && IrCallDepth <= 1 && getenv("BABYC_NO_STOP_MAIN") == NULL)
                { Hit = 1; StoppedMain = 1; }
                if (LinBreakHas((int)In->Imm))
                    Hit = 1;
                if (StepArm || getenv("BABYC_STEP") != NULL)
                    Hit = 1;
                if (NextArm && IrCallDepth <= NextDepth)
                    Hit = 1;
                if (getenv("BABYC_FINISH") != NULL && SeenDeep && IrCallDepth < FinishDepth)
                {
                    Hit = 1;
                    fprintf(stderr, "[babyc-debug] finish depth %d\n", IrCallDepth);
                }
                fprintf(stderr, "[babyc-debug] line %d depth %d%s\n",
                    (int)In->Imm, IrCallDepth, Hit ? " stop" : "");
                if (getenv("BABYC_BT") != NULL)
                    fprintf(stderr, "[babyc-debug] bt #%d %s %s:%d\n", IrCallDepth,
                (IrCallDepth > 0 && IrCallDepth < IR_CALL_MAX_DEPTH && IrDebugFunc[IrCallDepth - 1]) ? IrDebugFunc[IrCallDepth - 1] : "?",
                LinParser && LinParser->FileName ? LinParser->FileName : "?", (int)In->Imm);
                if (Hit && Locals != NULL)
                    LinPrintLocals(Locals, NLocals);
                StepArm = 0;
                if (NextArm && IrCallDepth <= NextDepth)
                    NextArm = 0;
                if (Hit && InFile != NULL)
                {
                    FILE *Df = fopen(InFile, "r");
                    char Cmd[64];
                    if (Df != NULL)
                    {
                        while (fgets(Cmd, sizeof(Cmd), Df) != NULL)
                        {
                            if (strncmp(Cmd, "step", 4) == 0)
                                StepArm = 1;
                            else if (strncmp(Cmd, "next", 4) == 0)
                            { NextArm = 1; NextDepth = IrCallDepth; }
                            else if (strncmp(Cmd, "continue", 8) == 0)
                            { StepArm = 0; NextArm = 0; }
                            else if (strncmp(Cmd, "break ", 6) == 0)
                            {
                                int ln = atoi(Cmd + 6);
                                LinBreakAdd(ln);
                                fprintf(stderr, "[babyc-debug] break %d\n", ln);
                            }
                            else if (strncmp(Cmd, "clear ", 6) == 0)
                            {
                                int ln = atoi(Cmd + 6);
                                LinBreakClear(ln);
                                fprintf(stderr, "[babyc-debug] clear %d\n", ln);
                            }
                            else if (strncmp(Cmd, "clear", 5) == 0)
                            {
                                LinBreakClear(0);
                                fprintf(stderr, "[babyc-debug] clear all\n");
                            }
                            else if (strncmp(Cmd, "print ", 6) == 0)
                            {
                                char *arg = Cmd + 6;
                                while (*arg == ' ') arg++;
                                { char *nl = arg; while (*nl && *nl != '\n' && *nl != '\r') nl++; *nl = 0; }
                                LinPrintArg(Locals, NLocals, arg);
                            }
                            else if (strncmp(Cmd, "locals", 6) == 0)
                                LinPrintLocals(Locals, NLocals);
                            fprintf(stderr, "[babyc-debug] cmd %s", Cmd);
                        }
                        fclose(Df);
                    }
                }
                else if (Hit && getenv("BABYC_DEBUG_TTY") != NULL && isatty(0))
                {
                    char Cmd[64];
                    fprintf(stderr, "[babyc-debug] (tty) ");
                    if (fgets(Cmd, sizeof(Cmd), stdin) != NULL)
                    {
                        if (strncmp(Cmd, "step", 4) == 0)
                            StepArm = 1;
                        else if (strncmp(Cmd, "next", 4) == 0)
                        { NextArm = 1; NextDepth = IrCallDepth; }
                        else if (strncmp(Cmd, "break ", 6) == 0)
                        {
                            int ln = atoi(Cmd + 6);
                            LinBreakAdd(ln);
                            fprintf(stderr, "[babyc-debug] break %d\n", ln);
                        }
                        else if (strncmp(Cmd, "print ", 6) == 0)
                        {
                            char *arg = Cmd + 6;
                            while (*arg == ' ') arg++;
                            { char *nl = arg; while (*nl && *nl != '\n' && *nl != '\r') nl++; *nl = 0; }
                            LinPrintArg(Locals, NLocals, arg);
                        }
                        else if (strncmp(Cmd, "locals", 6) == 0)
                            LinPrintLocals(Locals, NLocals);
                    }
                }
            }
            (*Ip)++;
            break;

        case IrLinImm:
            if (In->Tok == 4 && In->Imm >= 0 && In->Imm < IR_CALL_MAX_LOCALS)
            {
                /* Owned struct/union blob: LocalVal is already the payload
                 * address (&s → LocalVal, not &LocalVal cell). */
                if (IrCallDepth > 0
                    && IrCallStack[IrCallDepth - 1]->LocalOwned[In->Imm]
                    && IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm] != 0)
                    R[In->Dest] = IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm];
                else if (Locals != NULL && In->Imm < NLocals)
                    R[In->Dest] = (long long)(uintptr_t)&Locals[In->Imm];
                else if (IrCallDepth > 0)
                    R[In->Dest] = (long long)(uintptr_t)&IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm];
                else
                    R[In->Dest] = 0;
            }
            else if (In->Tok == 2 && In->Imm >= 0 && In->Imm < IR_LIN_STMT_MAX && LinWideOn[In->Imm])
                R[In->Dest] = LinWideImm[In->Imm];
            else if (In->Tok == 3 && In->Imm >= 0 && In->Imm < IR_LIN_STMT_MAX && LinWideOn[In->Imm])
                R[In->Dest] = LinWideImm[In->Imm];
            else
                R[In->Dest] = In->Imm;
            (*Ip)++;
            break;
        case IrLinLoadLocal:
            /* Imm < 0 → ParamVal[-(Imm+1)]; Imm >= 0 → LocalVal[Imm]. */
            if (In->Imm < 0)
            {
                int ps = (int)(-(In->Imm + 1));
                if (ps >= 0 && ps < IR_CALL_MAX_PARAMS)
                {
                    if (IrCallDepth > 0 && ps < IrCallStack[IrCallDepth - 1]->NumParams)
                        R[In->Dest] = IrCallStack[IrCallDepth - 1]->ParamVal[ps];
                    else if (LightSp > 0)
                        R[In->Dest] = LightParam[LightSp - 1][ps];
                    else if (IrCallDepth > 0)
                        R[In->Dest] = IrCallStack[IrCallDepth - 1]->ParamVal[ps];
                    else
                        R[In->Dest] = 0;
                }
                else
                    R[In->Dest] = 0;
                if (In->Dest < IR_LIN_REGS) LinRegU[In->Dest] = 0;
            }
            else if (In->Imm >= 0 && In->Imm < NLocals)
            {
                /* Uninit: prefer frame LocalInit; light bitmask only for pure
                 * leaf windows (no nested IrFramePush — IrCallDepth unchanged). */
                if (IrUninitChecksOn() && LinParser != NULL
                    && In->Imm >= 0 && In->Imm < 64)
                {
                    int slot = (int)In->Imm;
                    int uninit = 0;
                    if (IrCallDepth > 0 && slot < IR_CALL_MAX_LOCALS)
                    {
                        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                        /* Owned struct/union blob: LocalVal holds payload addr
                         * (always set at frame entry) — not a scalar uninit. */
                        if (!Fr->LocalIsPtr[slot] && !Fr->LocalIsFptr[slot]
                            && Fr->LocalArraySize[slot] <= 0
                            && !Fr->LocalOwned[slot]
                            && !Fr->LocalInit[slot])
                            uninit = 1;
                    }
                    else if (LightSp > 0
                             && (LightInitMask[LightSp - 1] & (1ULL << slot)) == 0)
                        uninit = 1;
                    if (uninit)
                    {
                        const char *nm = (IrCallDepth > 0 && slot < IR_CALL_MAX_LOCALS)
                            ? IrCallStack[IrCallDepth - 1]->LocalName[slot] : NULL;
                        if (nm) ParserSetCaretOnIdent(LinParser, nm);
                        if (nm)
                            ProgramFail(LinParser, "uninitialized variable '%s'", nm);
                        else
                            ProgramFail(LinParser, "uninitialized variable");
                    }
                }
                R[In->Dest] = Locals[In->Imm];
                /* RegFlags: mark dest UNSIGNED from LocalTyp. */
                if (In->Dest < IR_LIN_REGS)
                {
                    unsigned char u = 0;
                    if (IrCallDepth > 0 && In->Imm < IR_CALL_MAX_LOCALS)
                    {
                        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                        struct ValueType *T = Fr->LocalTyp[In->Imm];
                        if (T != NULL)
                        {
                            int b = T->Base;
                            if (b == TypeUnsignedChar || b == TypeUnsignedShort
                                || b == TypeUnsignedInt || b == TypeUnsignedLong
                                || b == TypeUnsignedLongLong)
                                u = 1;
                        }
                    }
                    LinRegU[In->Dest] = u;
                }
            }
            (*Ip)++;
            break;
        case IrLinStoreIndex:
        {
            void *Base = NULL;
            int eb = 0, es = 0;
            char *P = NULL;
            int im = (int)In->Imm;
            /* Tok=2 uses Imm as es, not IdxTab index — never bind IX. */
            IrExpr *IX = (In->Tok != 2 && im >= 0 && im < IdxN) ? IdxTab[im] : NULL;

            /* Tok=2: *p = v — value in R[A], pointer in R[B], es in Imm */
            if (In->Tok == 2)
            {
                void *Ptr = (void *)(intptr_t)R[In->B];
                es = (int)In->Imm;
                if (es == 1) eb = TypeChar;
                else if (es == 2) eb = TypeShort;
                else if (es == 8) eb = TypePointer; /* word-sized ptr / long long */
                else eb = TypeInt;
                if (Ptr)
                    IrStorePtrElem(LinParser, eb, Ptr, R[In->A]);
                /* Mark init when store lands in owned array OR scalar LocalVal cell
                 * (*q=v with q=&y; set(&x) with *p=v). Walk full call stack. */
                if (Ptr && IrCallDepth > 0 && es > 0)
                {
                    int di;
                    for (di = IrCallDepth - 1; di >= 0; di--)
                    {
                        IrCallFrame *Fr = IrCallStack[di];
                        int si, found = 0;
                        for (si = 0; si < IR_CALL_MAX_LOCALS; si++)
                        {
                            char *base;
                            int aes, an, asz, off, ei;
                            /* Scalar: Ptr is &LocalVal[si] (Tok=4 address) */
                            if (!Fr->LocalOwned[si] && Fr->LocalArraySize[si] <= 0
                                && !Fr->LocalIsPtr[si]
                                && (void *)&Fr->LocalVal[si] == Ptr)
                            {
                                Fr->LocalInit[si] = 1;
                                found = 1;
                                break;
                            }
                            if (!Fr->LocalOwned[si] || Fr->LocalVal[si] == 0)
                                continue;
                            base = (char *)(intptr_t)Fr->LocalVal[si];
                            aes = Fr->LocalElemSize[si] > 0 ? (int)Fr->LocalElemSize[si] : 0;
                            an = Fr->LocalArraySize[si];
                            /* Owned struct blob (ArraySize==0): Sizeof as range */
                            if (an <= 0)
                            {
                                struct ValueType *ST = Fr->LocalTyp[si];
                                if (ST == NULL) ST = Fr->LocalFromType[si];
                                if (ST != NULL && (ST->Base == TypeStruct
                                        || ST->Base == TypeUnion)
                                    && ST->Sizeof > 0)
                                {
                                    asz = (int)ST->Sizeof;
                                    if ((char *)Ptr < base || (char *)Ptr >= base + asz)
                                        continue;
                                    Fr->LocalInit[si] = 1;
                                    if (Fr->LocalInitMap[si] != NULL)
                                    {
                                        off = (int)((char *)Ptr - base);
                                        {
                                            int mln = IrFrameInitMapLen(Fr, si), i;
                                            for (i = 0; i < es && off + i < mln; i++)
                                                if (off + i >= 0)
                                                    Fr->LocalInitMap[si][off + i] = 1;
                                        }
                                    }
                                    found = 1;
                                    break;
                                }
                                continue;
                            }
                            if (aes <= 0) continue;
                            asz = an * aes;
                            if ((char *)Ptr < base || (char *)Ptr >= base + asz)
                                continue;
                            Fr->LocalInit[si] = 1;
                            if (Fr->LocalInitMap[si] != NULL)
                            {
                                off = (int)((char *)Ptr - base);
                                if (IrFrameIsByteMap(Fr, si))
                                {
                                    int mln = IrFrameInitMapLen(Fr, si), i;
                                    for (i = 0; i < es && off + i < mln; i++)
                                        if (off + i >= 0)
                                            Fr->LocalInitMap[si][off + i] = 1;
                                }
                                else
                                {
                                    ei = off / aes;
                                    if (ei >= 0 && ei < an)
                                        Fr->LocalInitMap[si][ei] = 1;
                                }
                            }
                            found = 1;
                            break;
                        }
                        if (found) break;
                    }
                }
                (*Ip)++;
                break;
            }
            /* Tok=3: memcpy(R[B], R[A], Imm) — struct by-value aos[i]=s / s=t */
            if (In->Tok == 3)
            {
                void *Src = (void *)(intptr_t)R[In->A];
                void *Dst = (void *)(intptr_t)R[In->B];
                int n = (int)In->Imm;
                if (Src != NULL && Dst != NULL && n > 0 && n <= 4096)
                    /* GUARD: IrCopyStructBlob (InitMap marked below) */
                    IrCopyStructBlob(Dst, Src, n, NULL, 0);
                if (IrCallDepth > 0 && Dst != NULL)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int si;
                    for (si = 0; si < IR_CALL_MAX_LOCALS; si++)
                    {
                        char *base;
                        int es, an, asz, off, ei, mln;
                        if (!Fr->LocalOwned[si] || Fr->LocalVal[si] == 0)
                            continue;
                        base = (char *)(intptr_t)Fr->LocalVal[si];
                        es = Fr->LocalElemSize[si] > 0 ? (int)Fr->LocalElemSize[si] : 0;
                        an = Fr->LocalArraySize[si];
                        if (an > 0 && es > 0)
                            asz = an * es;
                        else if (base == (char *)Dst)
                            asz = (es > 0 ? es : n);
                        else
                            continue;
                        if (asz <= 0) continue;
                        if ((char *)Dst < base || (char *)Dst >= base + asz)
                            continue;
                        Fr->LocalInit[si] = 1;
                        if (Fr->LocalInitMap[si] == NULL)
                            continue;
                        mln = IrFrameInitMapLen(Fr, si);
                        off = (int)((char *)Dst - base);
                        if (IrFrameIsByteMap(Fr, si) && mln > 0)
                        {
                            int i;
                            for (i = 0; i < n; i++)
                                if (off + i >= 0 && off + i < mln)
                                    Fr->LocalInitMap[si][off + i] = 1;
                        }
                        else if (es > 0 && an > 0)
                        {
                            ei = off / es;
                            if (ei >= 0 && ei < an)
                            {
                                /* Element map: size is an. Also tolerate
                                 * oversized maps by clamping. */
                                int cap = mln > 0 ? mln : an;
                                if (ei < cap)
                                    Fr->LocalInitMap[si][ei] = 1;
                            }
                        }
                        else if (mln > 0)
                            memset(Fr->LocalInitMap[si], 1, (size_t)mln);
                    }
                }
                (*Ip)++;
                break;
            }

            /* Meta-fast local STX */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 0
                && IdxMetaSlot[im] >= 0 && IrCallDepth > 0
                && IdxMetaSlot[im] < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int slot = IdxMetaSlot[im];
                es = Fr->LocalElemSize[slot] > 0 ? (int)Fr->LocalElemSize[slot]
                     : (IdxMetaEs[im] > 0 ? (int)IdxMetaEs[im] : 4);
                /* Scalar pointer cell only — not owned array-of-pointer (652). */
                if (Fr->LocalIsPtr[slot] && !Fr->LocalOwned[slot]
                    && Fr->LocalArraySize[slot] <= 0
                    && Fr->LocalPointeeSize[slot] > 0)
                    es = Fr->LocalPointeeSize[slot];
                if ((Fr->LocalOwned[slot] || Fr->LocalIsPtr[slot])
                    && Fr->LocalVal[slot] != 0 && es > 0)
                {
                    int n = Fr->LocalArraySize[slot];
                    if (n <= 0 && Fr->LocalOwned[slot] && im >= 0 && im < IdxN)
                        n = IdxMetaN[im];
                    char *Pbase = (char *)(intptr_t)Fr->LocalVal[slot];
                    long long nv = R[In->A];
                    long long idx = R[In->B];
                    if (n > 0 && (idx < 0 || idx >= (long long)n))
                    {
                        if (LinParser)
                            LinParser->Line = (IX && IX->SourceLine > 0)
                                ? IX->SourceLine : LinParser->Line;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)idx, n);
                    }
                    if (es == 1 || es == 2 || es == 4 || es == 8)
                        IrWriteByElemSize(Pbase + (ptrdiff_t)idx * es, es, nv);
                    else
                    {
                        eb = IrBaseFromElemSize(es, 0);
                        P = Pbase + (ptrdiff_t)idx * es;
                        IrStorePtrElem(LinParser, eb, P, nv);
                    }
                    if (Fr->LocalInitMap[slot] != NULL && idx >= 0
                        && (n <= 0 || idx < (long long)n)
                        && Fr->LocalArraySize[slot] > 0
                        && (int)idx < Fr->LocalArraySize[slot])
                        Fr->LocalInitMap[slot][(int)idx] = 1;
                    /* Param/local pointer into caller array: mark owner */
                    if (Fr->LocalIsPtr[slot] && !Fr->LocalOwned[slot] && es > 0)
                    {
                        char *Dest = Pbase + (ptrdiff_t)idx * es;
                        int di;
                        for (di = IrCallDepth - 1; di >= 0; di--)
                        {
                            IrCallFrame *Of = IrCallStack[di];
                            int oi, found = 0;
                            for (oi = 0; oi < IR_CALL_MAX_LOCALS; oi++)
                            {
                                char *ob;
                                int oes, on, oasz, ooff, oei;
                                if (!Of->LocalOwned[oi] || Of->LocalVal[oi] == 0)
                                    continue;
                                ob = (char *)(intptr_t)Of->LocalVal[oi];
                                oes = Of->LocalElemSize[oi] > 0 ? (int)Of->LocalElemSize[oi] : 0;
                                on = Of->LocalArraySize[oi];
                                if (on <= 0 || oes <= 0) continue;
                                oasz = on * oes;
                                if (Dest < ob || Dest >= ob + oasz) continue;
                                Of->LocalInit[oi] = 1;
                                if (Of->LocalInitMap[oi] != NULL)
                                {
                                    ooff = (int)(Dest - ob);
                                    if (IrFrameIsByteMap(Of, oi))
                                    {
                                        int mln = IrFrameInitMapLen(Of, oi), ii;
                                        for (ii = 0; ii < es && ooff + ii < mln; ii++)
                                            if (ooff + ii >= 0)
                                                Of->LocalInitMap[oi][ooff + ii] = 1;
                                    }
                                    else
                                    {
                                        oei = ooff / oes;
                                        if (oei >= 0 && oei < on)
                                            Of->LocalInitMap[oi][oei] = 1;
                                    }
                                }
                                found = 1;
                                break;
                            }
                            if (found) break;
                        }
                    }
                    (*Ip)++;
                    break;
                }
            }
            /* Native local-array STX */
            if (IX && IX->Kind == IrExIndex && IX->Left && IX->Left->Kind == IrExIdent
                && IX->Left->LocalSlot >= 0 && IrCallDepth > 0
                && IX->Left->LocalSlot < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int slot = IX->Left->LocalSlot;
                /* StoreIndex: index in B for some paths, A for others — use B if set shape */
                es = Fr->LocalElemSize[slot];
                if (es <= 0 && Fr->LocalFromType[slot] != NULL)
                    es = Fr->LocalFromType[slot]->Sizeof > 0
                        ? (int)Fr->LocalFromType[slot]->Sizeof : 0;
                if (Fr->LocalOwned[slot] && Fr->LocalVal[slot] != 0 && es > 0)
                {
                    int n = Fr->LocalArraySize[slot];
                    char *Pbase = (char *)(intptr_t)Fr->LocalVal[slot];
                    long long nv = R[In->A];  /* value */
                    long long idx = R[In->B]; /* index */
                    if (n > 0 && (idx < 0 || idx >= (long long)n))
                    {
                        if (LinParser) LinParser->Line = (IX->SourceLine > 0)
                            ? IX->SourceLine : LinParser->Line;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)idx, n);
                    }
                    /* GUARD: IrBaseFromElemSize */
                    eb = IrBaseFromElemSize(es, 0);
                    P = Pbase + (ptrdiff_t)idx * es;
                    IrStorePtrElem(LinParser, eb, P, nv);
                    if (Fr->LocalInitMap[slot] != NULL && idx >= 0
                        && (n <= 0 || idx < (long long)n)
                        && (int)idx < Fr->LocalArraySize[slot])
                        Fr->LocalInitMap[slot][(int)idx] = 1;
                    (*Ip)++;
                    break;
                }
            }
            /* P1: direct parameter-array store. IdxMetaPush marks kind 2 only
             * for a direct p[i] of a supported scalar width. Rechecking the
             * same AST shape here is redundant in the hot loop; retain the
             * slot bounds and runtime frame guards. */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 2
                && IdxMetaSlot[im] >= 0 && IdxMetaSlot[im] < IR_CALL_MAX_PARAMS
                && IX != NULL && LinParser)
            {
                int ps = IdxMetaSlot[im], pes = IdxMetaEs[im] > 0 ? IdxMetaEs[im] : 4;
                char *Pbase = NULL;
                int peb = TypeInt, typed_elem = 0;
                if (LightSp > 0)
                    Pbase = (char *)(intptr_t)LightParam[LightSp - 1][ps];
                else if (IrCallDepth > 0 && ps < IrCallStack[IrCallDepth - 1]->NumParams)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    struct ValueType *PT = Fr->ParamTyp[ps];
                    Pbase = (char *)(intptr_t)Fr->ParamVal[ps];
                    if (PT && PT->Base == TypePointer && PT->FromType)
                    {
                        pes = PT->FromType->Sizeof > 0 ? (int)PT->FromType->Sizeof : pes;
                        peb = PT->FromType->Base;
                        typed_elem = 1;
                    }
                }
                if (!typed_elem)
                    peb = pes == 1 ? TypeChar : pes == 2 ? TypeShort
                        : pes == 8 ? TypeLongLong : TypeInt;
                if (Pbase && pes > 0 && pes <= (int)sizeof(long long))
                {
                    if (peb == TypeChar && (IX->TypeFlags & IR_TF_UNSIGNED)) peb = TypeUnsignedChar;
                    else if (peb == TypeShort && (IX->TypeFlags & IR_TF_UNSIGNED)) peb = TypeUnsignedShort;
                    else if (peb == TypeInt && (IX->TypeFlags & IR_TF_UNSIGNED)) peb = TypeUnsignedInt;
                    else if (peb == TypeLong && (IX->TypeFlags & IR_TF_UNSIGNED)) peb = TypeUnsignedLong;
                    else if (peb == TypeLongLong && (IX->TypeFlags & IR_TF_UNSIGNED)) peb = TypeUnsignedLongLong;
                    P = Pbase + (ptrdiff_t)R[In->B] * pes;
                    IrStorePtrElem(LinParser, peb, P, R[In->A]);
                    /* The checked store performs the narrowing. Read the
                     * stored value back with a typed load, avoiding a second
                     * generic helper call in hot swap loops. */
                    if (pes == 1)
                        R[In->A] = (peb == TypeUnsignedChar)
                            ? (long long)*(const unsigned char *)P
                            : (long long)*(const signed char *)P;
                    else if (pes == 2)
                        R[In->A] = (peb == TypeUnsignedShort)
                            ? (long long)*(const unsigned short *)P
                            : (long long)*(const short *)P;
                    else if (pes == 4)
                        R[In->A] = (peb == TypeUnsignedInt || peb == TypeUnsignedLong)
                            ? (long long)*(const unsigned int *)P
                            : (long long)*(const int *)P;
                    else if (pes == 8)
                        R[In->A] = (peb == TypeUnsignedLong || peb == TypeUnsignedLongLong)
                            ? (long long)*(const unsigned long long *)P
                            : *(const long long *)P;
                    else
                        R[In->A] = IrLoadPtrElem(peb, P);
                    if (In->A < IR_LIN_REGS)
                        LinRegU[In->A] = (peb == TypeUnsignedChar || peb == TypeUnsignedShort
                            || peb == TypeUnsignedInt || peb == TypeUnsignedLong
                            || peb == TypeUnsignedLongLong);
                    /* Mark caller owned arrays OR scalar LocalVal cells containing P */
                    if (P != NULL && IrCallDepth > 0 && pes > 0)
                    {
                        int di;
                        for (di = IrCallDepth - 1; di >= 0; di--)
                        {
                            IrCallFrame *Of = IrCallStack[di];
                            int oi, found = 0;
                            for (oi = 0; oi < IR_CALL_MAX_LOCALS; oi++)
                            {
                                char *ob;
                                int oes, on, oasz, ooff, oei;
                                if (!Of->LocalOwned[oi] && Of->LocalArraySize[oi] <= 0
                                    && !Of->LocalIsPtr[oi]
                                    && (char *)&Of->LocalVal[oi] == P)
                                {
                                    Of->LocalInit[oi] = 1;
                                    found = 1;
                                    break;
                                }
                                if (!Of->LocalOwned[oi] || Of->LocalVal[oi] == 0)
                                    continue;
                                ob = (char *)(intptr_t)Of->LocalVal[oi];
                                oes = Of->LocalElemSize[oi] > 0 ? (int)Of->LocalElemSize[oi] : 0;
                                on = Of->LocalArraySize[oi];
                                if (on <= 0 || oes <= 0) continue;
                                oasz = on * oes;
                                if (P < ob || P >= ob + oasz) continue;
                                Of->LocalInit[oi] = 1;
                                if (Of->LocalInitMap[oi] != NULL)
                                {
                                    ooff = (int)(P - ob);
                                    if (IrFrameIsByteMap(Of, oi))
                                    {
                                        int mln = IrFrameInitMapLen(Of, oi), ii;
                                        for (ii = 0; ii < pes && ooff + ii < mln; ii++)
                                            if (ooff + ii >= 0)
                                                Of->LocalInitMap[oi][ooff + ii] = 1;
                                    }
                                    else
                                    {
                                        oei = ooff / oes;
                                        if (oei >= 0 && oei < on)
                                            Of->LocalInitMap[oi][oei] = 1;
                                    }
                                }
                                found = 1;
                                break;
                            }
                            if (found) break;
                        }
                    }
                    (*Ip)++;
                    break;
                }
            }
            /* Meta-fast global STX */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 1 && LinParser)
            {
                char *GBase = (char *)IdxGlobBase[im];
                int ges = IdxMetaEs[im] > 0 ? (int)IdxMetaEs[im] : 0;
                int gn = IdxMetaN[im];
                long long gidx = R[In->B];
                long long gnv = R[In->A];
                if (GBase == NULL || ges <= 0)
                {
                    void *gb = NULL;
                    int geb = 0;
                    IrExpr *RRoot = (IX && IX->Kind == IrExIndex) ? IX->Left : NULL;
                    while (RRoot != NULL && RRoot->Kind == IrExIndex)
                        RRoot = RRoot->Left;
                    if (RRoot != NULL
                        && IrResolvePtr(LinParser, RRoot, &gb, &geb, &ges)
                        && gb && ges > 0)
                    {
                        struct Value *GV = IrGlobalVal(LinParser, RRoot);
                        struct ValueType *T = GV ? GV->Typ : NULL;
                        int total = 1;
                        while (T != NULL && T->Base == TypeArray)
                        {
                            if (T->ArraySize <= 0 || total > 1048576 / T->ArraySize) { total = 0; break; }
                            total *= T->ArraySize;
                            T = T->FromType;
                        }
                        if (T == NULL || T->Sizeof <= 0 || T->Sizeof > (int)sizeof(long long))
                            GBase = NULL;
                        else
                        {
                            GBase = (char *)gb;
                            ges = (int)T->Sizeof; /* flattened leaf size, not row stride */
                            if (gn <= 0) gn = total;
                            IdxGlobBase[im] = GBase;
                            IdxMetaEs[im] = (unsigned char)(ges > 255 ? 255 : ges);
                            IdxMetaN[im] = gn;
                        }
                    }
                }
                if (GBase && ges > 0)
                {
                    if (!IrArrayIndexInBounds(gidx, gn) && IrSafeChecksOn())
                    {
                        if (IX && IX->SourceLine > 0)
                            LinParser->Line = IX->SourceLine;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)gidx, gn);
                    }
                    if (ges == 1 || ges == 2 || ges == 4 || ges == 8)
                        IrWriteByElemSize(GBase + (ptrdiff_t)gidx * ges, ges, gnv);
                    else
                        IrStorePtrElem(LinParser, TypeInt,
                            GBase + (ptrdiff_t)gidx * ges, gnv);
                    (*Ip)++;
                    break;
                }
            }
            /* Native global-array STX */
            if (IX && IX->Kind == IrExIndex && IX->Left && IX->Left->Kind == IrExIdent
                && IX->Left->LocalSlot < 0 && IX->Left->ParamSlot < 0
                && LinParser && IX->Left->Ident != NULL)
            {
                void *GBase = NULL;
                int geb = 0, ges = 0;
                if (IrResolvePtr(LinParser, IX->Left, &GBase, &geb, &ges)
                    && GBase && ges > 0)
                {
                    long long gidx = R[In->B];
                    long long gnv = R[In->A];
                    struct Value *GV = IrGlobalVal(LinParser, IX->Left);
                    int gn = 0;
                    if (GV && GV->Typ && GV->Typ->Base == TypeArray)
                        gn = GV->Typ->ArraySize;
                    if (!IrArrayIndexInBounds(gidx, gn) && IrSafeChecksOn())
                    {
                        if (IX->SourceLine > 0) LinParser->Line = IX->SourceLine;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)gidx, gn);
                    }
                    if (geb == 0)
                    {
                        /* GUARD: IrBaseFromElemSize */
                        geb = IrBaseFromElemSize(ges, 0);
                    }
                    P = (char *)GBase + (ptrdiff_t)gidx * ges;
                    IrStorePtrElem(LinParser, geb, P, gnv);
                    (*Ip)++;
                    break;
                }
            }
            if (LinParser && IX && IX->Kind == IrExMember
                && IX->Left != NULL && IX->Left->Kind == IrExIndex
                && IX->Left->Left != NULL
                && (IX->Left->Left->Kind == IrExIdent
                    || IX->Left->Left->Kind == IrExMember)
                && IX->Left->Right != NULL
                && IX->Left->Right->Kind != IrExConst)
            {
                /* g[i].m = v / L.pts[i].x = v with variable i */
                IrExpr *Ix = IX->Left;
                IrExpr *BaseId = Ix->Left;
                long long idx = R[In->B];
                void *arr = NULL;
                struct ValueType *ST = NULL;
                int es_arr = 0;
                struct Value *MV = NULL;
                if (BaseId->Kind == IrExMember)
                {
                    void *mbase = NULL;
                    int mes = 0, meb = 0;
                    if (IrSafeMemberAddr(LinParser, BaseId, &mbase, &mes, &meb) && mbase
                        && IrCallDepth > 0)
                    {
                        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                        struct Value *MM = NULL;
                        IrExpr *Id = BaseId->Left;
                        struct ValueType *LT = NULL;
                        while (Id != NULL && Id->Kind == IrExMember) Id = Id->Left;
                        if (Id != NULL && Id->Kind == IrExIdent)
                        {
                            if (Id->LocalSlot >= 0 && Id->LocalSlot < IR_CALL_MAX_LOCALS)
                            {
                                LT = Fr->LocalFromType[Id->LocalSlot];
                                if (LT == NULL) LT = Fr->LocalTyp[Id->LocalSlot];
                            }
                            else if (Id->Ident != NULL)
                            {
                                struct Value *GV = IrGlobalValSoft(LinParser, Id);
                                if (GV) LT = GV->Typ;
                            }
                            if (LT != NULL && LT->Members
                                && TableGet(LT->Members, BaseId->Ident, &MM, NULL, NULL, NULL)
                                && MM && MM->Typ && MM->Typ->Base == TypeArray
                                && MM->Typ->FromType)
                            {
                                arr = mbase;
                                ST = MM->Typ->FromType;
                                es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                            }
                        }
                    }
                }
                else if (BaseId->LocalSlot >= 0 && BaseId->LocalSlot < IR_CALL_MAX_LOCALS
                    && IrCallDepth > 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int ls = BaseId->LocalSlot;
                    arr = (void *)(intptr_t)Fr->LocalVal[ls];
                    es_arr = Fr->LocalElemSize[ls] > 0 ? (int)Fr->LocalElemSize[ls] : 0;
                    ST = Fr->LocalFromType[ls];
                    if (ST == NULL) ST = Fr->LocalTyp[ls];
                    if (ST != NULL && ST->Base == TypeArray && ST->FromType)
                        ST = ST->FromType;
                    if (es_arr <= 0 && ST != NULL)
                        es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                }
                else if (BaseId->Ident != NULL)
                {
                    struct Value *GV = IrGlobalVal(LinParser, BaseId);
                    if (GV != NULL && GV->Typ != NULL && GV->Val != NULL
                        && GV->Typ->Base == TypeArray && GV->Typ->FromType != NULL)
                    {
                        arr = (void *)GV->Val;
                        ST = GV->Typ->FromType;
                        es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                    }
                }
                if (arr != NULL && ST != NULL && ST->Members != NULL && es_arr > 0
                    && TableGet(ST->Members, IX->Ident, &MV, NULL, NULL, NULL)
                    && MV != NULL && MV->Val != NULL)
                {
                    P = (char *)arr + (ptrdiff_t)idx * es_arr + MV->Val->Integer;
                    es = 4; eb = TypeInt;
                    if (MV->Typ != NULL)
                    {
                        int base = MV->Typ->Base;
                        if (base == TypeChar || base == TypeUnsignedChar) { es = 1; eb = base; }
                        else if (base == TypeShort || base == TypeUnsignedShort) { es = 2; eb = base; }
                        else if (base == TypeFP) { es = TypeFPStorageSize(MV->Typ); eb = TypeFP; }
                        else if (base == TypeLongLong || base == TypeUnsignedLongLong
                                 || base == TypeLong || base == TypeUnsignedLong)
                        { es = (int)sizeof(long long); eb = base; }
                        else if (base == TypePointer) { es = (int)sizeof(void *); eb = base; }
                        else { es = 4; eb = base; }
                    }
                    if (MV->BitWidth > 0)
                    {
                        IX->BitOffset = MV->BitOffset;
                        IX->BitWidth = MV->BitWidth;
                        IX->BitSigned = !(MV->Typ != NULL
                            && (MV->Typ->Base == TypeUnsignedInt
                                || MV->Typ->Base == TypeUnsignedShort
                                || MV->Typ->Base == TypeUnsignedLong
                                || MV->Typ->Base == TypeUnsignedLongLong
                                || MV->Typ->Base == TypeUnsignedChar));
                    }
                }
                else
                    P = NULL;
            }
            else if (LinParser && IX && IX->Kind == IrExMember
                && IrSafeMemberAddr(LinParser, IX, (void **)&P, &es, &eb) && P)
                ;
            else if (LinParser && IX && IX->Kind == IrExIndex
                && IrResolvePtr(LinParser, IX->Left, &Base, &eb, &es) && Base && es > 0 && es <= (int)sizeof(long long))
                P = (char *)Base + (ptrdiff_t)R[In->B] * es;
            else if (IX && IX->Kind == IrExMember)
                P = NULL;
            /* *p = v: resolve Ident pointee (IrAddrOf skips plain Unary*Ident). */
            else if (LinParser && IX && IX->Kind == IrExUnary && IX->Op == TokenAsterisk
                && IX->Left != NULL
                && IrResolvePtr(LinParser, IX->Left, &Base, &eb, &es)
                && Base && es > 0 && es <= (int)sizeof(long long))
                P = (char *)Base;
            else if (LinParser && IX && IrAddrOf(LinParser, IX, (void **)&P, &eb, &es) && P && es > 0 && es <= (int)sizeof(long long))
                ;
            else
                P = NULL;
            if (P)
            {
                long long nv;
                if (IX && IX->Kind == IrExMember
                    && IrCallDepth > 0 && IX->Left != NULL
                    && IX->Left->Kind == IrExIdent
                    && IX->Left->LocalSlot >= 0
                    && IX->Left->LocalSlot < IR_CALL_MAX_LOCALS
                    && IrCallStack[IrCallDepth - 1]->LocalIsConst[IX->Left->LocalSlot])
                {
                    if (LinParser != NULL)
                    {
                        if (IX->SourceLine > 0)
                            LinParser->Line = IX->SourceLine;
                        ProgramFail(LinParser, "assignment of read-only variable");
                    }
                }
                if (IX && IX->Kind == IrExMember && IX->BitWidth > 0)
                {
                    /* Bitfield store: insert into container word. */
                    IdxAddr[In->Imm] = P;
                    IdxEs[In->Imm] = 4;
                    IdxEb[In->Imm] = TypeInt;
                    IdxDirty |= 1u << In->Imm;
                    IdxSave[In->Imm] = IrLoadPtrElem(TypeInt, P);
                    IrBitInsert(P, IX->BitOffset, IX->BitWidth, R[In->A]);
                    nv = IrBitExtract(P, IX->BitOffset, IX->BitWidth, IX->BitSigned);
                    R[In->A] = nv;
                }
                else
                {
                if (eb == 0 && es == 1) eb = TypeChar;
                else if (eb == 0 && es == 2) eb = TypeShort;
                else if (eb == 0 && es == 8) eb = TypeLongLong;
                else if (eb == 0) eb = TypeInt;
                IdxAddr[In->Imm] = P;
                IdxEs[In->Imm] = es > 0 ? es : 4;
                IdxEb[In->Imm] = eb;
                IdxDirty |= 1u << In->Imm;
                IdxSave[In->Imm] = IrLoadPtrElem(eb, P);
                IrStorePtrElem(LinParser, eb, P, R[In->A]);
                nv = IrLoadPtrElem(eb, P);
                R[In->A] = nv;
                }
                /* Prod path (EXEC=vm) skips tree: mark init map so SAFE stays accurate. */
                if (IX && IrCallDepth > 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int ls = -1;
                    if (IX->Kind == IrExMember && IX->Left)
                    {
                        if (IX->Left->Kind == IrExIdent)
                            ls = IX->Left->LocalSlot;
                        else if (IX->Left->Kind == IrExIndex && IX->Left->Left
                                 && IX->Left->Left->Kind == IrExIdent)
                            ls = IX->Left->Left->LocalSlot; /* v[i].m */
                        else if (IX->Left->Kind == IrExMember
                                 || (IX->Left->Kind == IrExIndex && IX->Left->Left
                                     && IX->Left->Left->Kind == IrExMember))
                        {
                            /* Nested o.in.x / L.p[i].y — walk to root Ident */
                            IrExpr *R = IX->Left;
                            while (R != NULL && (R->Kind == IrExMember || R->Kind == IrExIndex))
                                R = R->Left;
                            if (R != NULL && R->Kind == IrExIdent)
                                ls = R->LocalSlot;
                        }
                    }
                    else if (IX->Kind == IrExUnary && IX->Op == TokenAsterisk
                        && IX->Left && IX->Left->Kind == IrExIdent)
                        ls = IX->Left->LocalSlot;
                    else if (IX->Kind == IrExIndex && IX->Left && IX->Left->Kind == IrExIdent)
                        ls = IX->Left->LocalSlot;
                    if (ls >= 0 && ls < IR_CALL_MAX_LOCALS)
                    {
                        Fr->LocalInit[ls] = 1;
                        IrFrameMarkBytes(Fr, ls, P, es > 0 ? es : 4);
                    }
                }
            }
            (*Ip)++;
            break;
        }
        case IrLinLoadIndex:
        {
            void *Addr = NULL, *Base = NULL;
            int eb = 0, es = 0;
            int im = (int)In->Imm;
            /* Tok=2 uses Imm as es; Tok=7 uses B as member AST metadata. */
            IrExpr *IX = In->Tok == 7
                ? ((int)In->B < IdxN ? IdxTab[In->B] : NULL)
                : ((In->Tok != 2 && im >= 0 && im < IdxN) ? IdxTab[im] : NULL);
            /* Tok=2/7: direct pointer load; Tok=7 retains the member AST for diagnostics. */
            if (In->Tok == 2 || In->Tok == 7)
            {
                void *Ptr = (void *)(intptr_t)R[In->A];
                es = (int)In->Imm;
                { int uns = In->Tok == 7
                        ? (IX != NULL && (IX->TypeFlags & IR_TF_UNSIGNED) != 0)
                        : LinTok2Unsigned(In);
                  if (es == 1) eb = uns ? TypeUnsignedChar : TypeChar;
                  else if (es == 2) eb = uns ? TypeUnsignedShort : TypeShort;
                  else if (es == 8) eb = TypePointer;
                  else eb = uns ? TypeUnsignedInt : TypeInt;
                }
                if (Ptr == NULL && IrSafeChecksOn() && LinParser != NULL)
                {
                    if (IX && IX->SourceLine > 0)
                        LinParser->Line = IX->SourceLine;
                    ProgramFail(LinParser, "NULL pointer dereference");
                }
                /* Check every VM scalar load against owned struct byte maps.
                 * The effective address is authoritative: member-of-array
                 * (v[i].field) and array-of-member (s.arr[i]) have opposite
                 * expression-tree shapes but share the same byte map. */
                if (Ptr != NULL && IrUninitChecksOn() && IrCallDepth > 0)
                {
                    int di, hit = 0;
                    for (di = IrCallDepth - 1; di >= 0 && !hit; di--)
                    {
                        IrCallFrame *Fr = IrCallStack[di];
                        int si;
                        for (si = 0; si < IR_CALL_MAX_LOCALS; si++)
                        {
                            char *base;
                            int asz = 0, off;
                            if (!Fr->LocalOwned[si] || Fr->LocalVal[si] == 0)
                                continue;
                            base = (char *)(intptr_t)Fr->LocalVal[si];
                            /* Resolve every frame-owned object class, not just
                             * struct blobs: direct Tok=2/7 loads can also alias
                             * primitive arrays and address-taken scalar locals. */
                            {
                                struct ValueType *LT = Fr->LocalFromType[si]
                                    ? Fr->LocalFromType[si] : Fr->LocalTyp[si];
                                if (IrFrameIsByteMap(Fr, si))
                                    asz = IrFrameInitMapLen(Fr, si);
                                else if (Fr->LocalArraySize[si] > 0
                                         && Fr->LocalElemSize[si] > 0)
                                    asz = Fr->LocalArraySize[si]
                                        * (int)Fr->LocalElemSize[si];
                                else if (LT != NULL && LT->Sizeof > 0)
                                    asz = (int)LT->Sizeof;
                                else if (LT != NULL && LT->Base == TypePointer)
                                    asz = (int)sizeof(void *);
                                else if (Fr->LocalOwned[si] && Fr->LocalArraySize[si] <= 0)
                                    asz = 4;
                            }
                            if (asz <= 0) continue;
                            if ((char *)Ptr < base || (char *)Ptr >= base + asz)
                                continue;
                            hit = 1;
                            off = (int)((char *)Ptr - base);
                            {
                                int ok = 1, mln = IrFrameInitMapLen(Fr, si);
                                if (IrFrameIsByteMap(Fr, si))
                                {
                                    /* GUARD: IrInitMapBytesReady */
                                    if (!IrInitMapBytesReady(Fr->LocalInitMap[si], off, es, mln))
                                        ok = 0;
                                }
                                else if (Fr->LocalArraySize[si] > 0
                                         && Fr->LocalElemSize[si] > 0)
                                {
                                    int ei = off / (int)Fr->LocalElemSize[si];
                                    if (Fr->LocalInitMap[si] == NULL
                                        && Fr->LocalArraySize[si] < 65536)
                                    {
                                        Fr->LocalInitMap[si] = (unsigned char *)
                                            calloc((size_t)Fr->LocalArraySize[si], 1);
                                        mln = Fr->LocalInitMap[si] != NULL
                                            ? Fr->LocalArraySize[si] : 0;
                                    }
                                    if (Fr->LocalInitMap[si] != NULL && ei >= 0
                                        && ei < mln && Fr->LocalInitMap[si][ei] == 0)
                                        ok = 0;
                                }
                                else if (!Fr->LocalInit[si])
                                    ok = 0;
                                if (!ok && LinParser != NULL)
                                {
                                    const char *vname = NULL;
                                    IrExpr *R = IX; int has_mem = 0;
                                    if (IX && IX->SourceLine > 0)
                                        LinParser->Line = IX->SourceLine;
                                    while (R != NULL && (R->Kind == IrExIndex || R->Kind == IrExMember)
                                           && R->Left != NULL) {
                                        if (R->Kind == IrExMember) has_mem = 1;
                                        R = R->Left;
                                    }
                                    if (IX && IX->Kind == IrExMember) has_mem = 1;
                                    if (R != NULL && R->Kind == IrExIdent && R->Ident)
                                        vname = R->Ident;
                                    else if (IX && IX->Ident)
                                        vname = IX->Ident;
                                    else if (Fr->LocalName[si])
                                        vname = Fr->LocalName[si];
                                    if (has_mem && IX && IX->Ident)
                                        ParserSetCaretOnIdent(LinParser, IX->Ident);
                                    else if (IX && IX->Kind == IrExIndex && vname)
                                    {
                                        ParserSetCaretOnIndexAccess(LinParser, vname);
                                        LinParser->CharacterPos++;
                                    }
                                    else if (vname)
                                        ParserSetCaretOnIdent(LinParser, vname);
                                    ProgramFail(LinParser, "uninitialized variable");
                                }
                            }
                            break;
                        }
                    }
                }
                if (Ptr != NULL)
                    R[In->Dest] = IrLoadPtrElem(eb, Ptr);
                else
                    R[In->Dest] = 0;
                (*Ip)++;
                break;
            }
            /* Tok=6: s.arr[i] load — addr in R[A], es in B, meta=Index for uninit. */
            if (In->Tok == 6)
            {
                void *Ptr = (void *)(intptr_t)R[In->A];
                es = (int)In->B;
                if (es != 1 && es != 2 && es != 4 && es != 8) es = 4;
                /* GUARD: IrBaseFromElemSize */
                eb = IrBaseFromElemSize(es, 0);
                /* es==1 (char[]): host strcpy often wrote without map mark — skip. */
                if (Ptr != NULL && es >= 2 && IrUninitChecksOn() && IrCallDepth > 0)
                {
                    int di, hit = 0;
                    for (di = IrCallDepth - 1; di >= 0 && !hit; di--)
                    {
                        IrCallFrame *Fr = IrCallStack[di];
                        int si;
                        for (si = 0; si < IR_CALL_MAX_LOCALS; si++)
                        {
                            char *base;
                            int asz = 0, off;
                            if (!Fr->LocalOwned[si] || Fr->LocalVal[si] == 0)
                                continue;
                            base = (char *)(intptr_t)Fr->LocalVal[si];
                            if (Fr->LocalFromType[si] != NULL
                                && (Fr->LocalFromType[si]->Base == TypeStruct
                                    || Fr->LocalFromType[si]->Base == TypeUnion)
                                && Fr->LocalFromType[si]->Sizeof > 0)
                                asz = (int)Fr->LocalFromType[si]->Sizeof;
                            else if (Fr->LocalTyp[si] != NULL
                                && (Fr->LocalTyp[si]->Base == TypeStruct
                                    || Fr->LocalTyp[si]->Base == TypeUnion)
                                && Fr->LocalTyp[si]->Sizeof > 0)
                                asz = (int)Fr->LocalTyp[si]->Sizeof;
                            if (asz <= 0) continue;
                            if ((char *)Ptr < base || (char *)Ptr >= base + asz)
                                continue;
                            hit = 1;
                            if (Fr->LocalInitMap[si] != NULL)
                            {
                                off = (int)((char *)Ptr - base);
                                {
                                    int mln = IrFrameInitMapLen(Fr, si), i, ok = 1;
                                    for (i = 0; i < es && off + i < mln; i++)
                                        if (off + i >= 0 && Fr->LocalInitMap[si][off + i] == 0)
                                        { ok = 0; break; }
                                    if (!ok && LinParser != NULL)
                                    {
                                        const char *base = NULL;
                                        IrExpr *R = IX; int has_mem = 0;
                                        if (IX && IX->SourceLine > 0)
                                            LinParser->Line = IX->SourceLine;
                                        while (R != NULL && (R->Kind == IrExIndex || R->Kind == IrExMember)
                                               && R->Left != NULL) {
                                            if (R->Kind == IrExMember) has_mem = 1;
                                            R = R->Left;
                                        }
                                        if (IX && IX->Kind == IrExMember) has_mem = 1;
                                        if (R != NULL && R->Kind == IrExIdent && R->Ident)
                                            base = R->Ident;
                                        else if (IX && IX->Ident)
                                            base = IX->Ident;
                                        else if (Fr->LocalName[si])
                                            base = Fr->LocalName[si];
                                        if (has_mem && IX && IX->Ident)
                                            ParserSetCaretOnIdent(LinParser, IX->Ident);
                                        else if (base)
                                            ParserSetCaretOnIdent(LinParser, base);
                                        ProgramFail(LinParser, "uninitialized variable");
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
                if (Ptr == NULL && IrSafeChecksOn() && LinParser != NULL)
                {
                    if (IX && IX->SourceLine > 0)
                        LinParser->Line = IX->SourceLine;
                    ProgramFail(LinParser, "NULL pointer dereference");
                }
                if (Ptr != NULL)
                    R[In->Dest] = IrLoadPtrElem(eb, Ptr);
                else
                    R[In->Dest] = 0;
                (*Ip)++;
                break;
            }
            /* Tok=5: member array base address (&s.arr / &aos[i].arr).
             * Const outer index: IrSafeMemberAddr. Variable: R[In->A] as idx. */
            if (In->Tok == 5 && LinParser && IX && IX->Kind == IrExMember)
            {
                void *P = NULL;
                int mes = 0, meb = 0;
                if (IrSafeMemberAddr(LinParser, IX, &P, &mes, &meb) && P)
                    R[In->Dest] = (long long)(uintptr_t)P;
                else if (IX->Left != NULL && IX->Left->Kind == IrExIndex
                         && IX->Left->Left != NULL
                         && IX->Left->Left->Kind == IrExIdent
                         && IX->Left->Right != NULL
                         && IX->Left->Right->Kind != IrExConst
                         && IrCallDepth > 0)
                {
                    /* aos[var].m — index in R[A], same layout as const path. */
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    IrExpr *Id = IX->Left->Left;
                    long long idx = R[In->A];
                    int ls = Id->LocalSlot;
                    void *arr = NULL;
                    int es_arr = 0;
                    struct ValueType *ST = NULL;
                    struct Value *MV = NULL;
                    if (ls >= 0 && ls < IR_CALL_MAX_LOCALS)
                    {
                        es_arr = Fr->LocalElemSize[ls] > 0
                            ? (int)Fr->LocalElemSize[ls] : 0;
                        ST = Fr->LocalFromType[ls];
                        if (ST == NULL) ST = Fr->LocalTyp[ls];
                        if (ST != NULL && ST->Base == TypeArray && ST->FromType)
                            ST = ST->FromType;
                        arr = (void *)(intptr_t)Fr->LocalVal[ls];
                        if (es_arr <= 0 && ST != NULL)
                            es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                    }
                    if (arr != NULL && ST != NULL && es_arr > 0
                        && ST->Members != NULL
                        && TableGet(ST->Members, IX->Ident, &MV, NULL, NULL, NULL)
                        && MV != NULL && MV->Val != NULL)
                    {
                        P = (char *)arr + (ptrdiff_t)idx * es_arr
                            + (ptrdiff_t)MV->Val->Integer;
                        R[In->Dest] = (long long)(uintptr_t)P;
                    }
                    else
                        R[In->Dest] = 0;
                }
                else
                    R[In->Dest] = 0;
                (*Ip)++;
                break;
            }
            /* Meta-fast local LDX */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 0
                && IdxMetaSlot[im] >= 0 && IrCallDepth > 0
                && IdxMetaSlot[im] < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int slot = IdxMetaSlot[im];
                long long idx = R[In->A];
                int n = Fr->LocalArraySize[slot];
                if (n <= 0 && Fr->LocalOwned[slot] && im >= 0 && im < IdxN)
                    n = IdxMetaN[im];
                es = Fr->LocalElemSize[slot] > 0 ? (int)Fr->LocalElemSize[slot]
                     : (IdxMetaEs[im] > 0 ? (int)IdxMetaEs[im] : 4);
                if ((Fr->LocalOwned[slot] || Fr->LocalIsPtr[slot])
                    && Fr->LocalVal[slot] != 0 && es > 0)
                {
                    char *base = (char *)(intptr_t)Fr->LocalVal[slot];
                    if (!IrArrayIndexInBounds(idx, n) && IrSafeChecksOn())
                    {
                        if (IX && IX->SourceLine > 0 && LinParser)
                            LinParser->Line = IX->SourceLine;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)idx, n);
                    }
                    /* Element uninit (239): map allocated by DECL_ARRAY or lazy. */
                    if (IrUninitChecksOn() && n > 0 && idx >= 0 && idx < (long long)n)
                    {
                        if (Fr->LocalInitMap[slot] == NULL && n < 65536)
                        {
                            unsigned char *imap = (unsigned char *)calloc((size_t)n, 1);
                            if (imap != NULL)
                                Fr->LocalInitMap[slot] = imap;
                        }
                        if (Fr->LocalInitMap[slot] != NULL
                            && !Fr->LocalInitMap[slot][(int)idx])
                        {
                            if (LinParser && IX && IX->SourceLine > 0)
                                LinParser->Line = IX->SourceLine;
                            if (LinParser && IX) {
                                IrExpr *R = IX; int has_mem = 0, ndim = 0;
                                while (R != NULL && (R->Kind == IrExIndex || R->Kind == IrExMember)) {
                                    if (R->Kind == IrExMember) has_mem = 1;
                                    if (R->Kind == IrExIndex) ndim++;
                                    R = R->Left;
                                }
                                {
                                    const char *id = NULL;
                                    if (R != NULL && R->Kind == IrExIdent && R->Ident)
                                        id = R->Ident;
                                    else if (IX->Left && IX->Left->Ident)
                                        id = IX->Left->Ident;
                                    if (has_mem && id)
                                        ParserSetCaretOnIdent(LinParser, id);
                                    else if (id && !has_mem && ndim >= 2)
                                        IrLinCaretOnLastBracket(LinParser, IX);
                                    else if (id)
                                        ParserSetCaretOnIdent(LinParser, id);
                                }
                            }
                            ProgramFail(LinParser, "uninitialized variable");
                        }
                    }
                    /* GUARD: IrLoadByElemSize — no parallel es-switch. */
                    {
                        struct ValueType *ET = IrLinLocalIndexedType(Fr, slot);
                        int is_unsigned = ET != NULL && IrTypeIsUnsignedBase(ET->Base);
                        if (es == 1 || es == 2 || es == 4 || es == 8)
                            R[In->Dest] = IrLoadByElemSize(
                                base + (ptrdiff_t)idx * es, es, is_unsigned);
                        else
                        {
                            eb = IrBaseFromElemSize(es, is_unsigned);
                            R[In->Dest] = IrLoadPtrElem(eb, base + (ptrdiff_t)idx * es);
                        }
                        if (In->Dest < IR_LIN_REGS)
                            LinRegU[In->Dest] = (unsigned char)is_unsigned;
                    }
                    (*Ip)++;
                    break;
                }
            }
            /* Native local-array LDX: base+size from frame, OOB same as tree. */
            if (IX && IX->Kind == IrExIndex && IX->Left && IX->Left->Kind == IrExIdent
                && IX->Left->LocalSlot >= 0 && IrCallDepth > 0
                && IX->Left->LocalSlot < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int slot = IX->Left->LocalSlot;
                long long idx = R[In->A];
                int n = Fr->LocalArraySize[slot];
                es = Fr->LocalElemSize[slot];
                if (es <= 0 && Fr->LocalFromType[slot] != NULL)
                    es = Fr->LocalFromType[slot]->Sizeof > 0
                        ? (int)Fr->LocalFromType[slot]->Sizeof : 0;
                if ((Fr->LocalOwned[slot] || Fr->LocalIsPtr[slot])
                    && Fr->LocalVal[slot] != 0 && es > 0)
                {
                    char *Pbase = (char *)(intptr_t)Fr->LocalVal[slot];
                    if (n > 0 && (idx < 0 || idx >= (long long)n))
                    {
                        if (LinParser) LinParser->Line = (IX->SourceLine > 0)
                            ? IX->SourceLine : (LinParser->Line);
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)idx, n);
                    }
                    if (IrUninitChecksOn() && n > 0 && idx >= 0 && idx < (long long)n)
                    {
                        if (Fr->LocalInitMap[slot] == NULL && n < 65536)
                        {
                            /* Lazy map: bare int a[N] may skip DECL_ARRAY map alloc */
                            unsigned char *imap = (unsigned char *)calloc((size_t)n, 1);
                            if (imap != NULL)
                                Fr->LocalInitMap[slot] = imap;
                        }
                        if (Fr->LocalInitMap[slot] != NULL
                            && (int)idx < Fr->LocalArraySize[slot]
                            && !Fr->LocalInitMap[slot][(int)idx])
                        {
                            if (LinParser) LinParser->Line = (IX->SourceLine > 0)
                                ? IX->SourceLine : LinParser->Line;
                            if (IX != NULL) {
                                IrExpr *R = IX; int has_mem = 0, ndim = 0;
                                while (R != NULL && (R->Kind == IrExIndex || R->Kind == IrExMember)) {
                                    if (R->Kind == IrExMember) has_mem = 1;
                                    if (R->Kind == IrExIndex) ndim++;
                                    R = R->Left;
                                }
                                {
                                    const char *id = NULL;
                                    if (R != NULL && R->Kind == IrExIdent && R->Ident)
                                        id = R->Ident;
                                    else if (IX->Left && IX->Left->Ident)
                                        id = IX->Left->Ident;
                                    if (has_mem && id)
                                        ParserSetCaretOnIdent(LinParser, id);
                                    else if (id && !has_mem && ndim >= 2)
                                        IrLinCaretOnLastBracket(LinParser, IX);
                                    else if (id)
                                        ParserSetCaretOnIdent(LinParser, id);
                                }
                            }
                            ProgramFail(LinParser, "uninitialized variable");
                        }
                    }
                    if (es == 4 && !(IX->TypeFlags & IR_TF_UNSIGNED))
                        R[In->Dest] = (long long)((int *)Pbase)[idx];
                    else if (es == 1 && !(IX->TypeFlags & IR_TF_UNSIGNED))
                        R[In->Dest] = (long long)((signed char *)Pbase)[idx];
                    else
                    {
                        /* GUARD: IrBaseFromElemSize */
                        eb = IrBaseFromElemSize(es, 0);
                        if (IX->TypeFlags & IR_TF_UNSIGNED)
                        {
                            if (eb == TypeChar) eb = TypeUnsignedChar;
                            else if (eb == TypeShort) eb = TypeUnsignedShort;
                            else if (eb == TypeInt) eb = TypeUnsignedInt;
                            else if (eb == TypeLongLong) eb = TypeUnsignedLongLong;
                        }
                        R[In->Dest] = IrLoadPtrElem(eb, Pbase + (ptrdiff_t)idx * es);
                    }
                    (*Ip)++;
                    break;
                }
            }
            /* Meta-fast parameter-pointer LDX. IdxMetaPush marks kind 2 only
             * for a direct scalar p[i], so avoid a repeated AST-shape walk in
             * the hot loop. The frame supplies the actual pointer/type. */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 2
                && IdxMetaSlot[im] >= 0 && IdxMetaSlot[im] < IR_CALL_MAX_PARAMS
                && IX != NULL)
            {
                int ps = IdxMetaSlot[im];
                char *Pbase = NULL;
                long long idx = R[In->A];
                int es = IdxMetaEs[im] > 0 ? (int)IdxMetaEs[im] : 4;
                int eb = TypeInt, typed_elem = 0;
                if (LightSp > 0)
                    Pbase = (char *)(intptr_t)LightParam[LightSp - 1][ps];
                else if (IrCallDepth > 0
                         && ps < IrCallStack[IrCallDepth - 1]->NumParams)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    struct ValueType *PT = Fr->ParamTyp[ps];
                    Pbase = (char *)(intptr_t)Fr->ParamVal[ps];
                    if (PT && PT->Base == TypePointer && PT->FromType)
                    {
                        es = PT->FromType->Sizeof > 0 ? (int)PT->FromType->Sizeof : es;
                        eb = PT->FromType->Base;
                        typed_elem = 1;
                    }
                }
                if (!typed_elem)
                    eb = es == 1 ? TypeChar : es == 2 ? TypeShort
                        : es == 8 ? TypeLongLong : TypeInt;
                if (Pbase != NULL && es > 0 && es <= (int)sizeof(long long))
                {
                    /* In a light frame only the pointer value is stored. The
                     * lowered expression still carries signedness; combine it
                     * with the known element width for a correct typed load. */
                    if (IX->TypeFlags & IR_TF_UNSIGNED)
                    {
                        if (eb == TypeChar) eb = TypeUnsignedChar;
                        else if (eb == TypeShort) eb = TypeUnsignedShort;
                        else if (eb == TypeInt) eb = TypeUnsignedInt;
                        else if (eb == TypeLong) eb = TypeUnsignedLong;
                        else if (eb == TypeLongLong) eb = TypeUnsignedLongLong;
                    }
                    /* Keep common widths as direct loads; use the typed helper
                     * for unusual widths/types so sign/zero extension is explicit. */
                    if (es == 1)
                        R[In->Dest] = (eb == TypeUnsignedChar)
                            ? (long long)((const unsigned char *)Pbase)[idx]
                            : (long long)((const signed char *)Pbase)[idx];
                    else if (es == 2)
                        R[In->Dest] = (eb == TypeUnsignedShort)
                            ? (long long)((const unsigned short *)Pbase)[idx]
                            : (long long)((const short *)Pbase)[idx];
                    else if (es == 4)
                        R[In->Dest] = (eb == TypeUnsignedInt || eb == TypeUnsignedLong)
                            ? (long long)((const unsigned int *)Pbase)[idx]
                            : (long long)((const int *)Pbase)[idx];
                    else if (es == 8)
                        R[In->Dest] = (eb == TypeUnsignedLong || eb == TypeUnsignedLongLong)
                            ? (long long)((const unsigned long long *)Pbase)[idx]
                            : ((const long long *)Pbase)[idx];
                    else
                        R[In->Dest] = IrLoadPtrElem(eb, Pbase + (ptrdiff_t)idx * es);
                    if (In->Dest < IR_LIN_REGS)
                        LinRegU[In->Dest] = (eb == TypeUnsignedChar || eb == TypeUnsignedShort
                            || eb == TypeUnsignedInt || eb == TypeUnsignedLong
                            || eb == TypeUnsignedLongLong);
                    (*Ip)++;
                    break;
                }
            }
            /* kind 4: pointer-to-pointer param index (argv[i]). Load void* word. */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 4
                && IdxMetaSlot[im] >= 0 && IdxMetaSlot[im] < IR_CALL_MAX_PARAMS)
            {
                int ps = IdxMetaSlot[im];
                char *Pbase = NULL;
                long long idx = R[In->A];
                int es = (int)sizeof(void *);
                if (LightSp > 0)
                    Pbase = (char *)(intptr_t)LightParam[LightSp - 1][ps];
                else if (IrCallDepth > 0
                         && ps < IrCallStack[IrCallDepth - 1]->NumParams)
                    Pbase = (char *)(intptr_t)IrCallStack[IrCallDepth - 1]->ParamVal[ps];
                if (Pbase != NULL)
                {
                    R[In->Dest] = (long long)(intptr_t)((void **)Pbase)[idx];
                    if (In->Dest < IR_LIN_REGS)
                        LinRegU[In->Dest] = 0;
                    (*Ip)++;
                    break;
                }
            }
            /* Meta-fast global LDX with base cache.
             * Nested multi-dim (flattened at lower): IdxMetaEs/N already leaf
             * element size + total flat count — resolve base from root Ident. */
            if (im >= 0 && im < IdxN && IdxMetaKind[im] == 1 && LinParser)
            {
                char *GBase = (char *)IdxGlobBase[im];
                int ges = IdxMetaEs[im] > 0 ? (int)IdxMetaEs[im] : 0;
                int gn = IdxMetaN[im];
                long long gidx = R[In->A];
                if (GBase == NULL || ges <= 0)
                {
                    void *gb = NULL;
                    int geb = 0, rges = 0;
                    IrExpr *RRoot = (IX && IX->Kind == IrExIndex) ? IX->Left : NULL;
                    while (RRoot != NULL && RRoot->Kind == IrExIndex)
                        RRoot = RRoot->Left;
                    if (RRoot == NULL && IX && IX->Kind == IrExIndex)
                        RRoot = IX->Left;
                    if (RRoot != NULL
                        && IrResolvePtr(LinParser, RRoot, &gb, &geb, &rges)
                        && gb != NULL)
                    {
                        GBase = (char *)gb;
                        IdxGlobBase[im] = GBase;
                        /* Keep precomputed leaf es/N from IdxMetaPush when set. */
                        if (ges <= 0 && rges > 0)
                        {
                            ges = rges;
                            IdxMetaEs[im] = (unsigned char)(ges > 255 ? 255 : ges);
                        }
                        if (gn <= 0)
                        {
                            struct Value *GV = IrGlobalVal(LinParser, RRoot);
                            if (GV && GV->Typ && GV->Typ->Base == TypeArray)
                            {
                                struct ValueType *T = GV->Typ;
                                int total = 1;
                                while (T != NULL && T->Base == TypeArray)
                                {
                                    if (T->ArraySize > 0) total *= T->ArraySize;
                                    if (T->FromType != NULL && T->FromType->Base != TypeArray
                                        && ges <= 0)
                                    {
                                        ges = T->FromType->Sizeof > 0
                                            ? (int)T->FromType->Sizeof : 4;
                                        IdxMetaEs[im] = (unsigned char)(ges > 255 ? 255 : ges);
                                    }
                                    T = T->FromType;
                                }
                                gn = total;
                                IdxMetaN[im] = gn;
                            }
                        }
                    }
                }
                if (GBase && ges > 0)
                {
                    if (!IrArrayIndexInBounds(gidx, gn) && IrSafeChecksOn())
                    {
                        if (IX && IX->SourceLine > 0)
                            LinParser->Line = IX->SourceLine;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)gidx, gn);
                    }
                    /* The global fast path knows the element stride, but stride
                     * alone does not encode signedness (e.g. unsigned short and
                     * short are both two bytes). Recover the actual leaf type
                     * from the declaration before loading; otherwise 65535 is
                     * sign-extended to -1 only in the linear VM path. */
                    {
                        int geb = TypeInt;
                        IrExpr *Leaf = IX;
                        struct Value *GV = NULL;
                        struct ValueType *T;
                        while (Leaf != NULL && Leaf->Kind == IrExIndex)
                            Leaf = Leaf->Left;
                        if (Leaf != NULL && Leaf->Kind == IrExIdent)
                            GV = IrGlobalVal(LinParser, Leaf);
                        T = GV != NULL ? GV->Typ : NULL;
                        T = IrTypePeelArray(T);
                        if (T != NULL)
                            geb = T->Base;
                        R[In->Dest] = IrLoadPtrElem(geb,
                            GBase + (ptrdiff_t)gidx * ges);
                        if (In->Dest < IR_LIN_REGS)
                            LinRegU[In->Dest] = (geb == TypeUnsignedChar
                                || geb == TypeUnsignedShort || geb == TypeUnsignedInt
                                || geb == TypeUnsignedLong || geb == TypeUnsignedLongLong);
                    }
                    (*Ip)++;
                    break;
                }
            }
            /* Native global-array LDX */
            if (IX && IX->Kind == IrExIndex && IX->Left && IX->Left->Kind == IrExIdent
                && IX->Left->LocalSlot < 0 && IX->Left->ParamSlot < 0
                && LinParser && IX->Left->Ident != NULL)
            {
                void *GBase = NULL;
                int geb = 0, ges = 0;
                if (IrResolvePtr(LinParser, IX->Left, &GBase, &geb, &ges)
                    && GBase && ges > 0)
                {
                    long long gidx = R[In->A];
                    struct Value *GV = IrGlobalVal(LinParser, IX->Left);
                    int gn = 0;
                    if (GV && GV->Typ && GV->Typ->Base == TypeArray)
                        gn = GV->Typ->ArraySize;
                    /* seed meta cache */
                    if (im >= 0 && im < 32)
                    {
                        IdxGlobBase[im] = GBase;
                        IdxMetaEs[im] = (unsigned char)(ges > 255 ? 255 : ges);
                        IdxMetaN[im] = gn;
                        IdxMetaKind[im] = 1;
                    }
                    if (!IrArrayIndexInBounds(gidx, gn) && IrSafeChecksOn())
                    {
                        if (IX->SourceLine > 0) LinParser->Line = IX->SourceLine;
                        if (IX != NULL && IX->Left != NULL && IX->Left->Ident != NULL)
                            ParserSetCaretOnIdent(LinParser, IX->Left->Ident);
                        ProgramFail(LinParser,
                            "array index %d out of bounds (size %d)",
                            (int)gidx, gn);
                    }
                    if (geb == 0)
                    {
                        /* GUARD: IrBaseFromElemSize */
                        geb = IrBaseFromElemSize(ges, 0);
                    }
                    if (IX->TypeFlags & IR_TF_UNSIGNED)
                    {
                        if (geb == TypeChar) geb = TypeUnsignedChar;
                        else if (geb == TypeShort) geb = TypeUnsignedShort;
                        else if (geb == TypeInt) geb = TypeUnsignedInt;
                    }
                    if (ges == 4 && !(IX->TypeFlags & IR_TF_UNSIGNED))
                        R[In->Dest] = (long long)((int *)GBase)[gidx];
                    else
                        R[In->Dest] = IrLoadPtrElem(geb,
                            (char *)GBase + (ptrdiff_t)gidx * ges);
                    (*Ip)++;
                    break;
                }
            }
            /* Tok=2: pointer in R[A], Imm = element size (*f() path). */
            if (In->Tok == 2)
            {
                void *P = (void *)(intptr_t)R[In->A];
                es = (int)In->Imm;
                /* GUARD: IrBaseFromElemSize + Tok2 unsigned */
                eb = IrBaseFromElemSize(es, LinTok2Unsigned(In) ? 1 : 0);
                if (P == NULL && IrSafeChecksOn() && LinParser != NULL)
                {
                    if (IX != NULL && IX->SourceLine > 0)
                        LinParser->Line = IX->SourceLine;
                    else if (LinParser->SourceText != NULL)
                    {
                        /* Find last "return *p" / "*p" line for caret (303). */
                        const char *s = LinParser->SourceText, *lp;
                        int line = 1, last = LinParser->Line;
                        for (lp = s; *lp; lp++)
                        {
                            if (*lp == '\n') { line++; continue; }
                            if (lp[0]=='*' && (lp==s || lp[-1]==' ' || lp[-1]=='=' || lp[-1]=='('))
                                last = line;
                        }
                        LinParser->Line = last;
                    }
                    ProgramFail(LinParser, "NULL pointer dereference");
                }
                R[In->Dest] = P ? IrLoadPtrElem(eb, P) : 0;
                (*Ip)++;
                break;
            }
            if (In->Tok == 1 && LinParser && IX && IX->Kind == IrExMember
                && IX->Left != NULL && IX->Left->Kind == IrExIndex
                && IX->Left->Left != NULL
                && (IX->Left->Left->Kind == IrExIdent
                    || IX->Left->Left->Kind == IrExMember)
                && IX->Left->Right != NULL
                && IX->Left->Right->Kind != IrExConst)
            {
                /* g[i].m / G.pts[i].m with variable i */
                IrExpr *Ix = IX->Left;
                IrExpr *BaseId = Ix->Left;
                long long idx = R[In->A];
                void *arr = NULL;
                struct ValueType *ST = NULL;
                int es_arr = 0;
                struct Value *MV = NULL;
                if (BaseId->Kind == IrExMember)
                {
                    void *mbase = NULL;
                    int mes = 0, meb = 0;
                    if (IrSafeMemberAddr(LinParser, BaseId, &mbase, &mes, &meb) && mbase)
                    {
                        struct Value *MM = NULL;
                        IrExpr *Id = BaseId->Left;
                        struct ValueType *LT = NULL;
                        while (Id != NULL && Id->Kind == IrExMember) Id = Id->Left;
                        if (Id != NULL && Id->Kind == IrExIdent && IrCallDepth > 0)
                        {
                            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                            if (Id->LocalSlot >= 0 && Id->LocalSlot < IR_CALL_MAX_LOCALS)
                            {
                                LT = Fr->LocalFromType[Id->LocalSlot];
                                if (LT == NULL) LT = Fr->LocalTyp[Id->LocalSlot];
                            }
                            else if (Id->Ident != NULL)
                            {
                                struct Value *GV = IrGlobalValSoft(LinParser, Id);
                                if (GV) LT = GV->Typ;
                            }
                            if (LT != NULL && LT->Members
                                && TableGet(LT->Members, BaseId->Ident, &MM, NULL, NULL, NULL)
                                && MM && MM->Typ && MM->Typ->Base == TypeArray
                                && MM->Typ->FromType)
                            {
                                arr = mbase;
                                ST = MM->Typ->FromType;
                                es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                            }
                        }
                    }
                }
                else if (BaseId->LocalSlot >= 0 && BaseId->LocalSlot < IR_CALL_MAX_LOCALS
                    && IrCallDepth > 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int ls = BaseId->LocalSlot;
                    arr = (void *)(intptr_t)Fr->LocalVal[ls];
                    es_arr = Fr->LocalElemSize[ls] > 0 ? (int)Fr->LocalElemSize[ls] : 0;
                    ST = Fr->LocalFromType[ls];
                    if (ST == NULL) ST = Fr->LocalTyp[ls];
                    if (ST != NULL && ST->Base == TypeArray && ST->FromType)
                        ST = ST->FromType;
                    if (es_arr <= 0 && ST != NULL)
                        es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                }
                else if (BaseId->Ident != NULL)
                {
                    struct Value *GV = IrGlobalVal(LinParser, BaseId);
                    if (GV != NULL && GV->Typ != NULL && GV->Val != NULL
                        && GV->Typ->Base == TypeArray && GV->Typ->FromType != NULL)
                    {
                        arr = (void *)GV->Val;
                        ST = GV->Typ->FromType;
                        es_arr = ST->Base == TypeFP ? TypeFPStorageSize(ST)
                                    : (ST->Sizeof > 0 ? (int)ST->Sizeof : 0);
                    }
                }
                if (arr != NULL && ST != NULL && ST->Members != NULL && es_arr > 0
                    && TableGet(ST->Members, IX->Ident, &MV, NULL, NULL, NULL)
                    && MV != NULL && MV->Val != NULL)
                {
                    Addr = (char *)arr + (ptrdiff_t)idx * es_arr + MV->Val->Integer;
                    es = 4; eb = TypeInt;
                    if (MV->Typ != NULL)
                    {
                        int base = MV->Typ->Base;
                        if (base == TypeChar || base == TypeUnsignedChar) es = 1;
                        else if (base == TypeShort || base == TypeUnsignedShort) es = 2;
                        else if (base == TypeLongLong || base == TypeUnsignedLongLong
                                 || base == TypeLong || base == TypeUnsignedLong
                                 || base == TypeFP || base == TypePointer)
                            es = (base == TypeFP) ? TypeFPStorageSize(MV->Typ)
                                 : (base == TypePointer) ? (int)sizeof(void *)
                                 : (int)sizeof(long long);
                        else es = 4;
                        eb = base;
                    }
                    if (MV->BitWidth > 0)
                    {
                        IX->BitOffset = MV->BitOffset;
                        IX->BitWidth = MV->BitWidth;
                        IX->BitSigned = !(MV->Typ != NULL
                            && (MV->Typ->Base == TypeUnsignedInt
                                || MV->Typ->Base == TypeUnsignedShort
                                || MV->Typ->Base == TypeUnsignedLong
                                || MV->Typ->Base == TypeUnsignedLongLong
                                || MV->Typ->Base == TypeUnsignedChar));
                    }
                    if (IX->BitWidth > 0)
                        R[In->Dest] = IrBitExtract(Addr, IX->BitOffset, IX->BitWidth,
                                                   IX->BitSigned);
                    else
                        R[In->Dest] = IrLoadPtrElem(eb, Addr);
                    if (eb == TypeFP || (IX->TypeFlags & IR_TF_FP))
                        ; /* IEEE bits already in Dest */
                    (*Ip)++;
                    break;
                }
                /* fall through to IrSafeMemberAddr / zero */
            }
            if (In->Tok == 1 && LinParser && IX
                && IrSafeMemberAddr(LinParser, IX, &Addr, &es, &eb) && Addr)
            {
                if (IrUninitChecksOn() && IrCallDepth > 0 && IX->Left != NULL)
                {
                    IrExpr *Rident = IX->Left;
                    /* Walk Index/Member to root Ident (L.p[i].x). */
                    while (Rident != NULL
                           && (Rident->Kind == IrExIndex || Rident->Kind == IrExMember))
                        Rident = Rident->Left;
                    if (Rident != NULL && Rident->Kind == IrExIdent
                        && Rident->LocalSlot >= 0
                        && Rident->LocalSlot < IR_CALL_MAX_LOCALS)
                    {
                        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                        int ls = Rident->LocalSlot;
                        int sz = es > 0 ? es : 4;
                        if (IX->BitWidth > 0) sz = 4;
                        /* Skip pure pointer formals (pointee not in local blob).
                         * Array-of-struct Decl sets LocalIsPtr=1 but owns blob+InitMap. */
                        if (Fr->LocalInitMap[ls] != NULL && Fr->LocalVal[ls] != 0
                            && (Fr->LocalOwned[ls] || !Fr->LocalIsPtr[ls])
                            && !IrFrameLeafInit(Fr, ls, Addr, sz))
                        {
                            if (IX->SourceLine > 0)
                                LinParser->Line = IX->SourceLine;
                            {
                                const char *base = NULL;
                                IrExpr *R = IX; int has_mem = 0;
                                while (R != NULL && (R->Kind == IrExIndex || R->Kind == IrExMember)
                                       && R->Left != NULL) {
                                    if (R->Kind == IrExMember) has_mem = 1;
                                    R = R->Left;
                                }
                                if (IX->Kind == IrExMember) has_mem = 1;
                                if (R != NULL && R->Kind == IrExIdent && R->Ident)
                                    base = R->Ident;
                                else if (IX->Ident)
                                    base = IX->Ident;
                                if (has_mem && IX->Ident)
                                    ParserSetCaretOnIdent(LinParser, IX->Ident);
                                else if (has_mem)
                                    ParserSetCaretOnIdent(LinParser, base ? base : "?");
                                else
                                    ParserSetCaretOnIndexAccess(LinParser, base ? base : "?");
                            }
                            ProgramFail(LinParser, "uninitialized variable");
                        }
                    }
                }
                /* Bitfield: extract field, not full word (eliminates soft-mm). */
                if (IX->BitWidth > 0)
                {
                    R[In->Dest] = IrBitExtract(Addr, IX->BitOffset, IX->BitWidth,
                                               IX->BitSigned);
                    if (In->Dest < IR_LIN_REGS)
                        LinRegU[In->Dest] = IX->BitSigned ? 0 : 1;
                }
                else
                {
                    /* eb already signed/unsigned from member Typ (+ TypeFlags). */
                    if (eb == 0)
                    {
                        /* GUARD: IrBaseFromElemSize */
                        eb = IrBaseFromElemSize(es, 0);
                    }
                    R[In->Dest] = IrLoadPtrElem(eb, Addr);
                    if (In->Dest < IR_LIN_REGS)
                        LinRegU[In->Dest] = (eb == TypeUnsignedChar
                            || eb == TypeUnsignedShort || eb == TypeUnsignedInt
                            || eb == TypeUnsignedLong || eb == TypeUnsignedLongLong
                            || (IX->TypeFlags & IR_TF_UNSIGNED)) ? 1 : 0;
                }
            }
            else if (IX && IX->Kind == IrExMember)
            {
                if (getenv("BABYC_MEM_TRACE"))
                    fprintf(stderr, "[mem] abort Member ident=%s leftKind=%d tok=%d imm=%d IdxN=%d\n",
                        IX->Ident?IX->Ident:"?", IX->Left?IX->Left->Kind:-1,
                        In->Tok, (int)In->Imm, IdxN);
                LinStepAbort = 1;
                break;
            }
            else if (LinParser && IX && IX->Kind == IrExIndex && IX->Left != NULL
                && IrResolvePtr(LinParser, IX->Left, &Base, &eb, &es)
                && Base && es > 0 && es <= (int)sizeof(long long))
            {
                /* Use linear index in R[A] — do not re-eval index expr. */
                char *P = (char *)Base + (ptrdiff_t)R[In->A] * es;
                if (eb == 0 && es == 1) eb = TypeChar;
                else if (eb == 0 && es == 2) eb = TypeShort;
                else if (eb == 0 && es == 8) eb = TypeLongLong;
                else if (eb == 0) eb = TypeInt;
                /* Annotate UNSIGNED on Index → zero-extend (analysis §10.4). */
                if (IX->TypeFlags & IR_TF_UNSIGNED)
                {
                    if (eb == TypeChar) eb = TypeUnsignedChar;
                    else if (eb == TypeShort) eb = TypeUnsignedShort;
                    else if (eb == TypeInt) eb = TypeUnsignedInt;
                    else if (eb == TypeLong) eb = TypeUnsignedLong;
                    else if (eb == TypeLongLong) eb = TypeUnsignedLongLong;
                }
                R[In->Dest] = IrLoadPtrElem(eb, P);
            }
            /* *p plain: IrAddrOf does not handle Unary*Ident; resolve Ident. */
            else if (LinParser && IX && IX->Kind == IrExUnary && IX->Op == TokenAsterisk
                && IX->Left != NULL
                && IrResolvePtr(LinParser, IX->Left, &Base, &eb, &es)
                && Base && es > 0 && es <= (int)sizeof(long long))
            {
                if (eb == 0 && es == 1) eb = TypeChar;
                else if (eb == 0 && es == 2) eb = TypeShort;
                else if (eb == 0 && es == 8) eb = TypeLongLong;
                else if (eb == 0) eb = TypeInt;
                if (IX->TypeFlags & IR_TF_UNSIGNED)
                {
                    if (eb == TypeChar) eb = TypeUnsignedChar;
                    else if (eb == TypeShort) eb = TypeUnsignedShort;
                    else if (eb == TypeInt) eb = TypeUnsignedInt;
                    else if (eb == TypeLong) eb = TypeUnsignedLong;
                    else if (eb == TypeLongLong) eb = TypeUnsignedLongLong;
                }
                R[In->Dest] = IrLoadPtrElem(eb, Base);
            }
            else if (LinParser && IX && IrAddrOf(LinParser, IX, &Addr, &eb, &es) && Addr && es > 0 && es <= (int)sizeof(long long))
            {
                if (eb == 0 && es == 1) eb = TypeChar;
                else if (eb == 0 && es == 2) eb = TypeShort;
                else if (eb == 0 && es == 8) eb = TypeLongLong;
                else if (eb == 0) eb = TypeInt;
                R[In->Dest] = IrLoadPtrElem(eb, Addr);
            }
            else
                R[In->Dest] = 0;
            (*Ip)++;
            break;
        }
        case IrLinLoadGlobal:
        {
            int gi = (int)In->Imm;
            struct Value *V = (gi >= 0 && gi < GlobN && GlobVal[gi] != NULL)
                ? GlobVal[gi]
                : ((LinParser && gi >= 0 && gi < GlobN)
                    ? IrGlobalVal(LinParser, GlobTab[gi]) : NULL);
            if (V != NULL && gi >= 0 && gi < 32 && GlobVal[gi] == NULL)
                GlobVal[gi] = V; /* cache for next time */
            if (V != NULL && V->Val != NULL
                && V->Typ != NULL
                && (V->Typ->Base == TypeArray
                    || V->Typ->Base == TypeStruct
                    || V->Typ->Base == TypeUnion))
            {
                /* Address of global array/struct storage (decay / &g).
                 * TypePointer (stdout FILE*, char*) keeps IrValueAsInt64. */
                R[In->Dest] = (long long)(intptr_t)(void *)V->Val;
            }
            else if (V != NULL && V->Val != NULL && In->Tok == 1
                     && V->Typ != NULL && V->Typ->Base == TypePointer)
                R[In->Dest] = (long long)(intptr_t)V->Val->Pointer;
            else
                R[In->Dest] = V ? IrValueAsInt64(V) : 0;
            (*Ip)++;
            break;
        }
        case IrLinStoreGlobal:
        {
            int gi = (int)In->Imm;
            struct Value *V = (gi >= 0 && gi < GlobN && GlobVal[gi] != NULL)
                ? GlobVal[gi]
                : ((LinParser && gi >= 0 && gi < GlobN)
                    ? IrGlobalVal(LinParser, GlobTab[gi]) : NULL);
            if (V != NULL && V->Val != NULL && gi >= 0 && gi < 32)
            {
                long long nv = R[In->A];
                enum BaseType b = (V->Typ != NULL) ? V->Typ->Base : TypeInt;
                if (GlobVal[gi] == NULL)
                    GlobVal[gi] = V;
                GlobSave[gi] = IrValueAsInt64(V);
                GlobDirty |= 1u << gi;
                GlobSaved = 1;
                /* Direct scalar store: no string-literal / heap checks (globals). */
                switch (b)
                {
                    case TypeChar: V->Val->Character = (char)nv; break;
                    case TypeUnsignedChar: V->Val->UnsignedCharacter = (unsigned char)nv; break;
                    case TypeShort: V->Val->ShortInteger = (short)nv; break;
                    case TypeUnsignedShort: V->Val->UnsignedShortInteger = (unsigned short)nv; break;
                    case TypeInt: V->Val->Integer = (int)nv; break;
                    case TypeUnsignedInt: V->Val->UnsignedInteger = (unsigned)nv; break;
                    case TypeLong: V->Val->LongInteger = (long)nv; break;
                    case TypeUnsignedLong: V->Val->UnsignedLongInteger = (unsigned long)nv; break;
                    case TypeLongLong: V->Val->LongLongInteger = nv; break;
                    case TypeUnsignedLongLong: V->Val->UnsignedLongLongInteger = (unsigned long long)nv; break;
                    case TypePointer: V->Val->Pointer = (void *)(intptr_t)nv; break;
                    default: V->Val->Integer = (int)nv; break;
                }
                V->IsInitialized = TRUE;
            }
            (*Ip)++;
            break;
        }
            break;
        case IrLinStoreLocal:
            if (In->Imm < 0)
            {
                int ps = (int)(-(In->Imm + 1));
                long long nv = R[In->A];
                if (ps >= 0 && ps < IR_CALL_MAX_PARAMS)
                {
                    if (LightSp > 0)
                        LightParam[LightSp - 1][ps] = nv;
                    else if (IrCallDepth > 0)
                    {
                        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                        struct ValueType *PT = Fr->ParamTyp[ps];
                        if (PT != NULL)
                        {
                            int b = PT->Base;
                            if (b == TypeUnsignedShort)
                                nv = (long long)(unsigned short)nv;
                            else if (b == TypeShort)
                                nv = (long long)(short)nv;
                            else if (b == TypeUnsignedChar)
                                nv = (long long)(unsigned char)nv;
                            else if (b == TypeChar)
                                nv = (long long)(char)nv;
                        }
                        Fr->ParamVal[ps] = nv;
                    }
                    R[In->A] = nv;
                }
            }
            else if (In->Imm >= 0 && In->Imm < NLocals)
            {
                long long nv = R[In->A];
                int slot = (int)In->Imm;
                if (LightSp > 0)
                {
                    if (slot < 64)
                        LightInitMask[LightSp - 1] |= (1ULL << slot);
                    if (slot < LIGHT_LOCALS)
                        LightLocal[LightSp - 1][slot] = nv;
                    if (slot < NLocals)
                        Locals[slot] = nv;
                    /* Light owns init via LightInitMask only (no caller LocalInit). */
                    R[In->A] = nv;
                }
                else if (IrCallDepth > 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    {
                        int is_struct = (slot < IR_CALL_MAX_LOCALS && Fr->LocalOwned[slot]
                            && Fr->LocalArraySize[slot] == 0
                            && Fr->LocalTyp[slot] != NULL
                            && (Fr->LocalTyp[slot]->Base == TypeStruct
                                || Fr->LocalTyp[slot]->Base == TypeUnion));
                        if (is_struct)
                        {
                            /* s2 = s1 / t = make(): nv is source blob address */
                            int sz = Fr->LocalTyp[slot]->Sizeof;
                            if (nv != 0 && sz > 0)
                                IrLinStructAssignBlob(Fr, slot, (void *)(intptr_t)nv, sz);
                            R[In->A] = Fr->LocalVal[slot];
                        }
                        else
                        {
                            if (getenv("BABYC_FP_TRACE") && (Fr->LocalIsFptr[slot] || In->Tok == 5))
                                fprintf(stderr, "[fp] StoreLocal slot=%d tok=%d isfp=%d nv=%llx\n",
                                    slot, (int)In->Tok, (int)Fr->LocalIsFptr[slot],
                                    (unsigned long long)nv);
                            if (In->Tok != 5)
                                nv = IrNarrowToLocalType(Fr, slot, nv);
                            Locals[slot] = nv;
                            if (slot < IR_CALL_MAX_LOCALS)
                                Fr->LocalVal[slot] = nv;
                            R[In->A] = nv;
                            if (slot < IR_CALL_MAX_LOCALS)
                            {
                                Fr->LocalInit[slot] = 1;
                                if (In->Tok == 5)
                                {
                                    Fr->LocalIsFptr[slot] = 1;
                                    Fr->LocalIsPtr[slot] = 0;
                                }
                            }
                        }
                    }
                    if (0) /* LocalInit already set above for both branches */
                    {
                        Fr->LocalInit[slot] = 1;
                        /* Runtime fat-pointer bind (never at lower-time). */
                        if (Fr->LocalIsPtr[slot])
                        {
                            int si;
                            Fr->LocalArraySize[slot] = 0;
                            for (si = 0; si < Fr->NumLocals && si < IR_CALL_MAX_LOCALS; si++)
                            {
                                int es, n;
                                long long base;
                                if (!Fr->LocalOwned[si] || Fr->LocalArraySize[si] <= 0)
                                    continue;
                                es = Fr->LocalElemSize[si] > 0
                                    ? (int)Fr->LocalElemSize[si] : 4;
                                base = Fr->LocalVal[si];
                                if (base == 0 || es <= 0) continue;
                                if (nv == base)
                                {
                                    Fr->LocalArraySize[slot] = Fr->LocalArraySize[si];
                                    Fr->LocalElemSize[slot] = (unsigned char)es;
                                    break;
                                }
                                if (nv > base && ((nv - base) % (long long)es) == 0)
                                {
                                    long long off = (nv - base) / es;
                                    n = Fr->LocalArraySize[si];
                                    if (off >= 0 && off < (long long)n)
                                    {
                                        Fr->LocalArraySize[slot] = n - (int)off;
                                        Fr->LocalElemSize[slot] = (unsigned char)es;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    Locals[slot] = nv;
                    R[In->A] = nv;
                }
            }
            (*Ip)++;
            break;
        case IrLinRet:
            LinRetSeen = 1;
            LinRetVal = R[In->A];
            /* Struct/union return-by-value: copy owned local blob to stable
             * static buffer before the callee frame is freed. */
            if (IrCallDepth > 0 && LinRetVal != 0)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int li;
                for (li = 0; li < IR_CALL_MAX_LOCALS; li++)
                {
                    if (Fr->LocalOwned[li] && Fr->LocalVal[li] == LinRetVal)
                    {
                        int sz = 0;
                        void *Src = (void *)(intptr_t)LinRetVal;
                        if (Fr->LocalFromType[li] != NULL
                            && (Fr->LocalFromType[li]->Base == TypeStruct
                                || Fr->LocalFromType[li]->Base == TypeUnion))
                            sz = Fr->LocalFromType[li]->Sizeof;
                        else if (Fr->LocalTyp[li] != NULL
                            && (Fr->LocalTyp[li]->Base == TypeStruct
                                || Fr->LocalTyp[li]->Base == TypeUnion))
                            sz = Fr->LocalTyp[li]->Sizeof;
                        else if (Fr->LocalArraySize[li] <= 0
                            && Fr->LocalElemSize[li] > 4)
                            sz = (int)Fr->LocalElemSize[li];
                        if (Src != NULL && sz > 0 && sz <= (int)sizeof(IrLinStructRetBuf))
                        {
                            memcpy(IrLinStructRetBuf, Src, (size_t)sz);
                            LinRetVal = (long long)(intptr_t)IrLinStructRetBuf;
                        }
                        break;
                    }
                }
            }
            *Ip = -1;
            break;
        case IrLinCall:
        {
            int ci = (int)In->Imm;
            long long args[8];
            int ai, na;
            struct Value *FV;
            if (ci < 0 || ci >= CallN || LinParser == NULL)
            {
                R[In->Dest] = 0;
                (*Ip)++;
                break;
            }
            FV = CallFnTab[ci];
            na = CallArgN[ci];
            if (na > IR_LIN_CALL_ARGS) na = IR_LIN_CALL_ARGS;
            for (ai = 0; ai < na; ai++)
                args[ai] = R[CallArgReg[ci][ai]];
            /* Local fptr / tab[i]: Value* from LocalVal or pre-loaded reg. */
            if (ci >= 0 && ci < IR_LIN_CALL_SLOTS)
            {
                long long bits = 0;
                int got = 0;
                if (CallIndirectReg[ci] >= 0 && CallIndirectReg[ci] < IR_LIN_REGS)
                {
                    bits = R[CallIndirectReg[ci]];
                    got = 1;
                }
                else if (CallIndirectParam[ci] >= 0
                         && CallIndirectParam[ci] < IR_CALL_MAX_PARAMS)
                {
                    int ps = CallIndirectParam[ci];
                    if (LightSp > 0)
                        bits = LightParam[LightSp - 1][ps];
                    else if (IrCallDepth > 0)
                        bits = IrCallStack[IrCallDepth - 1]->ParamVal[ps];
                    got = 1;
                }
                else if (CallIndirectSlot[ci] >= 0)
                {
                    int islot = CallIndirectSlot[ci];
                    if (LightSp > 0 && islot < LIGHT_LOCALS)
                        bits = LightLocal[LightSp - 1][islot];
                    else if (IrCallDepth > 0 && islot < IR_CALL_MAX_LOCALS)
                        bits = IrCallStack[IrCallDepth - 1]->LocalVal[islot];
                    else if (Locals != NULL && islot < NLocals)
                        bits = Locals[islot];
                    got = 1;
                    if (getenv("BABYC_FP_TRACE"))
                        fprintf(stderr, "[fp] slot load islot=%d bits=%llx LightSp=%d depth=%d\n",
                            islot, (unsigned long long)bits, LightSp, IrCallDepth);
                }
                if (got)
                {
                    FV = (struct Value *)(intptr_t)bits;
                    if (getenv("BABYC_FP_TRACE"))
                        fprintf(stderr, "[fp] FV=%p base=%d\n", (void*)FV,
                            FV&&FV->Typ?(int)FV->Typ->Base:-1);
                    if (FV == NULL || FV->Typ == NULL || FV->Typ->Base != TypeFunction
                        || FV->Val == NULL)
                    {
                        if (getenv("BABYC_FP_TRACE"))
                            fprintf(stderr, "[fp] ABORT bad FV\n");
                        LinStepAbort = 1;
                        R[In->Dest] = 0;
                        (*Ip)++;
                        break;
                    }
                }
            }
            
            /* Full-width string/ptr args (CallStrBits) for host AND user. */
            for (ai = 0; ai < na; ai++)
                if (CallStrOn[ci][ai])
                    args[ai] = CallStrBits[ci][ai];
            /* Global fptr Variable → Function Value* at call site. */
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypePointer
                && FV->Typ->FromType != NULL
                && FV->Typ->FromType->Base == TypeFunction
                && FV->Val != NULL)
            {
                FV = (struct Value *)FV->Val->Pointer;
                if (FV == NULL || FV->Typ == NULL || FV->Typ->Base != TypeFunction
                    || FV->Val == NULL)
                {
                    LinStepAbort = 1;
                    R[In->Dest] = 0;
                    (*Ip)++;
                    break;
                }
            }
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                && FV->Val != NULL && FV->Val->FuncDef.Intrinsic != NULL)
            {
                /* Host Intrinsic (printf/strcpy/...): EXEC=vm path only. */
                LinHostCallName = (ci >= 0 && ci < IR_LIN_CALL_SLOTS) ? CallNameTab[ci] : NULL;
                if (LinParser != NULL && ci >= 0 && ci < IR_LIN_CALL_SLOTS && CallCharPos[ci] > 0)
                    LinParser->CharacterPos = CallCharPos[ci];
                R[In->Dest] = IrCallHostFromRegs(LinParser, FV, args, na,
                    CallArgIsPtr[ci], CallArgIsFp[ci]);
                LinHostCallName = NULL;
                /* strcpy/sprintf/memset dest: mark matching owned local array
                 * fully initialized (mirrors token-path lib-call mark). */
                if (IrCallDepth > 0 && na > 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int ai, si;
                    int scan_n = Fr->NumLocals > 0 ? Fr->NumLocals : IR_CALL_MAX_LOCALS;
                    for (ai = 0; ai < na; ai++)
                    {
                        if (!CallArgIsPtr[ci][ai] || args[ai] == 0)
                            continue;
                        for (si = 0; si < scan_n && si < IR_CALL_MAX_LOCALS; si++)
                        {
                            if (!Fr->LocalOwned[si] || Fr->LocalVal[si] != args[ai])
                                continue;
                            Fr->LocalInit[si] = 1;
                            if (Fr->LocalInitMap[si] != NULL)
                            {
                                int mln = IrFrameInitMapLen(Fr, si);
                                if (mln > 0)
                                    memset(Fr->LocalInitMap[si], 1, (size_t)mln);
                            }
                            else if (Fr->LocalArraySize[si] > 0
                                     && Fr->LocalArraySize[si] < 65536)
                            {
                                int n = Fr->LocalArraySize[si];
                                unsigned char *imap = (unsigned char *)calloc(1, (size_t)n);
                                if (imap != NULL)
                                {
                                    memset(imap, 1, (size_t)n);
                                    Fr->LocalInitMap[si] = imap;
                                }
                            }
                        }
                    }
                }
            }
            else if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                     && FV->Val != NULL && FV->Val->FuncDef.BodyIr != NULL)
            {
                IrStmt *CalleeBody = (IrStmt *)FV->Val->FuncDef.BodyIr;
                /* Resolve the cached slot before testing mono eligibility.
                 * FnCodeCompile computes MonoElig once per function; walking
                 * Body-IR here on every dynamic call made call_loop/leaf_call
                 * pay the full eligibility analysis millions of times. */
                FnCodeSlot *CS = FnCodeFindByFunc(FV);
                /* Compile on first Call if eligible (param Index, leaf loops). */
                if ((CS == NULL || CS->N <= 0) && CalleeBody != NULL && LinParser != NULL)
                    CS = FnCodeCompile(LinParser, CalleeBody, FV);
                /* Fast path: any already-compiled integer body (same or other). */
                if (CS != NULL && CS->N > 0 && CS->Elig == 1)
                {
                    int saved_ret = LinRetSeen;
                    long long saved_retval = LinRetVal;
                    int saved_abort = LinStepAbort;
                    int saved_div0 = LinDiv0;
                    long long result = 0;
                    /* Mono first: pure recursive shapes at Body-IR speed. */
                    if (CS->MonoElig == 1 && FV != NULL && na >= 1 && na <= 2)
                    {
                        R[In->Dest] = IrMonoRun(FV, args, na);
                        (*Ip)++;
                        break;
                    }
                    /* Memo-only entry (non-mono pure). */
                    if (CS->MemoElig == 1 && FV != NULL && na >= 1)
                    {
                        long long mh;
                        if (IrMemoLookup(FV, args, na, &mh))
                        {
                            R[In->Dest] = mh;
                            (*Ip)++;
                            break;
                        }
                    }
                    /* MaxLocal==0: light param window (no IrFramePush).
                     * Covers leaf helpers and same-body fib recursion
                     * (CallN>0, tables already installed when CS==FnActive). */
                    /* Array kernels need IrCallFrame (LocalVal/ParamName).
                     * DO-only kernels (IdxN==0) stay on light + CountdownLight. */
                    {
                        int has_pp = 0, pi, only_self = 1, ci2;
                        if (FV && FV->Val && FV->Val->FuncDef.ParamType)
                            for (pi = 0; pi < na && pi < FV->Val->FuncDef.NumParams; pi++)
                            {
                                struct ValueType *PT = FV->Val->FuncDef.ParamType[pi];
                                if (PT && (PT->Base == TypePointer || PT->Base == TypeArray
                                    || PT->Base == TypeStruct || PT->Base == TypeUnion))
                                    has_pp = 1; /* need IrCallFrame ParamVal for Member */
                            }
                        /* Light only for leaves or pure self-recursion.
                         * Mixed callees (qs→partition/printf) corrupt LightLocal (025). */
                        for (ci2 = 0; ci2 < CS->CallN; ci2++)
                        {
                            if (CS->CallFnTab[ci2] != NULL
                                && CS->CallFnTab[ci2] != CS->Func
                                && CS->CallFnTab[ci2] != FV)
                            {
                                only_self = 0;
                                break;
                            }
                        }
                    if (CS->MaxLocal >= 0 && CS->MaxLocal <= LIGHT_LOCALS
                        /* Light frames have their own scalar slots, but the
                         * surrounding VM frame is still consulted by a few
                         * address/member operations.  Do not use that shortcut
                         * for nested calls: a callee local with the same slot
                         * number must never alias its caller's LocalVal. */
                        && (IrCallDepth == 0 || IrFnCodeNestedLightSafe(CS))
                        && LightSp < LIGHT_MAX
                        && CS->N <= 128
                        && !CS->HasArrayKernel
                        && CS->IdxN <= 0
                        && !CS->HasKernel
                        && !has_pp
                        && only_self
                        && !IrFnCodeHasPtrSlots(CS))
                    {
                        int lsp = LightSp, gi, nloc;
                        long long *LLoc;
                        FnCodeSlot *prev = FnActive;
                        int need_install = (FnActive != CS);
                        int fast_swap = (IrExecIsVmProd() && need_install && prev != NULL
                            && prev->IdxN == 0 && CS->IdxN == 0 && IdxN == 0
                            && IdxDirty == 0);
                        for (ai = 0; ai < na && ai < IR_CALL_MAX_PARAMS; ai++)
                            LightParam[lsp][ai] = args[ai];
                        LightInitMask[lsp] = 0ULL;
                        nloc = CS->MaxLocal > 0 ? CS->MaxLocal : 0;
                        LLoc = LightLocal[lsp];
                        if (nloc > 0)
                        {
                            for (gi = 0; gi < nloc && gi < LIGHT_LOCALS; gi++)
                                LLoc[gi] = 0;
                        }
                        else
                            nloc = 1; /* LightRunBody needs nloc>=1 for LLoc[0] scratch */
                        LightSp = lsp + 1;
                        if (need_install && !fast_swap)
                            LinFramePush();
                        if (need_install || FnActive != CS)
                            FnCodeInstall(CS);
                        result = LightRunBody(CS, lsp);
                        if (LinStepAbort || LinDiv0)
                        {
                            /* Light/kernel aborted: discard partial result and
                             * re-enter callee via tree on a fresh frame. */
                            if (need_install)
                            {
                                if (fast_swap)
                                    FnCodeInstall(prev);
                                else
                                    LinFramePop();
                            }
                            else if (IdxN > 0 || IdxDirty)
                                IdxRunClearCache();
                            LightSp = lsp;
                            CS->Elig = 0;
                            LinStepAbort = saved_abort;
                            LinDiv0 = saved_div0;
                            result = IrCallUserFuncCached(LinParser, FV, args, na);
                        }
                        else
                        {
                        if (need_install)
                        {
                            if (fast_swap)
                                FnCodeInstall(prev);
                            else
                                LinFramePop();
                        }
                        else if (IdxN > 0 || IdxDirty)
                            IdxRunClearCache();
                        LightSp = lsp;
                        if (CS->MemoElig == 1 && FV != NULL && na >= 1)
                            IrMemoStore(FV, args, na, result);
                        }
                    }
                    else
                    {
/* Nested with locals: real frame (isolates LocalVal). */
                        IrCallFrame *F = IrFramePush(LinParser);
                        FnCodeSlot *prev = FnActive;
                        if (F == NULL)
                        {
                            R[In->Dest] = IrCallUserFuncCached(LinParser, FV, args, na);
                            (*Ip)++;
                            break;
                        }
                        F->NumParams = na;
                        F->NumLocals = CS->MaxLocal;
                        F->HasReturn = 0;
                        F->ReturnVal = 0;
                        for (ai = 0; ai < na && ai < IR_CALL_MAX_PARAMS; ai++)
                        {
                            F->ParamVal[ai] = args[ai];
                            F->ParamTyp[ai] = NULL;
                            F->ParamIsPtr[ai] = 0;
                            F->ParamName[ai] = NULL;
                            if (FV->Val != NULL && FV->Val->FuncDef.ParamName != NULL
                                && ai < FV->Val->FuncDef.NumParams)
                                F->ParamName[ai] = FV->Val->FuncDef.ParamName[ai];
                            if (FV->Val->FuncDef.ParamType != NULL
                                && ai < FV->Val->FuncDef.NumParams)
                            {
                                struct ValueType *PT = FV->Val->FuncDef.ParamType[ai];
                                F->ParamTyp[ai] = PT;
                                if (PT != NULL && (PT->Base == TypePointer
                                    || PT->Base == TypeArray
                                    || PT->Base == TypeStruct
                                    || PT->Base == TypeUnion))
                                    F->ParamIsPtr[ai] = 1;
                            }
                        }
                        LinFramePush();
                        FnCodeInstall(CS);
                        /* GUARD: descriptor seed only (no parallel SlotIsPtr loop). */
                        IrStorageDescSeedFrameFromFnSlot(F, CS);
                        LinRetSeen = 0; LinStepAbort = 0; LinDiv0 = 0;
                        {
                            /* Callee runs on its own IrCallFrame: the caller's
                             * light frame (LightLocal/LightParam[LightSp-1]) must
                             * not be visible to it (params/locals would alias). */
                            int saved_lsp = LightSp;
                            LightSp = 0;
                            IrExecLinLocals(CS->Code, CS->N, F->LocalVal,
                                            IR_CALL_MAX_LOCALS);
                            LightSp = saved_lsp;
                        }
                        if (LinStepAbort || LinDiv0)
                        {
                            /* Nested body aborted mid-run: do not publish a
                             * partial result and do not swallow the failure
                             * into the caller. Pop the linear frame, then
                             * re-enter the callee via the tree path which
                             * starts from a fresh IrFramePush. */
                            LinStepAbort = saved_abort;
                            LinDiv0 = saved_div0;
                            LinFramePop();
                            IrFramePop();
                            result = IrCallUserFuncCached(LinParser, FV, args, na);
                        }
                        else
                        {
                            if (LinRetSeen)
                                result = LinRetVal;
                            else if (F->HasReturn)
                                result = F->ReturnVal;
                            LinStepAbort = saved_abort;
                            LinDiv0 = saved_div0;
                            LinFramePop();
                            IrFramePop();
                        }
                    }
                    LinRetSeen = saved_ret;
                    LinRetVal = saved_retval;
                    R[In->Dest] = result;
                    } /* has_pp scope */
                }
                else
                    R[In->Dest] = IrCallUserFuncCached(LinParser, FV, args, na);
            }
            else if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction)
                R[In->Dest] = IrCallUserFuncCached(LinParser, FV, args, na);
            else
                R[In->Dest] = 0;
            (*Ip)++;
            break;
        }
        case IrLinKernel:
        {
            int ki = (int)In->Imm;
            IrStmt *KS = (ki >= 0 && ki < KernelN) ? KernelStmtTab[ki] : NULL;
            if (KS != NULL && LinParser != NULL)
            {
                if (In->Tok == IR_K_INSERT_SHIFT)
                {
                    if (!IrTryFastInsertShift(LinParser, KS))
                        LinStepAbort = 1;
                }
                else if (In->Tok == IR_K_ARRAY_ACCUM)
                {
                    if (!IrTryFastArrayAccum(LinParser, KS))
                        LinStepAbort = 1;
                }
                else if (In->Tok == IR_K_ARRAY_FILL)
                {
                    if (!IrTryFastArrayFill(LinParser, KS))
                        LinStepAbort = 1;
                }
                else if (In->Tok == IR_K_PTR_WALK)
                {
                    /* Match other kernels: failure must abort so the body is
                     * not silently skipped (NO_KERNEL / shape mismatch). */
                    if (!IrTryFastPtrWalk(LinParser, KS))
                        LinStepAbort = 1;
                }
                else if (In->Tok == IR_K_DO_COUNTDOWN)
                {
                    extern int IrTryFastDoCountdown(struct ParseState *, IrStmt *);
                    extern int IrTryFastDoCountdownLight(long long *, long long *, int, int, IrStmt *);
                    if (LightSp > 0)
                        IrTryFastDoCountdownLight(LightLocal[LightSp-1], LightParam[LightSp-1],
                            LIGHT_LOCALS, IR_CALL_MAX_PARAMS, KS);
                    else
                        IrTryFastDoCountdown(LinParser, KS);
                }
                else if (In->Tok == IR_K_DECL_ARRAY)
                {
                    IrStmt *D = KS;
                    IrCallFrame *Fr;
                    size_t nbytes;
                    void *mem;
                    int slot, es, n;
                    if (D == NULL || IrCallDepth <= 0)
                    {
                        LinStepAbort = 1;
                        break;
                    }
                    Fr = IrCallStack[IrCallDepth - 1];
                    slot = D->LocalSlot;
                    n = D->DeclArraySize;
                    es = 0;
                    if (In->A > 0)
                        es = (int)In->A;
                    if (es <= 0)
                        es = IrResolveDeclElemSize(D->DeclElemSize, D->DeclFromType);
                    if (slot < 0 || slot >= IR_CALL_MAX_LOCALS || n <= 0)
                    {
                        LinStepAbort = 1;
                        break;
                    }
                    nbytes = (size_t)n * (size_t)es;
                    /* GUARD: C8 IrAllocOwnedBlob */
                    mem = IrAllocOwnedBlob(Fr, slot, nbytes > 0 ? nbytes : 1);
                    if (mem == NULL)
                    {
                        LinStepAbort = 1;
                        break;
                    }
                    Fr->LocalIsPtr[slot] = 1; /* array decays for Index/accum */
                    /* Light window LoadLocal reads LLoc, not Fr->LocalVal (269). */
                    if (LightSp > 0 && slot < LIGHT_LOCALS)
                        LightLocal[LightSp - 1][slot] = (long long)(uintptr_t)mem;
                    if (D->DeclIsFptr)
                    {
                        Fr->LocalIsFptr[slot] = 1;
                        Fr->LocalElemIsPtr[slot] = 1;
                    }
                    else if (D->DeclElemIsPtr)
                        Fr->LocalElemIsPtr[slot] = 1;
                    if (D->DeclFromType != NULL)
                    {
                        struct ValueType *FT = D->DeclFromType;
                        FT = IrTypePeelArray(FT);
                        Fr->LocalFromType[slot] = FT;
                        Fr->LocalTyp[slot] = FT;
                    }
                    Fr->LocalArraySize[slot] = n;
                    Fr->LocalElemSize[slot] = (unsigned char)(es > 0 && es < 256 ? es : 1);
                    if (D->DeclElemIsPtr && D->DeclPointeeSize > 0)
                        Fr->LocalPointeeSize[slot] = (unsigned char)(
                            D->DeclPointeeSize < 256 ? D->DeclPointeeSize : 4);
                    else if (D->DeclElemIsPtr)
                        Fr->LocalPointeeSize[slot] = (unsigned char)sizeof(int);
                    /* Uninit: brace vals or array-ident copy. */
                    Fr->LocalInit[slot] = ((D->DeclInitVals != NULL && D->DeclInitCount > 0)
                        || D->DeclInitCount == -1) ? 1 : 0;
                    /* Uninit map: byte-map for struct/union (IrFrameIsByteMap),
                     * element-map for scalars. Mismatch caused heap-overflow in
                     * IrFrameMarkBytes under EXEC=vm (248/548). */
                    if (IrUninitChecksOn() && nbytes > 0 && nbytes < 65536)
                    {
                        int is_su = 0;
                        if (D->DeclFromType != NULL)
                        {
                            struct ValueType *FT = D->DeclFromType;
                            if (FT->Base == TypeArray && FT->FromType)
                                FT = FT->FromType;
                            if (FT->Base == TypeStruct || FT->Base == TypeUnion)
                                is_su = 1;
                        }
                        /* aos: byte map over full blob; scalar arrays: per-elem */
                        size_t map_n = is_su ? nbytes : (size_t)n;
                        unsigned char *imap = (unsigned char *)calloc(map_n, 1);
                        if (imap != NULL)
                        {
                            if (Fr->LocalInitMap[slot] != NULL)
                                free(Fr->LocalInitMap[slot]);
                            Fr->LocalInitMap[slot] = imap;
                            if (D->DeclInitVals != NULL && D->DeclInitCount > 0)
                                memset(imap, 1, map_n);
                        }
                    }
                    /* GUARD: array-ident init via shared helper (1042 STRICT). */
                    if (D->DeclInitCount == -1 && D->Expr != NULL
                        && D->Expr->Kind == IrExIdent && D->Expr->LocalSlot >= 0)
                    {
                        unsigned char *imap2 = Fr->LocalInitMap[slot];
                        if (IrCopyArrayIdentInit(LinParser, Fr, mem, n, es,
                                D->Expr, imap2))
                            Fr->LocalInit[slot] = 1;
                    }
                    if (D->DeclIdent != NULL)
                        Fr->LocalName[slot] = D->DeclIdent;
                    if (D->DeclFromType != NULL)
                    {
                        struct ValueType *FT = D->DeclFromType;
                        FT = IrTypePeelArray(FT);
                        Fr->LocalFromType[slot] = FT;
                        Fr->LocalTyp[slot] = FT;
                    }
                    if (D->DeclNDims > 0)
                    {
                        int di;
                        Fr->LocalNDims[slot] = (unsigned char)D->DeclNDims;
                        for (di = 0; di < D->DeclNDims && di < IR_MAX_ARRAY_DIMS; di++)
                            Fr->LocalDims[slot][di] = D->DeclDims[di];
                    }
                    else
                    {
                        Fr->LocalNDims[slot] = 1;
                        Fr->LocalDims[slot][0] = n;
                    }
                    /* brace-init: pack DeclInitVals into blob.
                     * Scalar int[]: es==4, one val per elem.
                     * struct S a[N]={{1,2},{3,4}}: es==sizeof(S), flat int vals. */
                    if (D->DeclInitVals != NULL && D->DeclInitCount > 0)
                    {
                        int ei;
                        /* Non-constant / non-folded initializers (x, x+y, (-1L)) live in
                         * DeclInitExprs; DeclInitVals holds 0 for them. */
                        long long *IV = D->DeclInitVals;
                        long long *IVown = NULL;
                        if (D->DeclInitExprs != NULL && LinParser != NULL)
                        {
                            IVown = (long long *)malloc(sizeof(long long) * (size_t)D->DeclInitCount);
                            if (IVown == NULL) { LinStepAbort = 1; break; }
                            for (ei = 0; ei < D->DeclInitCount; ei++)
                                IVown[ei] = (D->DeclInitExprs[ei] != NULL)
                                    ? IrEvalExpr(LinParser, D->DeclInitExprs[ei])
                                    : D->DeclInitVals[ei];
                            IV = IVown;
                        }
                        /* Pack by element size. DeclFromType may be TypeChar
                         * (not TypeArray) for char s[]="..", so FromType can
                         * be NULL — do not require Array.FromType. */
                        if (es == 1)
                        {
                            unsigned char *ca = (unsigned char *)mem;
                            for (ei = 0; ei < D->DeclInitCount && ei < n; ei++)
                                ca[ei] = (unsigned char)IV[ei];
                        }
                        else if (es == 2)
                        {
                            unsigned short *sa = (unsigned short *)mem;
                            for (ei = 0; ei < D->DeclInitCount && ei < n; ei++)
                                sa[ei] = (unsigned short)IV[ei];
                        }
                        else if (es > 0 && D->DeclInitCount > n)
                        {
                            /* Array-of-struct brace: pack per-element by member
                             * offsets (bitfields + nested). 557 a[2]={{1,2,3},{4,5,6}}. */
                            struct ValueType *ET = D->DeclFromType;
                            int packed = 0;
                            if (ET != NULL && ET->Base == TypeArray && ET->FromType)
                                ET = ET->FromType;
                            if (ET != NULL && (ET->Base == TypeStruct || ET->Base == TypeUnion)
                                && ET->Members != NULL)
                            {
                                struct Value *MVs[64];
                                int nmem = 0, bi, a, b, el, vi;
                                for (bi = 0; bi < ET->Members->Size; bi++)
                                {
                                    struct TableEntry *ent = ET->Members->HashTable[bi];
                                    for (; ent != NULL; ent = ent->Next)
                                    {
                                        struct Value *MV = ent->p.v.Val;
                                        if (MV == NULL || MV->Val == NULL) continue;
                                        if (nmem < 64) MVs[nmem++] = MV;
                                    }
                                }
                                for (a = 1; a < nmem; a++)
                                {
                                    struct Value *key = MVs[a];
                                    int kbyte = (int)key->Val->Integer;
                                    int kbit = key->BitOffset;
                                    b = a - 1;
                                    while (b >= 0
                                        && ((int)MVs[b]->Val->Integer > kbyte
                                            || ((int)MVs[b]->Val->Integer == kbyte
                                                && MVs[b]->BitOffset > kbit)))
                                    {
                                        MVs[b + 1] = MVs[b];
                                        b--;
                                    }
                                    MVs[b + 1] = key;
                                }
                                /* Sequential fill: expand nested struct/union
                                 * members so {{1,2},10} maps to in.a,in.b,id
                                 * (same contract as tree brace in ir_stmt). */
                                {
                                    int flat_i = 0;
                                    for (el = 0; el < n && flat_i < D->DeclInitCount; el++)
                                    {
                                        char *ebase = (char *)mem + (ptrdiff_t)el * es;
                                        for (vi = 0; vi < nmem && flat_i < D->DeclInitCount; vi++)
                                        {
                                            struct Value *MV = MVs[vi];
                                            char *base = ebase + MV->Val->Integer;
                                            if (MV->BitWidth > 0)
                                            {
                                                IrBitInsert(base, MV->BitOffset, MV->BitWidth,
                                                    IV[flat_i++]);
                                            }
                                            else if (MV->Typ != NULL
                                                && (MV->Typ->Base == TypeStruct
                                                    || MV->Typ->Base == TypeUnion)
                                                && MV->Typ->Members != NULL)
                                            {
                                                /* Nested struct: fill its scalar members. */
                                                struct Value *NM[64];
                                                int NO[64], nc = 0, nhi, ni, nj;
                                                struct TableEntry *NEnt;
                                                for (nhi = 0; nhi < MV->Typ->Members->Size; nhi++)
                                                    for (NEnt = MV->Typ->Members->HashTable[nhi];
                                                         NEnt != NULL; NEnt = NEnt->Next)
                                                    {
                                                        if (nc >= 64) break;
                                                        NM[nc] = NEnt->p.v.Val;
                                                        NO[nc] = NEnt->p.v.Val->Val->Integer;
                                                        nc++;
                                                    }
                                                /* Sort by DeclIndex then offset (parse order). */
                                                for (ni = 1; ni < nc; ni++)
                                                {
                                                    struct Value *mv = NM[ni];
                                                    int off = NO[ni];
                                                    for (nj = ni; nj > 0; nj--)
                                                    {
                                                        int better = 0;
                                                        if (NM[nj-1]->DeclIndex != mv->DeclIndex)
                                                            better = NM[nj-1]->DeclIndex > mv->DeclIndex;
                                                        else
                                                            better = NO[nj-1] > off;
                                                        if (!better) break;
                                                        NM[nj] = NM[nj-1];
                                                        NO[nj] = NO[nj-1];
                                                    }
                                                    NM[nj] = mv;
                                                    NO[nj] = off;
                                                }
                                                for (ni = 0; ni < nc && flat_i < D->DeclInitCount; ni++)
                                                {
                                                    long long v = IV[flat_i++];
                                                    void *na = (char *)base + NO[ni];
                                                    if (NM[ni]->BitWidth > 0)
                                                        IrBitInsert(na, NM[ni]->BitOffset,
                                                            NM[ni]->BitWidth, v);
                                                    else if (NM[ni]->Typ != NULL)
                                                    {
                                                        enum BaseType NB = NM[ni]->Typ->Base;
                                                        if (NB == TypeChar || NB == TypeUnsignedChar)
                                                            *(char *)na = (char)v;
                                                        else if (NB == TypeShort || NB == TypeUnsignedShort)
                                                            *(short *)na = (short)v;
                                                        else if (NB == TypeLongLong || NB == TypeUnsignedLongLong)
                                                            *(long long *)na = v;
                                                        else
                                                            *(int *)na = (int)v;
                                                    }
                                                    else
                                                        *(int *)na = (int)v;
                                                }
                                            }
                                            else if (MV->Typ != NULL)
                                            {
                                                enum BaseType bt = MV->Typ->Base;
                                                long long v = IV[flat_i++];
                                                if (bt == TypeChar || bt == TypeUnsignedChar)
                                                    *(unsigned char *)base = (unsigned char)v;
                                                else if (bt == TypeShort || bt == TypeUnsignedShort)
                                                    *(unsigned short *)base = (unsigned short)v;
                                                else if (bt == TypeLongLong || bt == TypeUnsignedLongLong)
                                                    *(long long *)base = v;
                                                else
                                                    *(int *)base = (int)v;
                                            }
                                            else
                                                *(int *)base = (int)IV[flat_i++];
                                        }
                                    }
                                }
                                packed = 1;
                            }
                            if (!packed)
                            {
                                int *ia = (int *)mem;
                                int maxi = (int)(nbytes / sizeof(int));
                                for (ei = 0; ei < D->DeclInitCount && ei < maxi; ei++)
                                    ia[ei] = (int)IV[ei];
                            }
                        }
                        else if (es == (int)sizeof(int))
                        {
                            int *ia = (int *)mem;
                            for (ei = 0; ei < D->DeclInitCount && ei < n; ei++)
                                ia[ei] = (int)IV[ei];
                        }
                        else if (es == (int)sizeof(long long))
                        {
                            long long *la = (long long *)mem;
                            for (ei = 0; ei < D->DeclInitCount && ei < n; ei++)
                                la[ei] = IV[ei];
                        }
                        else if (es > 0 && (es % (int)sizeof(int)) == 0)
                        {
                            int *ia = (int *)mem;
                            int maxi = (int)(nbytes / sizeof(int));
                            for (ei = 0; ei < D->DeclInitCount && ei < maxi; ei++)
                                ia[ei] = (int)IV[ei];
                        }
                        free(IVown);
                    }
                }
                else if (In->Tok == IR_K_DECL_STATIC_STU)
                {
                    IrStmt *D = KS;
                    IrCallFrame *Fr;
                    IrStaticLocalEntry *E;
                    int slot;
                    size_t nbytes;
                    void *mem = NULL;
                    if (D == NULL || IrCallDepth <= 0)
                    { LinStepAbort = 1; break; }
                    Fr = IrCallStack[IrCallDepth - 1];
                    slot = D->LocalSlot;
                    if (slot < 0 || slot >= IR_CALL_MAX_LOCALS || D->DeclIdent == NULL
                        || D->DeclFromType == NULL)
                    { LinStepAbort = 1; break; }
                    nbytes = D->DeclFromType->Sizeof > 0
                        ? (size_t)D->DeclFromType->Sizeof : 0;
                    if (nbytes == 0 || nbytes > 4096)
                    { LinStepAbort = 1; break; }
                    E = IrStaticLocalGet(D->DeclIdent,
                        D->SourceLine > 0 ? D->SourceLine : 0, 1);
                    if (E == NULL)
                    { LinStepAbort = 1; break; }
                    if (E->Inited && E->Val != 0)
                        mem = (void *)(intptr_t)E->Val;
                    else
                    {
                        mem = calloc(1, nbytes);
                        if (mem == NULL)
                        { LinStepAbort = 1; break; }
                        E->Val = (long long)(intptr_t)mem;
                        E->Inited = 1;
                    }
                    Fr->LocalVal[slot] = (long long)(intptr_t)mem;
                    Fr->LocalInit[slot] = 1;
                    Fr->LocalIsStatic[slot] = 1;
                    Fr->LocalStaticLine[slot] = D->SourceLine > 0 ? D->SourceLine : 0;
                    Fr->LocalOwned[slot] = 0;
                    Fr->LocalIsPtr[slot] = 0;
                    Fr->LocalArraySize[slot] = 0;
                    Fr->LocalElemSize[slot] = 0;
                    Fr->LocalName[slot] = D->DeclIdent;
                    Fr->LocalFromType[slot] = D->DeclFromType;
                    Fr->LocalTyp[slot] = D->DeclFromType;
                    if (IrUninitChecksOn() && nbytes > 0 && nbytes < 65536)
                    {
                        unsigned char *imap = (unsigned char *)calloc(nbytes, 1);
                        if (imap != NULL)
                        {
                            memset(imap, 1, nbytes); /* BSS fully init */
                            if (Fr->LocalInitMap[slot] != NULL)
                                free(Fr->LocalInitMap[slot]);
                            Fr->LocalInitMap[slot] = imap;
                        }
                    }
                    if (Fr->NumLocals <= slot)
                        Fr->NumLocals = slot + 1;
                }
                else if (In->Tok == IR_K_DECL_STATIC_ARR)
                {
                    IrStmt *D = KS;
                    IrCallFrame *Fr;
                    IrStaticLocalEntry *E;
                    int slot, es, n;
                    size_t nbytes;
                    void *mem = NULL;
                    if (D == NULL || IrCallDepth <= 0)
                    { LinStepAbort = 1; break; }
                    Fr = IrCallStack[IrCallDepth - 1];
                    slot = D->LocalSlot;
                    if (slot < 0 || slot >= IR_CALL_MAX_LOCALS || D->DeclIdent == NULL)
                    { LinStepAbort = 1; break; }
                    es = (int)In->A;
                    if (es != 1 && es != 2 && es != 4 && es != 8)
                        es = D->DeclElemSize > 0 ? D->DeclElemSize : (int)sizeof(int);
                    n = D->DeclArraySize > 0 ? D->DeclArraySize : 0;
                    if (n <= 0 || n > 1024)
                    { LinStepAbort = 1; break; }
                    nbytes = (size_t)n * (size_t)es;
                    E = IrStaticLocalGet(D->DeclIdent,
                        D->SourceLine > 0 ? D->SourceLine : 0, 1);
                    if (E == NULL)
                    { LinStepAbort = 1; break; }
                    if (E->Inited && E->Val != 0)
                        mem = (void *)(intptr_t)E->Val;
                    else
                    {
                        mem = calloc(1, nbytes); /* BSS zero, then optional brace-init */
                        if (mem == NULL)
                        { LinStepAbort = 1; break; }
                        /* Tree (ir_stmt) applies DeclInitVals after alloc. Without
                         * this, static T buf[N] = {...} stays zero under linear VM
                         * so *ret_ptr() / ret_ptr()[i] read 0 (770). */
                        if (D->DeclInitVals != NULL
                            && (D->DeclFromType == NULL
                                || (D->DeclFromType->Base != TypeFP
                                    && D->DeclFromType->Base != TypeStruct
                                    && D->DeclFromType->Base != TypeUnion)))
                        {
                            int ei;
                            char *base = (char *)mem;
                            for (ei = 0; ei < D->DeclInitCount && ei < n; ei++)
                            {
                                long long cv = D->DeclInitVals[ei];
                                if (D->DeclInitExprs != NULL
                                    && D->DeclInitExprs[ei] != NULL
                                    && LinParser != NULL)
                                    cv = IrEvalExpr(LinParser, D->DeclInitExprs[ei]);
                                IrWriteByElemSize(base + (size_t)ei * (size_t)es, es, cv);
                            }
                        }
                        E->Val = (long long)(intptr_t)mem;
                        E->Inited = 1;
                    }
                    Fr->LocalVal[slot] = (long long)(intptr_t)mem;
                    Fr->LocalInit[slot] = 1;
                    Fr->LocalIsStatic[slot] = 1;
                    Fr->LocalStaticLine[slot] = D->SourceLine > 0 ? D->SourceLine : 0;
                    Fr->LocalOwned[slot] = 0; /* process-lifetime blob */
                    Fr->LocalIsPtr[slot] = 1; /* array decays to ptr */
                    Fr->LocalElemSize[slot] = (unsigned char)es;
                    Fr->LocalArraySize[slot] = n;
                    Fr->LocalName[slot] = D->DeclIdent;
                    if (D->DeclFromType != NULL)
                    {
                        Fr->LocalFromType[slot] = D->DeclFromType;
                        Fr->LocalTyp[slot] = D->DeclFromType;
                    }
                    if (D->DeclNDims > 0)
                    {
                        int di;
                        Fr->LocalNDims[slot] = (unsigned char)D->DeclNDims;
                        for (di = 0; di < D->DeclNDims && di < IR_MAX_ARRAY_DIMS; di++)
                            Fr->LocalDims[slot][di] = D->DeclDims[di];
                    }
                    else
                    {
                        Fr->LocalNDims[slot] = 1;
                        Fr->LocalDims[slot][0] = n;
                    }
                    if (IrUninitChecksOn() && nbytes > 0 && nbytes < 65536)
                    {
                        unsigned char *imap = (unsigned char *)calloc(n, 1);
                        if (imap != NULL)
                        {
                            memset(imap, 1, (size_t)n); /* BSS fully init */
                            if (Fr->LocalInitMap[slot] != NULL)
                                free(Fr->LocalInitMap[slot]);
                            Fr->LocalInitMap[slot] = imap;
                        }
                    }
                    if (Fr->NumLocals <= slot)
                        Fr->NumLocals = slot + 1;
                }
                else if (In->Tok == IR_K_DECL_STATIC)
                {
                    IrStmt *D = KS;
                    IrCallFrame *Fr;
                    IrStaticLocalEntry *E;
                    int slot;
                    long long initv = 0;
                    if (D == NULL || IrCallDepth <= 0)
                    { LinStepAbort = 1; break; }
                    Fr = IrCallStack[IrCallDepth - 1];
                    slot = D->LocalSlot;
                    if (slot < 0 || slot >= IR_CALL_MAX_LOCALS || D->DeclIdent == NULL)
                    { LinStepAbort = 1; break; }
                    E = IrStaticLocalGet(D->DeclIdent,
                        D->SourceLine > 0 ? D->SourceLine : 0, 1);
                    if (E == NULL)
                    { LinStepAbort = 1; break; }
                    if (E->Inited)
                        initv = E->Val;
                    else
                    {
                        if (D->DeclInitVals != NULL && D->DeclInitCount > 0)
                            initv = D->DeclInitVals[0];
                        else if (D->Expr != NULL && D->Expr->Kind == IrExConst)
                            initv = D->Expr->ConstVal;
                        else
                            initv = 0;
                        E->Val = initv;
                        E->Inited = 1;
                    }
                    Fr->LocalVal[slot] = initv;
                    Fr->LocalInit[slot] = 1;
                    Fr->LocalIsStatic[slot] = 1;
                    Fr->LocalStaticLine[slot] = D->SourceLine > 0 ? D->SourceLine : 0;
                    Fr->LocalIsPtr[slot] = 0;
                    Fr->LocalOwned[slot] = 0;
                    Fr->LocalArraySize[slot] = 0;
                    Fr->LocalName[slot] = D->DeclIdent;
                    if (D->DeclFromType != NULL)
                    {
                        Fr->LocalFromType[slot] = D->DeclFromType;
                        Fr->LocalTyp[slot] = D->DeclFromType;
                    }
                    if (Fr->NumLocals <= slot)
                        Fr->NumLocals = slot + 1;
                    /* StoreLocal sync: keep Locals[] if used */
                }
                else if (In->Tok == IR_K_DECL_STRUCT)
                {
                    IrStmt *D = KS;
                    IrCallFrame *Fr;
                    size_t nbytes;
                    void *mem;
                    int slot;
                    if (D == NULL || IrCallDepth <= 0)
                    { LinStepAbort = 1; break; }
                    Fr = IrCallStack[IrCallDepth - 1];
                    slot = D->LocalSlot;
                    if (slot < 0 || slot >= IR_CALL_MAX_LOCALS || D->DeclFromType == NULL)
                    { LinStepAbort = 1; break; }
                    nbytes = D->DeclFromType->Sizeof > 0
                        ? (size_t)D->DeclFromType->Sizeof : 1;
                    /* GUARD: C8 IrAllocOwnedBlob */
                    mem = IrAllocOwnedBlob(Fr, slot, nbytes);
                    if (mem == NULL)
                    { LinStepAbort = 1; break; }
                    Fr->LocalIsPtr[slot] = 0;
                    /* Locals[] + Light window: one address contract with LoadLocal. */
                    if (Locals != NULL && slot < NLocals)
                        Locals[slot] = (long long)(uintptr_t)mem;
                    if (LightSp > 0 && slot < LIGHT_LOCALS)
                    {
                        LightLocal[LightSp - 1][slot] = (long long)(uintptr_t)mem;
                        /* Blob exists after DECL: address-of is defined even if
                         * member values are still uninit (InitMap zeros). */
                        if (slot < 64)
                            LightInitMask[LightSp - 1] |= (1ULL << slot);
                    }
                    Fr->LocalArraySize[slot] = 0;
                    Fr->LocalNDims[slot] = 0;
                    Fr->LocalInit[slot] = 0;
                    Fr->LocalFromType[slot] = D->DeclFromType;
                    Fr->LocalTyp[slot] = D->DeclFromType;
                    /* Ensure FreeOwned walks this slot on pop. */
                    if (Fr->NumLocals <= slot)
                        Fr->NumLocals = slot + 1;
                    Fr->CompactClear = 0; /* cold meta written */
                    if (D->DeclIdent != NULL)
                        Fr->LocalName[slot] = D->DeclIdent;
                    if (D->DeclIsConst)
                        Fr->LocalIsConst[slot] = 1;
                    if (D->DeclInitVals != NULL && D->DeclInitCount > 0
                        && nbytes > 0)
                    {
                        /* GUARD: brace pack only via IrPackStructBraceInit. */
                        struct ValueType *ST = D->DeclFromType;
                        int used_members = 0;
                        if (ST != NULL && ST->Members != NULL
                            && (ST->Base == TypeStruct || ST->Base == TypeUnion))
                        {
                            int esz = D->DeclElemSize > 0 ? D->DeclElemSize
                                : (ST->Sizeof > 0 ? ST->Sizeof : 0);
                            int narr = D->DeclArraySize > 0 ? D->DeclArraySize : 1;
                            used_members = IrPackStructBraceInit(LinParser, mem, ST,
                                D->DeclInitVals, D->DeclInitNames,
                                NULL, D->DeclInitCount, narr, esz);
                        }
                        if (!used_members && nbytes >= sizeof(int))
                        {
                            int ei, maxi = (int)(nbytes / sizeof(int));
                            int *ia = (int *)mem;
                            for (ei = 0; ei < D->DeclInitCount && ei < maxi; ei++)
                                ia[ei] = (int)D->DeclInitVals[ei];
                        }
                        Fr->LocalInit[slot] = 1;
                    }
                    if (IrUninitChecksOn() && nbytes > 0 && nbytes < 65536)
                    {
                        unsigned char *imap = (unsigned char *)calloc(nbytes, 1);
                        if (imap != NULL)
                        {
                            if (Fr->LocalInitMap[slot] != NULL)
                                free(Fr->LocalInitMap[slot]);
                            Fr->LocalInitMap[slot] = imap;
                            if (D->DeclInitVals != NULL && D->DeclInitCount > 0)
                                memset(imap, 1, nbytes);
                        }
                    }
                }
                else if (In->Tok == IR_K_SCAN_ACCUM)
                {
                    IrStmt *Sf = KS;
                    IrStmt *Bd, *Th;
                    IrExpr *Cn;
                    int islot, cslot = -1, sslot = -1, ges = 1, gn = 0, k;
                    long long iv, bound, count, sumv;
                    unsigned char *raw = NULL;
                    struct Value *Av = NULL;
                    IrCallFrame *Fr;
                    if (Sf == NULL || IrCallDepth <= 0 || LinParser == NULL)
                    { LinStepAbort = 1; break; }
                    Fr = IrCallStack[IrCallDepth - 1];
                    Cn = Sf->Expr;
                    if (!Cn || !Cn->Left || !Cn->Right) { LinStepAbort = 1; break; }
                    islot = Cn->Left->LocalSlot;
                    bound = Cn->Right->ConstVal;
                    if (Cn->Op == TokenLessThan) bound -= 1;
                    if (islot < 0 || islot >= IR_CALL_MAX_LOCALS) { LinStepAbort = 1; break; }
                    Bd = Sf->BodyStmt;
                    if (Bd && Bd->Kind == IrStBlock && Bd->StmtCount == 1 && Bd->Stmts)
                        Bd = Bd->Stmts[0];
                    if (!Bd || Bd->Kind != IrStIf || !Bd->Expr || !Bd->ThenStmt)
                    { LinStepAbort = 1; break; }
                    Th = Bd->ThenStmt;
                    if (Th->Kind != IrStBlock || Th->StmtCount != 2 || !Th->Stmts)
                    { LinStepAbort = 1; break; }
                    for (k = 0; k < 2; k++)
                    {
                        IrStmt *St = Th->Stmts[k];
                        if (!St || !St->Expr || !St->Expr->Left) continue;
                        if (St->Expr->Kind == IrExUnary) cslot = St->Expr->Left->LocalSlot;
                        else sslot = St->Expr->Left->LocalSlot;
                    }
                    if (cslot < 0 || sslot < 0 || cslot >= IR_CALL_MAX_LOCALS
                        || sslot >= IR_CALL_MAX_LOCALS)
                    { LinStepAbort = 1; break; }
                    {
                        IrExpr *Base = Bd->Expr->Left ? Bd->Expr->Left->Left : NULL;
                        if (!Base) { LinStepAbort = 1; break; }
                        Av = (Base->CachedVal && !Base->CachedVal->OutOfScope)
                            ? Base->CachedVal : IrGlobalVal(LinParser, Base);
                    }
                    if (Av && Av->Typ && Av->Typ->Base == TypeArray && Av->Val)
                    {
                        raw = (unsigned char *)Av->Val;
                        if (Av->Typ->FromType && Av->Typ->FromType->Sizeof > 0)
                            ges = (int)Av->Typ->FromType->Sizeof;
                        gn = Av->Typ->ArraySize;
                    }
                    if (!raw || ges != 1) { LinStepAbort = 1; break; }
                    iv = Fr->LocalVal[islot];
                    count = Fr->LocalVal[cslot];
                    sumv = Fr->LocalVal[sslot];
                    {
                        long long lim = bound;
                        if (gn > 0 && lim >= (long long)gn) lim = (long long)gn - 1;
                        if (iv < 0) iv = 0;
                        for (; iv <= lim; iv++)
                            if (!raw[iv]) { count++; sumv += iv; }
                    }
                    Fr->LocalVal[islot] = iv;
                    Fr->LocalVal[cslot] = count;
                    Fr->LocalVal[sslot] = sumv;
                    Fr->LocalInit[islot] = 1;
                    Fr->LocalInit[cslot] = 1;
                    Fr->LocalInit[sslot] = 1;
                }
                else if (In->Tok == IR_K_STRIDE_FILL)
                {
                    IrStmt *Sf = KS;
                    IrStmt *Bd, *Ic;
                    IrExpr *Cn, *As, *Idx, *Base, *Rhs, *StrideE, *BoundE;
                    int jslot, stslot = -1, bslot = -1, ges = 1, gn = 0;
                    long long jv, stride, bound, fill;
                    char *raw = NULL;
                    struct Value *Av = NULL;
                    if (Sf == NULL || IrCallDepth <= 0 || LinParser == NULL)
                    { LinStepAbort = 1; break; }
                    Bd = Sf->BodyStmt;
                    if (Bd && Bd->Kind == IrStBlock && Bd->StmtCount == 1)
                        Bd = Bd->Stmts[0];
                    Ic = Sf->IncStmt;
                    Cn = Sf->Expr;
                    As = Bd->Expr;
                    Idx = As->Left;
                    Rhs = As->Right;
                    Base = Idx->Left;
                    jslot = Idx->Right->LocalSlot;
                    fill = Rhs->ConstVal;
                    StrideE = Ic->Expr->Right->Right;
                    if (StrideE->Kind == IrExConst)
                        stride = StrideE->ConstVal;
                    else
                    {
                        stslot = StrideE->LocalSlot;
                        stride = IrCallStack[IrCallDepth - 1]->LocalVal[stslot];
                    }
                    BoundE = Cn->Right;
                    if (BoundE->Kind == IrExConst)
                        bound = BoundE->ConstVal;
                    else
                    {
                        bslot = BoundE->LocalSlot;
                        bound = IrCallStack[IrCallDepth - 1]->LocalVal[bslot];
                    }
                    if (Cn->Op == TokenLessThan) bound -= 1;
                    Av = (Base->CachedVal && !Base->CachedVal->OutOfScope)
                        ? Base->CachedVal : IrGlobalVal(LinParser, Base);
                    if (Av && Av->Typ && Av->Typ->Base == TypeArray && Av->Val)
                    {
                        raw = (char *)Av->Val;
                        if (Av->Typ->FromType && Av->Typ->FromType->Sizeof > 0)
                            ges = (int)Av->Typ->FromType->Sizeof;
                        gn = Av->Typ->ArraySize;
                    }
                    if (!raw || stride <= 0 || (ges != 1 && ges != 2 && ges != 4))
                    { LinStepAbort = 1; break; }
                    jv = IrCallStack[IrCallDepth - 1]->LocalVal[jslot];
                    if (ges == 1)
                    {
                        unsigned char *b = (unsigned char *)raw;
                        unsigned char fv = (unsigned char)fill;
                        if (gn > 0)
                        {
                            for (; jv <= bound && jv < (long long)gn; jv += stride)
                                b[jv] = fv;
                        }
                        else
                        {
                            for (; jv <= bound; jv += stride)
                                b[jv] = fv;
                        }
                    }
                    else if (ges == 4)
                    {
                        int *b = (int *)raw;
                        int fv = (int)fill;
                        if (gn > 0)
                        {
                            for (; jv <= bound && jv < (long long)gn; jv += stride)
                                b[jv] = fv;
                        }
                        else
                        {
                            for (; jv <= bound; jv += stride)
                                b[jv] = fv;
                        }
                    }
                    else
                    {
                        short *b = (short *)raw;
                        short fv = (short)fill;
                        if (gn > 0)
                        {
                            for (; jv <= bound && jv < (long long)gn; jv += stride)
                                b[jv] = fv;
                        }
                        else
                        {
                            for (; jv <= bound; jv += stride)
                                b[jv] = fv;
                        }
                    }
                    IrCallStack[IrCallDepth - 1]->LocalVal[jslot] = jv;
                    IrCallStack[IrCallDepth - 1]->LocalInit[jslot] = 1;
                }
                else if (In->Tok == IR_K_KERNEL_FOR)
                {
                    int ok = 0;
                    if (LightSp > 0)
                        ok = IrTryKernelForLight(KS, LightLocal[LightSp - 1],
                            LightParam[LightSp - 1], LIGHT_LOCALS, IR_CALL_MAX_PARAMS,
                            (int)In->A);
                    else if (LinParser)
                        ok = IrTryKernelFor(LinParser, KS);
                    if (!ok)
                        LinStepAbort = 1;
                }
            }
            (*Ip)++;
            break;
        }
        case IrLinUnary:
            if (In->Tok == TokenUnaryNot)
                R[In->Dest] = !R[In->A];
            else if (In->Tok == TokenUnaryExor)
                R[In->Dest] = ~R[In->A];
            else if (In->Tok == TokenCast && In->Imm == 1)
            {
                /* FP→integer truncate */
                union { long long l; double d; } u;
                u.l = R[In->A];
                R[In->Dest] = (long long)u.d;
            }
            else if (In->Tok == TokenCast && In->Imm == 2)
            {
                union { long long l; double d; } u;
                u.d = (double)R[In->A];
                R[In->Dest] = u.l;
            }
            else if (In->Tok == TokenCast && In->Imm >= 0x100 && In->Imm < 0x100 + Type_Type)
            {
                enum BaseType cb = (enum BaseType)(In->Imm - 0x100);
                R[In->Dest] = IrNarrowToBaseType(cb, R[In->A]);
                if (In->Dest < IR_LIN_REGS)
                    LinRegU[In->Dest] = (cb == TypeUnsignedChar || cb == TypeUnsignedShort
                        || cb == TypeUnsignedInt || cb == TypeUnsignedLong || cb == TypeUnsignedLongLong);
            }
            else
                R[In->Dest] = -R[In->A];
            (*Ip)++;
            break;
        case IrLinBin:
        {
            int u = (In->Imm & IR_LIN_F_UNSIGNED) != 0;
            int wide = (In->Imm & IR_LIN_F_WIDE) != 0;
            long long La = R[In->A], Lb = R[In->B];
            if (In->Imm & IR_LIN_F_FP)
            {
                union { long long l; double d; } a, b, r;
                a.l = La; b.l = Lb;
                if (In->Tok == TokenPlus) r.d = a.d + b.d;
                else if (In->Tok == TokenMinus) r.d = a.d - b.d;
                else if (In->Tok == TokenAsterisk) r.d = a.d * b.d;
                else if (In->Tok == TokenSlash)
                    r.d = (b.d != 0.0) ? (a.d / b.d) : 0.0;
                else r.l = 0;
                R[In->Dest] = r.l;
                if (In->Dest < IR_LIN_REGS) LinRegU[In->Dest] = 0;
                (*Ip)++;
                break;
            }
            /* RegFlags fallback: if Imm lacks UNSIGNED but operand reg was
             * loaded from unsigned slot/member, treat as unsigned. */
            if (!u && !IsRel((enum LexToken)In->Tok)
                && In->A < IR_LIN_REGS && LinRegU[In->A])
                u = 1;
            /* Div/mod zero: ProgramFail immediately (correct caret via ops).
             * Do not soft-set LinDiv0 — that forced tree fallback and under
             * vm_strict became "tree fallback (body)" instead of the diag. */
            if ((In->Tok == TokenSlash || In->Tok == TokenModulus) && Lb == 0
                && LinParser != NULL)
            {
                R[In->Dest] = IrApplyBinOpRaw(LinParser, (enum LexToken)In->Tok,
                                             La, Lb, u, wide);
                /* ProgramFail does not return */
                LinDiv0 = 1;
                return;
            }
            if ((In->Tok == TokenSlash || In->Tok == TokenModulus) && Lb == 0)
                LinDiv0 = 1;
            R[In->Dest] = IrApplyBinOpRaw(LinParser, (enum LexToken)In->Tok,
                                         La, Lb, u, wide);
            if (In->Dest < IR_LIN_REGS)
                LinRegU[In->Dest] = u ? 1 : 0;
            (*Ip)++;
            break;
        }
        case IrLinJmp:
            *Ip = (int)In->Imm;
            break;
        case IrLinBrZ:
            if (R[In->A] == 0)
                *Ip = (int)In->Imm;
            else
                (*Ip)++;
            break;
        case IrLinAddI:
            R[In->Dest] = R[In->A] + In->Imm;
            (*Ip)++;
            break;
        case IrLinAddLocal:
        {
            int s = (int)In->A;
            long long nv;
            if (LightSp > 0 && s >= 0 && s < LIGHT_LOCALS)
            {
                LightLocal[LightSp - 1][s] += In->Imm;
                if (s < 64)
                    LightInitMask[LightSp - 1] |= (1ULL << s);
                nv = LightLocal[LightSp - 1][s];
                if (s < NLocals)
                    Locals[s] = nv;
            }
            else if (s >= 0 && s < NLocals)
            {
                if (IrCallDepth > 0 && s < IR_CALL_MAX_LOCALS)
                {
                    IrCallFrame *Fr0 = IrCallStack[IrCallDepth - 1];
                    if (Fr0->LocalOwned[s] && Fr0->LocalArraySize[s] == 0
                        && Fr0->LocalTyp[s] != NULL
                        && (Fr0->LocalTyp[s]->Base == TypeStruct
                            || Fr0->LocalTyp[s]->Base == TypeUnion))
                    {
                        /* Do not ++/-- an owned struct blob pointer. */
                        nv = Locals[s];
                        R[In->Dest] = nv;
                        (*Ip)++;
                        break;
                    }
                }
                Locals[s] += In->Imm;
                nv = Locals[s];
                if ((int)In->Tok == (int)TypeChar || (int)In->Tok == (int)TypeUnsignedChar
                    || (int)In->Tok == (int)TypeShort || (int)In->Tok == (int)TypeUnsignedShort)
                    nv = IrNarrowToBaseType((enum BaseType)In->Tok, nv);
                if (IrCallDepth > 0 && s < IR_CALL_MAX_LOCALS)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    /* uchar/ushort ++ wrap (723 narrow_incr) */
                    if (!Fr->LocalIsPtr[s])
                    {
                        struct ValueType *LT = Fr->LocalFromType[s]
                            ? Fr->LocalFromType[s] : Fr->LocalTyp[s];
                        if (LT != NULL)
                        {
                            if (LT->Base == TypeUnsignedChar)
                                nv = (long long)(unsigned char)nv;
                            else if (LT->Base == TypeChar)
                                nv = (long long)(signed char)nv;
                            else if (LT->Base == TypeUnsignedShort)
                                nv = (long long)(unsigned short)nv;
                            else if (LT->Base == TypeShort)
                                nv = (long long)(short)nv;
                            else if (LT->Base == TypeUnsignedInt)
                                nv = (long long)(unsigned int)nv;
                            else if (LT->Base == TypeInt)
                                nv = (long long)(int)nv;
                            Locals[s] = nv;
                        }
                    }
                    Fr->LocalVal[s] = nv;
                    Fr->LocalInit[s] = 1;
                    if (Fr->LocalIsPtr[s] && Fr->LocalArraySize[s] > 0)
                    {
                        int es = Fr->LocalElemSize[s] > 0
                            ? (int)Fr->LocalElemSize[s] : 4;
                        if (es > 0 && (In->Imm == (long long)es
                                       || In->Imm == -(long long)es))
                        {
                            if (In->Imm > 0)
                            {
                                int n = Fr->LocalArraySize[s] - 1;
                                Fr->LocalArraySize[s] = n > 0 ? n : 0;
                            }
                            else
                                Fr->LocalArraySize[s] += 1;
                        }
                    }
                }
            }
            else
                nv = 0;
            R[In->Dest] = nv;
            (*Ip)++;
            break;
        }
        case IrLinIncG:
        {
            int g = (int)In->Imm;
            struct Value *V = (g >= 0 && g < GlobN && GlobVal[g] != NULL)
                ? GlobVal[g]
                : ((LinParser && g >= 0 && g < GlobN)
                    ? IrGlobalVal(LinParser, GlobTab[g]) : NULL);
            if (V != NULL && V->Val != NULL)
            {
                enum BaseType b = (V->Typ != NULL) ? V->Typ->Base : TypeInt;
                if (g >= 0 && g < 32 && GlobVal[g] == NULL) GlobVal[g] = V;
                if (b == TypeChar || b == TypeUnsignedChar
                    || b == TypeShort || b == TypeUnsignedShort) {
                    long long oldv;
                    switch (b) {
                    case TypeChar: oldv = V->Val->Character; break;
                    case TypeUnsignedChar: oldv = V->Val->UnsignedCharacter; break;
                    case TypeShort: oldv = V->Val->ShortInteger; break;
                    default: oldv = V->Val->UnsignedShortInteger; break;
                    }
                    oldv = IrNarrowToBaseType(b, oldv + 1);
                    switch (b) {
                    case TypeChar: V->Val->Character = (char)oldv; break;
                    case TypeUnsignedChar: V->Val->UnsignedCharacter = (unsigned char)oldv; break;
                    case TypeShort: V->Val->ShortInteger = (short)oldv; break;
                    default: V->Val->UnsignedShortInteger = (unsigned short)oldv; break;
                    }
                } else if (b == TypeLongLong || b == TypeUnsignedLongLong)
                    V->Val->LongLongInteger += 1;
                else if (b == TypeLong || b == TypeUnsignedLong)
                    V->Val->LongInteger += 1;
                else
                    V->Val->Integer += 1;
                V->IsInitialized = TRUE;                V->IsInitialized = TRUE;
            }
            (*Ip)++;
            break;
        }
        default:
            (*Ip)++;
            break;
    }
}


int LinExecIndexCopy(const IrLinInst *In, long long *R,
                            long long *Locals, int NLocals, int *Ip)
{
    int old_ip = *Ip, tmp_ip = old_ip;
    int sm = (int)(In->Imm & 31LL), dm = (int)((In->Imm >> 5) & 31LL);
    IrLinInst Ld, St;
    memset(&Ld, 0, sizeof(Ld));
    Ld.Op = IrLinLoadIndex; Ld.Dest = In->Dest; Ld.A = In->A; Ld.Imm = sm;
    if (!LinTryParamIndexedAccess(&Ld, R, &tmp_ip))
        Step(&Ld, R, Locals, NLocals, &tmp_ip);
    memset(&St, 0, sizeof(St));
    St.Op = IrLinStoreIndex; St.A = In->Dest; St.B = In->B; St.Imm = dm;
    if (!LinTryParamIndexedAccess(&St, R, &tmp_ip))
        Step(&St, R, Locals, NLocals, &tmp_ip);
    *Ip = old_ip + 1;
    return 1;
}


/* P0-perf: the light path was disabled for ALL nested calls (IrCallDepth > 0) to stop a callee local
 * aliasing its caller's slot (216). That forced every nested call through a full IrCallFrame push.
 * A callee that touches no local slot (params only, native light ops only) cannot alias anything,
 * so it may still use the light path. Cached per slot (0 unknown). */
static int IrFnCodeNestedLightSafe(FnCodeSlot *CS)
{
    int i;
    if (CS->NestedLightSafe != 0)
        return CS->NestedLightSafe > 0;
    CS->NestedLightSafe = 1;
    for (i = 0; i < CS->N && CS->NestedLightSafe > 0; i++)
    {
        const IrLinInst *In = &CS->Code[i];
        switch (In->Op)
        {
        case IrLinImm:
            if (In->Tok == 4) CS->NestedLightSafe = -1; /* address of a local */
            break;
        case IrLinBin: case IrLinUnary: case IrLinJmp: case IrLinBrZ: case IrLinRet:
        case IrLinLoadGlobal: case IrLinStoreGlobal: case IrLinLine: case IrLinCall:
        case IrLinIncG: case IrLinAddI:
            break;
        case IrLinLoadLocal: case IrLinStoreLocal:
            if (In->Imm >= 0) CS->NestedLightSafe = -1; /* real local slot */
            break;
        default:
            CS->NestedLightSafe = -1;
            break;
        }
    }
    return CS->NestedLightSafe > 0;
}

int IrExecLinLocals(const IrLinInst *Code, int N, long long *Locals, int NLocals)
{
    long long R[IR_LIN_REGS];
    int i, ip, steps;
    int ok;
    LinDiv0 = 0;
    LinStepAbort = 0;
    LinRetSeen = 0;
    for (i = 0; i < IR_LIN_REGS; i++)
    {
        R[i] = 0;
        LinRegU[i] = 0;
    }
    /* LocalInit is owned solely by runtime StoreLocal / kernel DECL on this
     * frame. Compile must not stamp the live frame (IrLiveMarkInit no-ops
     * under LinCompileActive); frame push already zeros LocalInit. The old
     * entry-wide clear was a band-aid for compile poison and cost O(n) on
     * every body entry — removed under option A. */
    LinExecDepth++; BabycLinExecDepth = LinExecDepth;
    /* Tight dispatch: inline hot opcodes; Step only for rare ones.
     * Stop immediately on LinStepAbort/LinDiv0 so a failed kernel cannot
     * continue into later host I/O (printf) and then tree-fallback double-print. */
    for (ip = 0, steps = 0; ip >= 0 && ip < N && steps < 500000000
         && !LinStepAbort && !LinDiv0; steps++)
    {
        const IrLinInst *In = &Code[ip];
        switch (In->Op)
        {
            case IrLinImm:
                if (In->Tok == 4 && In->Imm >= 0 && In->Imm < IR_CALL_MAX_LOCALS)
                {
                    if (IrCallDepth > 0
                        && IrCallStack[IrCallDepth - 1]->LocalOwned[In->Imm]
                        && IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm] != 0)
                        R[In->Dest] = IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm];
                    else if (Locals != NULL && In->Imm < NLocals)
                        R[In->Dest] = (long long)(uintptr_t)&Locals[In->Imm];
                    else if (IrCallDepth > 0)
                        R[In->Dest] = (long long)(uintptr_t)&IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm];
                    else
                        R[In->Dest] = 0;
                }
                else if ((In->Tok == 2 || In->Tok == 3) && In->Imm >= 0
                    && In->Imm < IR_LIN_STMT_MAX && LinWideOn[In->Imm])
                    R[In->Dest] = LinWideImm[In->Imm];
                else
                    R[In->Dest] = In->Imm;
                ip++;
                break;
            case IrLinLoadLocal:
                if (IrUninitChecksOn() && LinParser != NULL
                    && In->Imm >= 0 && In->Imm < IR_CALL_MAX_LOCALS
                    && IrCallDepth > 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int slot = (int)In->Imm;
                    /* Same contract as Step/Light/Prod: owned blob address is
                     * valid after DECL even when LocalInit is still 0. */
                    if (!Fr->LocalIsPtr[slot] && !Fr->LocalIsFptr[slot]
                        && Fr->LocalArraySize[slot] <= 0
                        && !Fr->LocalOwned[slot]
                        && !Fr->LocalInit[slot])
                    {
                        if (Fr->LocalName[slot] != NULL)
                            ParserSetCaretOnIdent(LinParser, Fr->LocalName[slot]);
                        ProgramFail(LinParser, "uninitialized variable '%s'",
                            Fr->LocalName[slot] ? Fr->LocalName[slot] : "?");
                    }
                }
                /* Owned blob: prefer LocalVal address (Locals[] may lag). */
                if (In->Imm >= 0 && In->Imm < IR_CALL_MAX_LOCALS
                    && IrCallDepth > 0
                    && IrCallStack[IrCallDepth - 1]->LocalOwned[In->Imm]
                    && IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm] != 0)
                    R[In->Dest] = IrCallStack[IrCallDepth - 1]->LocalVal[In->Imm];
                else if (In->Imm >= 0 && In->Imm < NLocals)
                    R[In->Dest] = Locals[In->Imm];
                else if (In->Imm < 0)
                {
                    int ps = (int)(-(In->Imm + 1));
                    /* Nested full-frame under light parent: use this frame ParamVal. */
                    if (IrCallDepth > 0 && ps >= 0 && ps < IR_CALL_MAX_PARAMS
                        && ps < IrCallStack[IrCallDepth - 1]->NumParams)
                        R[In->Dest] = IrCallStack[IrCallDepth - 1]->ParamVal[ps];
                    else if (LightSp > 0 && ps >= 0 && ps < IR_CALL_MAX_PARAMS)
                        R[In->Dest] = LightParam[LightSp - 1][ps];
                    else if (IrCallDepth > 0 && ps >= 0 && ps < IR_CALL_MAX_PARAMS)
                        R[In->Dest] = IrCallStack[IrCallDepth - 1]->ParamVal[ps];
                    else
                        R[In->Dest] = 0;
                }
                else
                    R[In->Dest] = 0;
                ip++;
                break;
            case IrLinStoreLocal:
                if (In->Imm >= 0 && In->Imm < IR_CALL_MAX_LOCALS
                    && IrCallDepth > 0
                    && IrCallStack[IrCallDepth - 1]->LocalIsConst[In->Imm]
                    && LinParser != NULL)
                {
                    if (LinParser->CharacterPos > 0)
                        LinParser->CharacterPos++;
                    ProgramFail(LinParser, "assignment of read-only variable");
                }
                if (In->Tok == 1 && In->Imm >= 0 && In->Imm < IR_CALL_MAX_LOCALS
                    && IrCallDepth > 0)
                    IrCallStack[IrCallDepth - 1]->LocalIsConst[In->Imm] = 1;
                if (In->Imm >= 0 && In->Imm < NLocals)
                {
                    long long sv = R[In->A];
                    if (IrCallDepth > 0 && In->Imm < IR_CALL_MAX_LOCALS)
                    {
                        IrCallFrame *Frs = IrCallStack[IrCallDepth - 1];
                        struct ValueType *LT = Frs->LocalFromType[In->Imm]
                            ? Frs->LocalFromType[In->Imm]
                            : Frs->LocalTyp[In->Imm];
                        /* Struct/union owned blob: memcpy by-value (s2=s1, t=make()). */
                        if (Frs->LocalOwned[In->Imm] && Frs->LocalArraySize[In->Imm] == 0
                            && LT != NULL
                            && (LT->Base == TypeStruct || LT->Base == TypeUnion))
                        {
                            int sz = LT->Sizeof;
                            if (sv != 0 && sz > 0)
                                IrLinStructAssignBlob(Frs, (int)In->Imm,
                                    (void *)(intptr_t)sv, sz);
                            /* keep Locals[slot] as blob address */
                            Frs->LocalInit[In->Imm] = 1;
                        }
                        else
                        {
                        /* Never narrow pointer/array slots — truncates addr to 32-bit. */
                        if (Frs->LocalIsPtr[In->Imm]
                            || Frs->LocalArraySize[In->Imm] > 0
                            || (LT != NULL && (LT->Base == TypePointer
                                || LT->Base == TypeArray)))
                            ; /* keep full width */
                        else if (LT != NULL && LT->Base == TypeUnsignedInt)
                            sv = (long long)(unsigned int)sv; /* zero-extend */
                        else if (LT != NULL && LT->Base == TypeInt)
                            sv = (long long)(int)sv;
                        else if (LT != NULL && (LT->Base == TypeUnsignedShort
                                              || LT->Base == TypeShort))
                        {
                            if (LT->Base == TypeUnsignedShort)
                                sv = (long long)(unsigned short)sv;
                            else
                                sv = (long long)(short)sv;
                        }
                        else if (LT != NULL && (LT->Base == TypeUnsignedChar
                                              || LT->Base == TypeChar))
                        {
                            if (LT->Base == TypeUnsignedChar)
                                sv = (long long)(unsigned char)sv;
                            else
                                sv = (long long)(char)sv;
                        }
                        Locals[In->Imm] = sv;
                        Frs->LocalInit[In->Imm] = 1;
                        }
                    }
                    else
                    {
                        Locals[In->Imm] = sv;
                        if (IrCallDepth > 0 && In->Imm < IR_CALL_MAX_LOCALS)
                            IrCallStack[IrCallDepth - 1]->LocalInit[In->Imm] = 1;
                    }
                }
                else if (In->Imm < 0)
                {
                    int ps = (int)(-(In->Imm + 1));
                    if (LightSp > 0 && ps >= 0 && ps < IR_CALL_MAX_PARAMS)
                        LightParam[LightSp - 1][ps] = R[In->A];
                    else if (IrCallDepth > 0 && ps >= 0 && ps < IR_CALL_MAX_PARAMS)
                        IrCallStack[IrCallDepth - 1]->ParamVal[ps] = R[In->A];
                }
                ip++;
                break;
            case IrLinBin:
            {
                int u = (In->Imm & IR_LIN_F_UNSIGNED) != 0;
                int wide = (In->Imm & IR_LIN_F_WIDE) != 0;
                long long a = R[In->A], b = R[In->B];
                if (In->Imm & IR_LIN_F_FP)
                {
                    union { long long l; double d; } xa, xb, xr;
                    xa.l = a; xb.l = b;
                    if (In->Tok == TokenPlus) xr.d = xa.d + xb.d;
                    else if (In->Tok == TokenMinus) xr.d = xa.d - xb.d;
                    else if (In->Tok == TokenAsterisk) xr.d = xa.d * xb.d;
                    else if (In->Tok == TokenSlash)
                        xr.d = (xb.d != 0.0) ? (xa.d / xb.d) : 0.0;
                    else xr.l = 0;
                    R[In->Dest] = xr.l;
                    if (In->Dest < IR_LIN_REGS) LinRegU[In->Dest] = 0;
                    ip++;
                    break;
                }
                if ((In->Tok == TokenSlash || In->Tok == TokenModulus) && b == 0)
                {
                    if (LinParser != NULL)
                    {
                        (void)IrApplyBinOpRaw(LinParser, (enum LexToken)In->Tok,
                                              a, b, u, wide);
                        /* ProgramFail does not return */
                    }
                    LinDiv0 = 1; ip = -1; goto done_step;
                }
                /* Inline the common integer operators to avoid the two-call
                 * IrApplyBinOpRaw -> IrApplyBinOpFlags path on every bytecode.
                 * Division/modulus keep the checked helper for diagnostics. */
                switch ((enum LexToken)In->Tok)
                {
                    case TokenPlus:
                        if (!u && !wide)
                            R[In->Dest] = (long long)(int)((unsigned int)a + (unsigned int)b);
                        else if (u && !wide)
                            R[In->Dest] = (long long)(unsigned int)((unsigned int)a + (unsigned int)b);
                        else
                            R[In->Dest] = (long long)((unsigned long long)a + (unsigned long long)b);
                        break;
                    case TokenMinus:
                        if (!u && !wide)
                            R[In->Dest] = (long long)(int)((unsigned int)a - (unsigned int)b);
                        else if (u && !wide)
                            R[In->Dest] = (long long)(unsigned int)((unsigned int)a - (unsigned int)b);
                        else
                            R[In->Dest] = (long long)((unsigned long long)a - (unsigned long long)b);
                        break;
                    case TokenAsterisk:
                        if (!u && !wide)
                            R[In->Dest] = (long long)(int)((unsigned int)a * (unsigned int)b);
                        else if (u && !wide)
                            R[In->Dest] = (long long)(unsigned int)((unsigned int)a * (unsigned int)b);
                        else
                            R[In->Dest] = (long long)((unsigned long long)a * (unsigned long long)b);
                        break;
                    case TokenEqual: R[In->Dest] = (a == b); break;
                    case TokenNotEqual: R[In->Dest] = (a != b); break;
                    case TokenLessThan:
                        R[In->Dest] = u ? (wide
                            ? (unsigned long long)a < (unsigned long long)b
                            : (unsigned int)a < (unsigned int)b) : a < b;
                        break;
                    case TokenGreaterThan:
                        R[In->Dest] = u ? (wide
                            ? (unsigned long long)a > (unsigned long long)b
                            : (unsigned int)a > (unsigned int)b) : a > b;
                        break;
                    case TokenLessEqual:
                        R[In->Dest] = u ? (wide
                            ? (unsigned long long)a <= (unsigned long long)b
                            : (unsigned int)a <= (unsigned int)b) : a <= b;
                        break;
                    case TokenGreaterEqual:
                        R[In->Dest] = u ? (wide
                            ? (unsigned long long)a >= (unsigned long long)b
                            : (unsigned int)a >= (unsigned int)b) : a >= b;
                        break;
                    case TokenAmpersand: R[In->Dest] = a & b; break;
                    case TokenArithmeticOr: R[In->Dest] = a | b; break;
                    case TokenArithmeticExor: R[In->Dest] = a ^ b; break;
                    case TokenShiftLeft:
                        if (!u && !wide) R[In->Dest] = (long long)(int)((unsigned int)a << (unsigned)(b & 31));
                        else if (u && wide) R[In->Dest] = (long long)((unsigned long long)a << (unsigned)(b & 63));
                        else if (u) R[In->Dest] = (long long)((unsigned int)a << (unsigned)(b & 31));
                        else R[In->Dest] = a << b;
                        break;
                    case TokenShiftRight:
                        if (!u && !wide) R[In->Dest] = (long long)((int)a >> (unsigned)(b & 31));
                        else if (u && wide) R[In->Dest] = (long long)((unsigned long long)a >> (unsigned)(b & 63));
                        else if (u) R[In->Dest] = (long long)((unsigned int)a >> (unsigned)(b & 31));
                        else R[In->Dest] = a >> b;
                        break;
                    default:
                        R[In->Dest] = IrApplyBinOpRaw(LinParser, (enum LexToken)In->Tok,
                                                       a, b, u, wide);
                        break;
                }
                if (In->Dest < IR_LIN_REGS)
                    LinRegU[In->Dest] = u ? 1 : 0;
                ip++;
                break;
            }
            case IrLinUnary:
                if (In->Tok == TokenMinus) R[In->Dest] = -R[In->A];
                else if (In->Tok == TokenUnaryNot) R[In->Dest] = !R[In->A];
                else if (In->Tok == TokenUnaryExor) R[In->Dest] = ~R[In->A];
                else if (In->Tok == TokenCast && In->Imm == 1) {
                    union { long long l; double d; } u; u.l = R[In->A];
                    R[In->Dest] = (long long)u.d;
                }
                else if (In->Tok == TokenCast && In->Imm == 2) {
                    union { long long l; double d; } u;
                    u.d = (double)R[In->A];
                    R[In->Dest] = u.l;
                }
                else if (In->Tok == TokenCast && In->Imm >= 0x100 && In->Imm < 0x100 + Type_Type) {
                    enum BaseType cb = (enum BaseType)(In->Imm - 0x100);
                    R[In->Dest] = IrNarrowToBaseType(cb, R[In->A]);
                    if (In->Dest < IR_LIN_REGS)
                        LinRegU[In->Dest] = (cb == TypeUnsignedChar || cb == TypeUnsignedShort
                            || cb == TypeUnsignedInt || cb == TypeUnsignedLong || cb == TypeUnsignedLongLong);
                }
                else R[In->Dest] = R[In->A];
                ip++;
                break;
            case IrLinBrZ:
                if (!R[In->A]) ip = (int)In->Imm; else ip++;
                break;
            case IrLinJmp:
                ip = (int)In->Imm;
                break;
            case IrLinRet:
                LinRetSeen = 1;
                LinRetVal = R[In->A];
                if (IrCallDepth > 0 && LinRetVal != 0)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    int li;
                    for (li = 0; li < IR_CALL_MAX_LOCALS; li++)
                    {
                        if (Fr->LocalOwned[li] && Fr->LocalVal[li] == LinRetVal)
                        {
                            int sz = 0;
                            void *Src = (void *)(intptr_t)LinRetVal;
                            if (Fr->LocalFromType[li] != NULL
                                && (Fr->LocalFromType[li]->Base == TypeStruct
                                    || Fr->LocalFromType[li]->Base == TypeUnion))
                                sz = Fr->LocalFromType[li]->Sizeof;
                            else if (Fr->LocalTyp[li] != NULL
                                && (Fr->LocalTyp[li]->Base == TypeStruct
                                    || Fr->LocalTyp[li]->Base == TypeUnion))
                                sz = Fr->LocalTyp[li]->Sizeof;
                            if (Src != NULL && sz > 0 && sz <= (int)sizeof(IrLinStructRetBuf))
                            {
                                memcpy(IrLinStructRetBuf, Src, (size_t)sz);
                                LinRetVal = (long long)(intptr_t)IrLinStructRetBuf;
                            }
                            break;
                        }
                    }
                }
                ip = -1;
                break;
            case IrLinLine:
                if (LinParser != NULL && In->Imm > 0)
                    LinParser->Line = (int)In->Imm;
                ip++;
                break;
            case IrLinAddI:
                R[In->Dest] = R[In->A] + In->Imm;
                ip++;
                break;
            case IrLinCopyIndex:
                if (!LinExecIndexCopy(In, R, Locals, NLocals, &ip)) ip++;
                break;
            case IrLinAddStoreLocal:
            {
                int slot = (int)In->A;
                if (slot >= 0 && slot < IR_CALL_MAX_LOCALS && IrUninitChecksOn()
                    && LinParser != NULL && IrCallDepth > 0) {
                    IrCallFrame *Chk = IrCallStack[IrCallDepth - 1];
                    if (!Chk->LocalIsPtr[slot] && Chk->LocalArraySize[slot] <= 0
                        && !Chk->LocalOwned[slot]
                        && !Chk->LocalInit[slot]) {
                        if (Chk->LocalName[slot]) ParserSetCaretOnIdent(LinParser, Chk->LocalName[slot]);
                        ProgramFail(LinParser, "uninitialized variable '%s'",
                            Chk->LocalName[slot] ? Chk->LocalName[slot] : "?");
                    }
                }
                long long raw = (slot >= 0 && slot < NLocals ? Locals[slot] : 0) + R[In->B];
                long long sv = raw;
                if (slot >= 0 && slot < NLocals && IrCallDepth > 0 && slot < IR_CALL_MAX_LOCALS)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    struct ValueType *LT = Fr->LocalFromType[slot] ? Fr->LocalFromType[slot] : Fr->LocalTyp[slot];
                    if (LT != NULL && LT->Base == TypeInt) sv = (long long)(int)sv;
                    else if (LT != NULL && LT->Base == TypeUnsignedInt) sv = (long long)(unsigned int)sv;
                    Fr->LocalVal[slot] = sv;
                    Fr->LocalInit[slot] = 1;
                }
                if (slot >= 0 && slot < NLocals) Locals[slot] = sv;
                R[In->Dest] = raw;
                if (In->Dest < IR_LIN_REGS) LinRegU[In->Dest] = 0;
                ip++;
                break;
            }
            case IrLinAddLocal:
                Step(In, R, Locals, NLocals, &ip);
                break;
            case IrLinIncG:
            case IrLinLoadGlobal:
            case IrLinStoreGlobal:
            case IrLinLoadIndex:
            case IrLinStoreIndex:
                if (!LinTryParamIndexedAccess(In, R, &ip))
                    Step(In, R, Locals, NLocals, &ip);
                break;
            case IrLinCall:
            case IrLinKernel:
            default:
                Step(In, R, Locals, NLocals, &ip);
                break;
        }
done_step: ;
        BabycLinInstExec++;
    }
    LinExecDepth--; BabycLinExecDepth = LinExecDepth;
    ok = steps < 500000000;
    if (!ok) LinStepAbort = 1;
    return ok;
}

