/* ir_mem.c — Body-IR memory primitives (D1 step 3 extract from ir_exec.c)
 *
 * Owns: bitfield extract/insert, typed load/store through pointers,
 *       pointer element size from frame metadata, init-map byte marks.
 * Does NOT own ResolvePtr/Member/AddrOf (those call IrEvalExpr → would
 * cycle with ir_eval). Bridge: ProgramFail, string-literal RO, heap check.
 */
#include "picoc.h"
#include <stdint.h>
#include <string.h>
#include "ir_runtime.h"
#include "ir_storage.h"
#include "ir_bridge.h"

unsigned long long IrBitMask(int Width)
{
    if (Width <= 0) return 0;
    if (Width >= 64) return ~0ULL;
    return (1ULL << Width) - 1ULL;
}

long long IrBitExtract(const void *Container, int BitOffset, int BitWidth, int IsSigned)
{
    /* storage units are int-sized (4 bytes) in our layout model */
    unsigned int Word = 0;
    unsigned long long Raw;
    memcpy(&Word, Container, sizeof(Word));
    Raw = ((unsigned long long)Word >> BitOffset) & IrBitMask(BitWidth);
    if (IsSigned && BitWidth > 0 && (Raw & (1ULL << (BitWidth - 1))))
        Raw |= ~IrBitMask(BitWidth);
    return (long long)Raw;
}

void IrBitInsert(void *Container, int BitOffset, int BitWidth, long long Value)
{
    unsigned int Word = 0;
    unsigned int Mask = (unsigned int)(IrBitMask(BitWidth) << BitOffset);
    unsigned int Field = (unsigned int)(((unsigned long long)Value << BitOffset) & Mask);
    memcpy(&Word, Container, sizeof(Word));
    Word = (Word & ~Mask) | Field;
    memcpy(Container, &Word, sizeof(Word));
}

/* Pointee signedness for *(p+i) / *p stores. Size alone maps to signed
 * and sign-extends unsigned char/short (ptr_deref_uchar_sum). */
static int IrPointeeUnsigned(IrExpr *E)
{
    if (E == NULL)
        return 0;
    if (E->TypeFlags & IR_TF_UNSIGNED)
        return 1;
    if (E->Kind == IrExBinOp)
        return IrPointeeUnsigned(E->Left) || IrPointeeUnsigned(E->Right);
    if (E->Kind == IrExUnary && E->Op == TokenAsterisk)
        return IrPointeeUnsigned(E->Left);
    if (E->Kind == IrExIdent && IrCallDepth > 0)
    {
        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
        struct ValueType *T = NULL;
        if (E->LocalSlot >= 0 && E->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            T = Fr->LocalTyp[E->LocalSlot];
            if (T == NULL)
                T = Fr->LocalFromType[E->LocalSlot];
        }
        else if (E->ParamSlot >= 0 && E->ParamSlot < Fr->NumParams)
            T = Fr->ParamTyp[E->ParamSlot];
        if (T != NULL && T->Base == TypeArray && T->FromType != NULL)
            T = T->FromType;
        else if (T != NULL && T->Base == TypePointer && T->FromType != NULL)
            T = T->FromType;
        if (T != NULL && (T->Base == TypeUnsignedChar || T->Base == TypeUnsignedShort
                          || T->Base == TypeUnsignedInt || T->Base == TypeUnsignedLong
                          || T->Base == TypeUnsignedLongLong))
            return 1;
    }
    return 0;
}

/* IrBaseFromElemSize: shared static inline in ir_runtime.h */

/* The local-array stride alone does not identify its leaf type.  In
 * particular, multidimensional unsigned-char/short arrays have strides 1/2
 * but must not be stored through an int* merely because the old fallback was
 * TypeInt.  DeclFromType/LocalFromType carries the declared leaf type. */
static int IrLocalArrayLeafBase(IrCallFrame *Fr, int Slot, int ElemSize)
{
    struct ValueType *T = NULL;
    if (Fr == NULL || Slot < 0 || Slot >= IR_CALL_MAX_LOCALS)
        return IrBaseFromElemSize(ElemSize, 0);
    T = Fr->LocalFromType[Slot];
    if (T == NULL)
        T = Fr->LocalTyp[Slot];
    T = IrTypePeelArray(T);
    if (T != NULL && T->Base != TypeFunction && T->Base != TypeVoid)
        return T->Base;
    return IrBaseFromElemSize(ElemSize, 0);
}

int IrPtrElemSizeOf(IrExpr *E)
{
    if (E == NULL)
        return 0;
    if (E->Kind == IrExVaArg)
    {
        struct ValueType *Vt = E->VaType;
        if (Vt != NULL && Vt->Base == TypePointer && Vt->FromType != NULL)
            return (Vt->FromType->Base == TypePointer) ? (int)sizeof(void *)
                 : (Vt->FromType->Sizeof > 0 ? (int)Vt->FromType->Sizeof : 0);
        return 0;
    }
    if (E->Kind == IrExCall)
    {
        if (E->ElemSize > 0)
            return (int)E->ElemSize;
        if (E->CachedFunc != NULL && E->CachedFunc->Val != NULL
            && E->CachedFunc->Val->FuncDef.ReturnType != NULL
            && E->CachedFunc->Val->FuncDef.ReturnType->Base == TypePointer
            && E->CachedFunc->Val->FuncDef.ReturnType->FromType != NULL
            && E->CachedFunc->Val->FuncDef.ReturnType->FromType->Sizeof > 0)
            return E->CachedFunc->Val->FuncDef.ReturnType->FromType->Sizeof;
        return 0;
    }
    if (IrCallDepth <= 0)
        return 0;
    /* Index and dereference expressions are values, not pointer values, unless
     * their resulting type is itself a pointer.  In particular, the outer
     * dereference in **pp has an element-size hint inherited from pp in some
     * parser/lowering paths; using that hint here turns `**pp + 1` into pointer
     * arithmetic (on 64-bit hosts, 7 + sizeof(void*) == 15). */
    /* Nested pointer arithmetic: (p + 9) - p, (text + 5) - 2. The sum is a
     * pointer when exactly one operand is; p - n keeps p's stride. */
    if (E->Kind == IrExBinOp && (E->Op == TokenPlus || E->Op == TokenMinus))
    {
        int l = IrPtrElemSizeOf(E->Left);
        int r = IrPtrElemSizeOf(E->Right);
        if (E->Op == TokenPlus)
            return (l > 0 && r <= 0) ? l : (r > 0 && l <= 0) ? r : 0;
        return (l > 0 && r <= 0) ? l : 0;
    }
    /* a[i] of T *a[N]: a pointer element with declared pointee T. */
    if (E->Kind == IrExIndex && E->PointeeType != NULL
        && E->PointeeType->Sizeof > 0)
        return (int)E->PointeeType->Sizeof;
    if (E->Kind == IrExIndex)
        return 0;
    if ((E->TypeFlags & IR_TF_INT) && !(E->TypeFlags & IR_TF_PTR))
        return 0;
    if (E->Kind == IrExUnary && E->Op == TokenAsterisk
        && !(E->TypeFlags & IR_TF_PTR))
        return 0;
    /* ElemSize is a scale only for pointer-valued expressions. */
    if (E->ElemSize > 0)
    {
        if (!(E->TypeFlags & IR_TF_PTR))
            return 0;
        return E->ElemSize;
    }
    if (E->Kind == IrExIdent)
    {
        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
        if (E->LocalSlot >= 0 && E->LocalSlot < IR_CALL_MAX_LOCALS
            && Fr->LocalIsPtr[E->LocalSlot])
        {
            int ps = IrFrameGetPointeeSize(Fr, E->LocalSlot);
            struct ValueType *LFT = Fr->LocalFromType[E->LocalSlot];
            struct ValueType *LT = Fr->LocalTyp[E->LocalSlot];
            if (ps > 0)
                return ps;
            if (LFT != NULL && LFT->Sizeof > 0 && LFT->Base != TypePointer)
                return LFT->Sizeof;
            if (LT != NULL && LT->Base == TypePointer && LT->FromType != NULL
                && LT->FromType->Sizeof > 0)
                return LT->FromType->Sizeof;
            /* Never use LocalElemSize here: it is often sizeof(void*). */
            return (int)sizeof(int);
        }
        if (E->ParamSlot >= 0 && E->ParamSlot < Fr->NumParams
            && Fr->ParamIsPtr[E->ParamSlot])
        {
            struct ValueType *PT = Fr->ParamTyp[E->ParamSlot];
            if (PT != NULL && PT->FromType != NULL && PT->FromType->Sizeof > 0)
                return PT->FromType->Sizeof;
            return 1;
        }
        return 0;
    }
    /* a[i] yields an element value — do NOT inherit array scale for a[i]+n */
    if (E->Kind == IrExIndex)
        return 0;
    /* Unary * is a load of pointee — not a pointer value for p+n scale */
    if (E->Kind == IrExBinOp && (E->Op == TokenPlus || E->Op == TokenMinus))
    {
        /* Only pointer-typed ± yields a pointer for further scale. */
        if (!(E->TypeFlags & IR_TF_PTR))
            return 0;
        {
            int L = IrPtrElemSizeOf(E->Left);
            if (L > 0) return L;
            return IrPtrElemSizeOf(E->Right);
        }
    }
    if (E->Kind == IrExMember)
        return 0; /* member scale handled via IrResolveMember */
    return 0;
}

int IrFrameIsByteMap(IrCallFrame *Fr, int slot)
{
    struct ValueType *t;
    if (Fr == NULL || slot < 0 || slot >= IR_CALL_MAX_LOCALS)
        return 0;
    t = Fr->LocalFromType[slot];
    /* Array locals retain the declared array type in some VM entry paths.
     * Their init map must still track bytes when the ultimate element is an
     * aggregate; element-level tracking incorrectly marks every field of
     * v[i] initialized after a single member store. */
    while (t != NULL && t->Base == TypeArray)
        t = t->FromType;
    return (t != NULL && (t->Base == TypeStruct || t->Base == TypeUnion));
}

int IrFrameInitMapLen(IrCallFrame *Fr, int slot)
{
    int n, es;
    if (Fr == NULL || slot < 0 || slot >= IR_CALL_MAX_LOCALS
        || Fr->LocalInitMap[slot] == NULL)
        return 0;
    if (IrFrameIsByteMap(Fr, slot))
    {
        struct ValueType *t = Fr->LocalFromType[slot];
        while (t != NULL && t->Base == TypeArray)
            t = t->FromType;
        es = (t != NULL && t->Sizeof > 0)
            ? t->Sizeof
            : (Fr->LocalElemSize[slot] > 0 ? (int)Fr->LocalElemSize[slot] : 1);
        n = Fr->LocalArraySize[slot] > 0 ? Fr->LocalArraySize[slot] : 1;
        return n * es;
    }
    return Fr->LocalArraySize[slot] > 0 ? Fr->LocalArraySize[slot] : 0;
}

void IrFrameMarkBytes(IrCallFrame *Fr, int slot, void *Addr, int Size)
{
    char *base;
    int off, i, n;
    if (Fr == NULL || slot < 0 || slot >= IR_CALL_MAX_LOCALS || Addr == NULL
        || Fr->LocalInitMap[slot] == NULL || Fr->LocalVal[slot] == 0)
        return;
    if (Size < 1) Size = 1;
    base = (char *)(intptr_t)Fr->LocalVal[slot];
    off = (int)((char *)Addr - base);
    Fr->LocalInit[slot] = 1;
    if (IrFrameIsByteMap(Fr, slot))
    {
        n = IrFrameInitMapLen(Fr, slot);
        for (i = 0; i < Size; i++)
        {
            int ei = off + i;
            if (ei >= 0 && ei < n)
                Fr->LocalInitMap[slot][ei] = 1;
        }
    }
    else
    {
        int es = Fr->LocalElemSize[slot] > 0 ? Fr->LocalElemSize[slot] : 1;
        int ei = es > 0 ? off / es : 0;
        n = Fr->LocalArraySize[slot];
        if (ei >= 0 && ei < n)
            Fr->LocalInitMap[slot][ei] = 1;
    }
}

long long IrLoadPtrElem(int ElemBase, void *Addr)
{
    switch (ElemBase)
    {
        case TypeChar:
            return (long long)*(signed char *)Addr;
        case TypeUnsignedChar:
            return (long long)*(unsigned char *)Addr;
        case TypeShort:
            return (long long)*(short *)Addr;
        case TypeUnsignedShort:
            return (long long)*(unsigned short *)Addr;
        case TypeInt:
            return (long long)*(int *)Addr;
        case TypeUnsignedInt:
            return (long long)*(unsigned int *)Addr;
        case TypeLong:
            return (long long)*(long *)Addr;
        case TypeUnsignedLong:
            return (long long)*(unsigned long *)Addr;
        case TypeLongLong:
            return *(long long *)Addr;
        case TypeUnsignedLongLong:
            return (long long)*(unsigned long long *)Addr;
        case TypePointer:
        case TypeArray:
            return (long long)(intptr_t)(*(void **)Addr);
#ifndef NO_FP
        case TypeFP:
        {
            union { double d; long long l; } u;
            u.d = *(double *)Addr;
            return u.l;
        }
#endif
        default:
            return *(int *)Addr;
    }
}


/* F3: compile-known string-literal store ("lit"[i] = , *"lit" =).
 * Interior offsets still go through IrStorePtrElem's literal-start check. */
int IrAssignTargetIsStringLiteral(IrExpr *E)
{
    IrExpr *B;
    if (E == NULL)
        return 0;
    if (E->Kind == IrExIndex || (E->Kind == IrExUnary && E->Op == TokenAsterisk))
    {
        B = E->Left;
        if (B != NULL && B->Kind == IrExConst && B->Op == TokenStringConstant)
            return 1;
    }
    return 0;
}

void IrStorePtrElem(struct ParseState *Parser, int ElemBase, void *Addr, long long V)
{
    /* Safety checks only when Addr cannot be frame-local storage.
     * Full VariableAddressIsStringLiteral scans the whole literal table (O(n))
     * and BabycHeapCheckWrite walks all live mallocs — both were ~2× on
     * messages_manager. Fast path: exact string-literal start + heap only
     * if any live blocks exist. */
    if (Parser != NULL && Addr != NULL)
    {
        int skip = 0;
        if (IrCallDepth > 0)
        {
            uintptr_t A = (uintptr_t)Addr;
            int fi;
            /* A pointer may escape a caller frame and be written by a callee.
             * Check every live frame, not only the top frame: caller LocalVal
             * cells and arenas are frame storage too, not heap/string memory. */
            for (fi = IrCallDepth - 1; fi >= 0 && !skip; --fi)
            {
                IrCallFrame *Fr = IrCallStack[fi];
                uintptr_t Lo = (uintptr_t)&Fr->LocalVal[0];
                uintptr_t Hi = (uintptr_t)&Fr->LocalVal[IR_CALL_MAX_LOCALS];
                if (A >= Lo && A < Hi)
                    skip = 1;
                else if (Fr->Arena != NULL)
                {
                    uintptr_t AL = (uintptr_t)Fr->Arena;
                    uintptr_t AH = AL + (uintptr_t)Fr->ArenaCap;
                    if (A >= AL && A < AH)
                        skip = 1;
                }
            }
        }
        if (!skip)
        {
            /* O(1) hash: Addr is interned literal start (covers p[0]='x' on
             * char *p = "lit"). Interior p[i] i>0 remains rare UB; full table
             * scan was ~2× on messages_manager and is not done here. */
            if (VariableStringLiteralGet(Parser->pc, (char *)Addr) != NULL)
            {
                ParserSetCaretOnAssignTarget(Parser);
                ProgramFail(Parser, "assignment of read-only location (string literal)");
            }
            {
                int nbytes = (int)sizeof(int);
                if (ElemBase == TypeChar || ElemBase == TypeUnsignedChar) nbytes = 1;
                else if (ElemBase == TypeShort || ElemBase == TypeUnsignedShort) nbytes = 2;
                else if (ElemBase == TypeLong || ElemBase == TypeUnsignedLong) nbytes = (int)sizeof(long);
                else if (ElemBase == TypeLongLong || ElemBase == TypeUnsignedLongLong) nbytes = 8;
                else if (ElemBase == TypePointer || ElemBase == TypeArray) nbytes = (int)sizeof(void *);
#ifndef NO_FP
                else if (ElemBase == TypeFP) nbytes = (int)sizeof(double);
#endif
                BabycHeapCheckWrite(Parser, Addr, nbytes);
            }
        }
    }
    /* Local scalar FP values use a canonical 64-bit double-bit slot, including
     * language-level float values. A native 4-byte store through &local would
     * only overwrite half of that representation. When the target is exactly
     * a scalar FP LocalVal cell, update its canonical value and apply float
     * rounding according to the declared local type. */
#ifndef NO_FP
    if (Addr != NULL && IrCallDepth > 0)
    {
        uintptr_t A = (uintptr_t)Addr;
        int fi;
        /* FP locals are represented as canonical 64-bit values in LocalVal.
         * The pointee may belong to any active caller frame (e.g. a callee
         * receives &caller_float), so locate the owning frame by exact slot
         * address before falling back to a native-width memory store. */
        for (fi = IrCallDepth - 1; fi >= 0; --fi)
        {
            IrCallFrame *Fr = IrCallStack[fi];
            uintptr_t Lo = (uintptr_t)&Fr->LocalVal[0];
            uintptr_t Hi = (uintptr_t)&Fr->LocalVal[IR_CALL_MAX_LOCALS];
            if (A < Lo || A >= Hi || ((A - Lo) % sizeof(long long)) != 0)
                continue;
            {
                int slot = (int)((A - Lo) / sizeof(long long));
                struct ValueType *T = slot >= 0 && slot < IR_CALL_MAX_LOCALS
                    ? Fr->LocalTyp[slot] : NULL;
                union { long long l; double d; } value;
                if (T == NULL && slot >= 0 && slot < IR_CALL_MAX_LOCALS)
                    T = Fr->LocalFromType[slot];
                if (slot >= 0 && slot < IR_CALL_MAX_LOCALS
                    && !Fr->LocalIsPtr[slot] && Fr->LocalArraySize[slot] <= 0
                    && T != NULL && T->Base == TypeFP)
                {
                    value.l = V;
                    if (T->FPFormat == FPStorageBinary32)
                    {
                        float f = (float)value.d;
                        value.d = (double)f;
                    }
                    Fr->LocalVal[slot] = value.l;
                    Fr->LocalInit[slot] = 1;
                    return;
                }
                /* Exact LocalVal slot, but not an FP scalar: continue through
                 * the existing typed store path. */
                break;
            }
        }
    }
#endif
    switch (ElemBase)
    {
        case TypeChar: case TypeUnsignedChar:
            *(char *)Addr = (char)V; break;
        case TypeShort: case TypeUnsignedShort:
            *(short *)Addr = (short)V; break;
        case TypeInt: case TypeUnsignedInt:
            *(int *)Addr = (int)V; break;
        case TypeLong: case TypeUnsignedLong:
            *(long *)Addr = (long)V; break;
        case TypeLongLong: case TypeUnsignedLongLong:
            *(long long *)Addr = V; break;
        case TypePointer:
        case TypeArray:
            *(void **)Addr = (void *)(intptr_t)V; break;
#ifndef NO_FP
        case TypeFP:
        {
            union { long long l; double d; } u;
            u.l = V;
            *(double *)Addr = u.d; break;
        }
#endif
        default:
            *(int *)Addr = (int)V; break;
    }
    /* Scalar locals live in LocalVal[] (8 bytes). A 1/2-byte store must
     * sign/zero-extend the slot or ident reads see the unsigned residue
     * (ptr_narrow_store_short: (*p)-- on short -> 65236). */
    if (IrCallDepth > 0 && Addr != NULL)
    {
        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
        char *SlotBase = (char *)&Fr->LocalVal[0];
        char *A = (char *)Addr;
        if (A >= SlotBase && A < SlotBase + (int)sizeof(Fr->LocalVal))
        {
            int slot = (int)((A - SlotBase) / (int)sizeof(long long));
            int inner = (int)((A - SlotBase) % (int)sizeof(long long));
            if (inner == 0 && slot >= 0 && slot < IR_CALL_MAX_LOCALS
                && Fr->LocalArraySize[slot] <= 0 && !Fr->LocalIsPtr[slot])
            {
                switch (ElemBase)
                {
                    case TypeChar:
                        Fr->LocalVal[slot] = (long long)*(signed char *)Addr; break;
                    case TypeUnsignedChar:
                        Fr->LocalVal[slot] = (long long)*(unsigned char *)Addr; break;
                    case TypeShort:
                        Fr->LocalVal[slot] = (long long)*(short *)Addr; break;
                    case TypeUnsignedShort:
                        Fr->LocalVal[slot] = (long long)*(unsigned short *)Addr; break;
                    case TypeInt:
                        Fr->LocalVal[slot] = (long long)*(int *)Addr; break;
                    case TypeUnsignedInt:
                        Fr->LocalVal[slot] = (long long)*(unsigned int *)Addr; break;
                    default:
                        break;
                }
            }
        }
    }
}


/* ---- Resolve / AddrOf (D1 post: from ir_exec) ---- */
int IrAddrOf(struct ParseState *Parser, IrExpr *E,
                    void **AddrOut, int *ElemBaseOut, int *ElemSizeOut)
{
    void *Base = NULL;
    int Eb = TypeInt, Es = (int)sizeof(int);
    long long Idx;
    (void)Idx;

    if (E == NULL)
        return 0;

    if (E->Kind == IrExIdent)
    {
        /* Address-of a pointer local must name the pointer slot itself, not
         * the pointee held in LocalVal. IrResolvePtr intentionally resolves a
         * pointer identifier to its current target for dereference/store paths;
         * reusing that result for &p loses one level of indirection and breaks
         * **pp when pp aliases a local pointer variable. */
        if (E->LocalSlot >= 0 && IrCallDepth > 0
            && E->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            int slot = E->LocalSlot;
            struct ValueType *T = Fr->LocalTyp[slot] != NULL
                ? Fr->LocalTyp[slot] : Fr->LocalFromType[slot];
            if ((E->TypeFlags & IR_TF_PTR) != 0
                || (T != NULL && T->Base == TypePointer)
                || Fr->LocalIsPtr[slot] || Fr->LocalIsFptr[slot])
            {
                *AddrOut = (void *)&Fr->LocalVal[slot];
                if (ElemBaseOut) *ElemBaseOut = TypePointer;
                if (ElemSizeOut) *ElemSizeOut = (int)sizeof(void *);
                return 1;
            }
        }
        if (E->ParamSlot >= 0 && IrCallDepth > 0
            && E->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            int slot = E->ParamSlot;
            struct ValueType *T = Fr->ParamTyp[slot];
            if ((E->TypeFlags & IR_TF_PTR) != 0
                || (T != NULL && T->Base == TypePointer)
                || Fr->ParamIsPtr[slot])
            {
                *AddrOut = (void *)&Fr->ParamVal[slot];
                if (ElemBaseOut) *ElemBaseOut = TypePointer;
                if (ElemSizeOut) *ElemSizeOut = (int)sizeof(void *);
                return 1;
            }
        }
        return IrResolvePtr(Parser, E, AddrOut, ElemBaseOut, ElemSizeOut);
    }
    if (E->Kind == IrExMember)
        return IrResolvePtr(Parser, E, AddrOut, ElemBaseOut, ElemSizeOut);
    /* &a[i]: need element storage address. IrResolvePtr for pointer-arrays
     * loads the pointer word (for a[i][j]); here we stop at the slot. */
    if (E->Kind == IrExIndex)
    {
        void *BaseAddr = NULL;
        int Eb = TypeInt, Es = (int)sizeof(int);
        long long Idx;
        IrExpr *Root = E->Left;
        if (Root != NULL && Root->Kind == IrExIdent
            && Root->LocalSlot >= 0 && IrCallDepth > 0
            && Root->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            if (Fr->LocalArraySize[Root->LocalSlot] > 0
                && Fr->LocalVal[Root->LocalSlot] != 0)
            {
                Es = Fr->LocalElemSize[Root->LocalSlot] > 0
                    ? Fr->LocalElemSize[Root->LocalSlot]
                    : (int)sizeof(void *);
                if (Fr->LocalElemIsPtr[Root->LocalSlot] || Fr->LocalIsFptr[Root->LocalSlot])
                    Es = (int)sizeof(void *);
                Idx = IrEvalExpr(Parser, E->Right);
                *AddrOut = (char *)(void *)(intptr_t)Fr->LocalVal[Root->LocalSlot]
                    + (ptrdiff_t)Idx * Es;
                *ElemBaseOut = TypePointer;
                *ElemSizeOut = Es;
                return 1;
            }
        }
        if (!IrResolvePtr(Parser, E->Left, &BaseAddr, &Eb, &Es))
            return 0;
        Idx = IrEvalExpr(Parser, E->Right);
        *AddrOut = (char *)BaseAddr + (ptrdiff_t)Idx * Es;
        *ElemBaseOut = Eb;
        *ElemSizeOut = Es;
        return 1;
    }

    /* *(p+expr) / *(p-expr) — Ident index handled by existing fast paths */
    if (E->Kind == IrExUnary && E->Op == TokenAsterisk
        && E->Left != NULL
        && E->Left->Kind == IrExBinOp
        && (E->Left->Op == TokenPlus || E->Left->Op == TokenMinus))
    {
        long long Ptr, Off;
        int EsL = IrPtrElemSizeOf(E->Left->Left);
        if (EsL <= 0)
            EsL = IrPtrElemSizeOf(E->Left->Right);
        if (EsL <= 0 && E->Left->Left != NULL && E->Left->Left->Kind == IrExMember)
            EsL = IrMemberPtrElemSize(Parser, E->Left->Left);
        if (EsL <= 0 && E->Left->Left != NULL && E->Left->Left->Kind == IrExCall)
        {
            struct Value *FV = NULL;
            if (E->Left->Left->CachedFunc != NULL)
                FV = E->Left->Left->CachedFunc;
            else if (E->Left->Left->Ident != NULL)
                IrVarTableGet(Parser, E->Left->Left->Ident, &FV);
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                && FV->Val->FuncDef.ReturnType != NULL
                && FV->Val->FuncDef.ReturnType->Base == TypePointer
                && FV->Val->FuncDef.ReturnType->FromType != NULL
                && FV->Val->FuncDef.ReturnType->FromType->Sizeof > 0)
            {
                EsL = FV->Val->FuncDef.ReturnType->FromType->Sizeof;
                E->Left->Left->ElemSize = (unsigned char)(EsL < 256 ? EsL : 0);
            }
        }
        if (EsL <= 0)
            return 0; /* refuse unscaled guess */
        Ptr = IrEvalExpr(Parser, E->Left->Left);
        Off = IrEvalExpr(Parser, E->Left->Right);
        if (E->Left->Op == TokenPlus)
            Base = (void *)(intptr_t)(Ptr + Off * (long long)EsL);
        else
            Base = (void *)(intptr_t)(Ptr - Off * (long long)EsL);
        *AddrOut = Base;
        *ElemBaseOut = IrBaseFromElemSize(EsL, IrPointeeUnsigned(E->Left));
        *ElemSizeOut = EsL;
        return Base != NULL;
    }

    /* p + n  /  p - n  as address (decay) */
    if (E->Kind == IrExBinOp && (E->Op == TokenPlus || E->Op == TokenMinus))
    {
        long long Ptr, Off;
        int EsL = IrPtrElemSizeOf(E->Left);
        if (EsL <= 0)
            return 0;
        Ptr = IrEvalExpr(Parser, E->Left);
        Off = IrEvalExpr(Parser, E->Right);
        if (E->Op == TokenPlus)
            Base = (void *)(intptr_t)(Ptr + Off * (long long)EsL);
        else
            Base = (void *)(intptr_t)(Ptr - Off * (long long)EsL);
        *AddrOut = Base;
        *ElemBaseOut = IrBaseFromElemSize(EsL, IrPointeeUnsigned(E->Left));
        *ElemSizeOut = EsL;
        return 1;
    }
    return 0;
}

/* Bounds check an index in a statically sized array chain. Depth selects
 * the dimension represented by this index (grid[i] => 0, grid[i][j] => 1).
 * Pointer expressions remain unchecked because their allocation extent is
 * not generally available. */
static void IrCheckIndexBounds(struct ParseState *Parser, IrExpr *Index, long long Idx)
{
    IrExpr *Root = Index;
    int Depth = 0, Dim = 0;
    struct ValueType *T = NULL;
    while (Root != NULL && Root->Kind == IrExIndex)
    {
        Depth++;
        Root = Root->Left;
    }
    if (!IrSafeChecksOn() || Root == NULL || Root->Kind != IrExIdent || Depth <= 0)
        return;
    if (Root->LocalSlot >= 0 && IrCallDepth > 0
        && Root->LocalSlot < IR_CALL_MAX_LOCALS)
    {
        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
        if (Depth <= Fr->LocalNDims[Root->LocalSlot])
            Dim = Fr->LocalDims[Root->LocalSlot][Depth - 1];
    }
    else if (Root->ParamSlot >= 0 && IrCallDepth > 0
             && Root->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
    {
        T = IrCallStack[IrCallDepth - 1]->ParamTyp[Root->ParamSlot];
        if (T != NULL && T->Base == TypePointer) T = T->FromType;
        for (int d = 1; d < Depth && T != NULL && T->Base == TypeArray; ++d)
            T = T->FromType;
        if (T != NULL && T->Base == TypeArray) Dim = T->ArraySize;
    }
    else if (Root->LocalSlot < 0 && Root->ParamSlot < 0 && Root->Ident != NULL)
    {
        struct Value *GV = IrGlobalValSoft(Parser, Root);
        if (GV != NULL) T = GV->Typ;
        for (int d = 1; d < Depth && T != NULL && T->Base == TypeArray; ++d)
            T = T->FromType;
        if (T != NULL && T->Base == TypeArray) Dim = T->ArraySize;
    }
    if (Dim > 0 && (Idx < 0 || Idx >= Dim))
        ProgramFail(Parser, DiagnosticMsgArrayIndexOutOfBoundsSized, (int)Idx, Dim);
}

/* Declared type of a local/global root identifier after applying Levels dereferences (NULL if unknown).
 * LocalFromType is already one dereference down (pointee or element type). Parameters carry no type here. */
static struct ValueType *IrDeclTypeAfterDerefs(struct ParseState *Parser, IrExpr *Root, int Levels)
{
    struct ValueType *T = NULL;
    int i;
    if (Root == NULL || Levels < 1)
        return NULL;
    if (Root->LocalSlot >= 0)
    {
        if (IrCallDepth <= 0 || Root->LocalSlot >= IR_CALL_MAX_LOCALS)
            return NULL;
        T = IrCallStack[IrCallDepth - 1]->LocalFromType[Root->LocalSlot];
        Levels -= 1;
    }
    else if (Root->ParamSlot < 0 && Root->Ident != NULL)
    {
        struct Value *GV = IrGlobalValSoft(Parser, Root);
        T = GV != NULL ? GV->Typ : NULL;
    }
    else
        return NULL;
    for (i = 0; T != NULL && i < Levels; i++)
    {
        if (T->Base != TypePointer && T->Base != TypeArray)
            return NULL;
        T = T->FromType;
    }
    return T;
}

static int IrBaseLoadableByPtrElem(int B)
{
    switch (B)
    {
    case TypeChar: case TypeUnsignedChar: case TypeShort: case TypeUnsignedShort:
    case TypeInt: case TypeUnsignedInt: case TypeLong: case TypeUnsignedLong:
    case TypeLongLong: case TypeUnsignedLongLong: case TypePointer:
#ifndef NO_FP
    case TypeFP:
#endif
        return 1;
    default:
        return 0;
    }
}

int IrResolvePtr(struct ParseState *Parser, IrExpr *Base,
                        void **AddrOut, int *ElemBaseOut, int *ElemSizeOut)
{
    struct Value *Var = NULL;
    struct ValueType *Typ = NULL;
    void *Addr = NULL;
    int ElemBase = TypeInt;
    int ElemSize = (int)sizeof(int);

    if (Base == NULL)
        return 0;

    /* va_arg(ap, T *) is a typed pointer expression. Evaluate it once, then
     * expose the pointee type/stride to indexing and dereference. */
    if (Base->Kind == IrExVaArg && Base->VaType != NULL
        && Base->VaType->Base == TypePointer && Base->VaType->FromType != NULL)
    {
        struct ValueType *Ft = Base->VaType->FromType;
        long long PV = IrEvalExpr(Parser, Base);
        Addr = (void *)(intptr_t)PV;
        if (AddrOut) *AddrOut = Addr;
        if (ElemBaseOut) *ElemBaseOut = (int)Ft->Base;
        if (ElemSizeOut) *ElemSizeOut = (Ft->Base == TypePointer) ? (int)sizeof(void *)
                                       : (Ft->Sizeof > 0 ? (int)Ft->Sizeof : 1);
        return Addr != NULL;
    }

    /* p + n / p - n → element address (for *(p+i)=v) */
    if (Base->Kind == IrExBinOp && (Base->Op == TokenPlus || Base->Op == TokenMinus))
    {
        long long Ptr, Off;
        IrExpr *PtrExpr = Base->Left, *OffExpr = Base->Right;
        int EsL = IrPtrElemSizeOf(PtrExpr);
        /* C permits integer + pointer. The address and stride must come from
         * the pointer operand, not from AST position. */
        if (EsL <= 0 && Base->Op == TokenPlus)
        {
            int EsR = IrPtrElemSizeOf(Base->Right);
            if (EsR > 0)
            {
                PtrExpr = Base->Right;
                OffExpr = Base->Left;
                EsL = EsR;
            }
        }
        if (EsL <= 0)
            EsL = (int)sizeof(int);
        Ptr = IrEvalExpr(Parser, PtrExpr);
        Off = IrEvalExpr(Parser, OffExpr);
        if (Base->Op == TokenPlus)
            Addr = (void *)(intptr_t)(Ptr + Off * (long long)EsL);
        else
            Addr = (void *)(intptr_t)(Ptr - Off * (long long)EsL);
        ElemBase = IrBaseFromElemSize(EsL, IrPointeeUnsigned(PtrExpr));
        ElemSize = EsL;
        if (AddrOut) *AddrOut = Addr;
        if (ElemBaseOut) *ElemBaseOut = ElemBase;
        if (ElemSizeOut) *ElemSizeOut = ElemSize;
        return Addr != NULL;
    }

    /* In **array for a multidimensional local, the inner *array is a row
     * array which decays to a pointer to its first scalar; it is not a stored
     * pointer word. Preserve the row's address instead of loading data as an
     * address (the old behavior segfaulted on int m[2][2]; **m). */
    if (Base->Kind == IrExUnary && Base->Op == TokenAsterisk
        && Base->Left != NULL && Base->Left->Kind == IrExUnary
        && Base->Left->Op == TokenAsterisk && Base->Left->Left != NULL
        && Base->Left->Left->Kind == IrExIdent)
    {
        IrExpr *Root = Base->Left->Left;
        int IsMultiArray = 0, LeafSize = (int)sizeof(int);
        if (Root->LocalSlot >= 0 && IrCallDepth > 0
            && Root->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            IsMultiArray = Fr->LocalNDims[Root->LocalSlot] >= 2;
            if (Fr->LocalElemSize[Root->LocalSlot] > 0)
                LeafSize = Fr->LocalElemSize[Root->LocalSlot];
        }
        else if (Root->Ident != NULL && Root->LocalSlot < 0 && Root->ParamSlot < 0)
        {
            struct Value *GV = IrGlobalValSoft(Parser, Root);
            struct ValueType *T = GV != NULL ? GV->Typ : NULL;
            if (T != NULL && T->Base == TypeArray && T->FromType != NULL
                && T->FromType->Base == TypeArray)
            {
                IsMultiArray = 1;
                while (T != NULL && T->Base == TypeArray && T->FromType != NULL)
                    T = T->FromType;
                if (T != NULL && T->Sizeof > 0) LeafSize = (int)T->Sizeof;
            }
        }
        if (IsMultiArray)
        {
            if (!IrResolvePtr(Parser, Root, &Addr, &ElemBase, &ElemSize))
                return 0;
            if (AddrOut) *AddrOut = Addr;
            if (ElemBaseOut) *ElemBaseOut = IrBaseFromElemSize(LeafSize, 0);
            if (ElemSizeOut) *ElemSizeOut = LeafSize;
            return Addr != NULL;
        }
    }

    /* *row where row is an element of a multidimensional array produces an
     * array lvalue, which decays to a pointer to its first scalar. Return the
     * row address; do not load the first integer as though it were a pointer. */
    if (Base->Kind == IrExUnary && Base->Op == TokenAsterisk && Base->Left != NULL)
    {
        IrExpr *Root = Base->Left;
        while (Root != NULL && Root->Kind == IrExBinOp
               && (Root->Op == TokenPlus || Root->Op == TokenMinus))
        {
            if (Root->Left != NULL && Root->Left->Kind == IrExIdent)
                Root = Root->Left;
            else if (Root->Op == TokenPlus && Root->Right != NULL
                     && Root->Right->Kind == IrExIdent)
                Root = Root->Right;
            else
                break;
        }
        if (Root != NULL && Root->Kind == IrExIdent)
        {
            int IsMultiArray = 0, LeafSize = (int)sizeof(int), LeafBase = TypeInt;
            if (Root->LocalSlot >= 0 && IrCallDepth > 0
                && Root->LocalSlot < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                IsMultiArray = Fr->LocalNDims[Root->LocalSlot] >= 2;
                if (Fr->LocalElemSize[Root->LocalSlot] > 0)
                    LeafSize = Fr->LocalElemSize[Root->LocalSlot];
                if (Fr->LocalFromType[Root->LocalSlot] != NULL)
                    LeafBase = Fr->LocalFromType[Root->LocalSlot]->Base;
            }
            else if (Root->LocalSlot < 0 && Root->ParamSlot < 0 && Root->Ident != NULL)
            {
                struct Value *GV = IrGlobalValSoft(Parser, Root);
                struct ValueType *T = GV != NULL ? GV->Typ : NULL;
                if (T != NULL && T->Base == TypeArray && T->FromType != NULL
                    && T->FromType->Base == TypeArray)
                {
                    IsMultiArray = 1;
                    while (T != NULL && T->Base == TypeArray && T->FromType != NULL)
                        T = T->FromType;
                    if (T != NULL)
                    {
                        LeafBase = T->Base;
                        if (T->Sizeof > 0) LeafSize = (int)T->Sizeof;
                    }
                }
            }
            if (IsMultiArray)
            {
                int IgnoreBase = TypeInt, IgnoreSize = (int)sizeof(int);
                if (!IrResolvePtr(Parser, Base->Left, &Addr, &IgnoreBase, &IgnoreSize))
                    return 0;
                if (AddrOut) *AddrOut = Addr;
                if (ElemBaseOut) *ElemBaseOut = LeafBase;
                if (ElemSizeOut) *ElemSizeOut = LeafSize;
                return Addr != NULL;
            }
        }
    }

    /* multi-level *p / ***pppx: peel one * and load through */
    if (Base->Kind == IrExUnary && Base->Op == TokenAsterisk && Base->Left != NULL)
    {
        void *InnerAddr = NULL;
        int Ib = TypeInt, Isz = (int)sizeof(int);
        if (!IrResolvePtr(Parser, Base->Left, &InnerAddr, &Ib, &Isz) || InnerAddr == NULL)
            return 0;
        /* load pointer word at InnerAddr */
        Addr = *(void **)InnerAddr;
        if (AddrOut) *AddrOut = Addr;
        {   /* the final load must use the declared pointee type, not a hard-coded int */
            int Levels = 0, eb = TypeInt, es = (int)sizeof(int);
            IrExpr *Rt = Base;
            struct ValueType *Tf;
            while (Rt != NULL && Rt->Kind == IrExUnary && Rt->Op == TokenAsterisk)
            {
                Levels++;
                Rt = Rt->Left;
            }
            Tf = (Rt != NULL && Rt->Kind == IrExIdent) ? IrDeclTypeAfterDerefs(Parser, Rt, Levels + 1) : NULL;
            if (Tf != NULL && Tf->Sizeof > 0 && IrBaseLoadableByPtrElem(Tf->Base))
            {
                eb = Tf->Base;
                es = (int)Tf->Sizeof;
            }
            if (ElemBaseOut) *ElemBaseOut = eb;
            if (ElemSizeOut) *ElemSizeOut = es;
        }
        return Addr != NULL;
    }

    /* *f() / *ret_ptr(): call returns pointer address */
    if (Base->Kind == IrExCall)
    {
        long long PV = IrEvalExpr(Parser, Base);
        Addr = (void *)(intptr_t)PV;
        if (AddrOut) *AddrOut = Addr;
        if (Base->ElemSize <= 0 && Base->Ident != NULL)
        {
            struct Value *FV = NULL;
            IrVarTableGet(Parser, Base->Ident, &FV);
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                && FV->Val->FuncDef.ReturnType != NULL
                && FV->Val->FuncDef.ReturnType->Base == TypePointer
                && FV->Val->FuncDef.ReturnType->FromType != NULL)
            {
                struct ValueType *Ft = FV->Val->FuncDef.ReturnType->FromType;
                if (Ft->Sizeof > 0 && Ft->Sizeof < 256)
                    Base->ElemSize = (unsigned char)Ft->Sizeof;
                if (Ft->Base == TypeUnsignedChar || Ft->Base == TypeUnsignedShort
                    || Ft->Base == TypeUnsignedInt || Ft->Base == TypeUnsignedLong
                    || Ft->Base == TypeUnsignedLongLong)
                    Base->TypeFlags |= IR_TF_UNSIGNED;
            }
        }
        if (Base->ElemSize > 0) {
            if (ElemSizeOut) *ElemSizeOut = (int)Base->ElemSize;
            if (ElemBaseOut) {
                if (Base->ElemSize == 1)
                    *ElemBaseOut = (Base->TypeFlags & IR_TF_UNSIGNED) ? TypeUnsignedChar : TypeChar;
                else if (Base->ElemSize == 2)
                    *ElemBaseOut = (Base->TypeFlags & IR_TF_UNSIGNED) ? TypeUnsignedShort : TypeShort;
                else if (Base->ElemSize == 8)
                    *ElemBaseOut = (Base->TypeFlags & IR_TF_UNSIGNED) ? TypeUnsignedLongLong : TypeLongLong;
                else
                    *ElemBaseOut = (Base->TypeFlags & IR_TF_UNSIGNED) ? TypeUnsignedInt : TypeInt;
            }
        } else {
            if (ElemBaseOut) *ElemBaseOut = TypeInt;
            if (ElemSizeOut) *ElemSizeOut = (int)sizeof(int);
        }
        return Addr != NULL;
    }

    if (Base->Kind == IrExIdent)
    {
        long long PV = 0;
        int got = 0;
        /* B.2.1: slot locals/params — frame only, never VarGet */
        if (Base->ParamSlot >= 0 && IrCallDepth > 0
            && Base->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
        {
            PV = IrCallStack[IrCallDepth - 1]->ParamVal[Base->ParamSlot];
            got = 1;
            Typ = IrCallStack[IrCallDepth - 1]->ParamTyp[Base->ParamSlot];
        }
        else if (Base->LocalSlot >= 0 && IrCallDepth > 0
                 && Base->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            PV = Fr->LocalVal[Base->LocalSlot];
            got = 1;
            /* Prefer full LocalTyp; fall back to LocalFromType (pointee/elem) */
            if (Fr->LocalTyp[Base->LocalSlot] != NULL)
                Typ = Fr->LocalTyp[Base->LocalSlot];
            else if (Fr->LocalFromType[Base->LocalSlot] != NULL)
                Typ = Fr->LocalFromType[Base->LocalSlot];
            /* Owned array/struct blob: address is the storage itself */
            if (Fr->LocalOwned[Base->LocalSlot] && Fr->LocalVal[Base->LocalSlot] != 0)
                Addr = (void *)(intptr_t)Fr->LocalVal[Base->LocalSlot];
            else if ((Base->TypeFlags & IR_TF_PTR) == 0
                     && !Fr->LocalIsPtr[Base->LocalSlot]
                     && !Fr->LocalIsFptr[Base->LocalSlot]
                     && Fr->LocalArraySize[Base->LocalSlot] <= 0
                     && Typ != NULL
                     && Typ->Base != TypePointer
                     && Typ->Base != TypeStruct
                     && Typ->Base != TypeUnion
                     && Typ->Base != TypeArray
                     && Typ->Base != TypeFunction)
                /* Scalar only when declared type is a real scalar. Typ==NULL or
                 * pointee struct (T* DeclFromType) must NOT take &LocalVal — that
                 * made cur->next = prev clobber LocalVal[nx]. */
                Addr = (void *)&Fr->LocalVal[Base->LocalSlot];
            /* else pointer local: Addr stays NULL → filled from PV below */
        }
        /* global / non-slot only — reuse CachedAddr when CachedVal still live */
        if (!got && Base->Ident != NULL)
        {
            if (Base->CachedAddr != NULL && Base->CachedVal != NULL
                && !Base->CachedVal->OutOfScope)
            {
                Var = Base->CachedVal;
                Typ = Var->Typ;
                Addr = Base->CachedAddr;
            }
            else if ((IrGlobalValSoft(Parser, Base) != NULL)
                     )
            {
                Var = IrGlobalValSoft(Parser, Base);
                if (Var != NULL && Var->Typ != NULL)
                {
                    Typ = Var->Typ;
                    if (Var->Typ->Base == TypePointer)
                        Addr = Var->Val->Pointer;
                    else
                        Addr = (void *)Var->Val; /* array/struct/scalar payload */
                    /* Cache stable global base (array body / pointer word target).
                     * Pointer globals: CachedAddr is the *pointee* from Val->Pointer;
                     * if the pointer variable is reassigned, CachedVal stays but
                     * pointee changes — only cache TypeArray/struct/scalar payload. */
                    if (Addr != NULL && Var->Typ->Base != TypePointer)
                        Base->CachedAddr = Addr;
                }
            }
        }
        if (got && Addr == NULL)
            Addr = (void *)(intptr_t)PV;
        else if (!got && Addr == NULL && Var != NULL && Var->Typ != NULL)
        {
            if (Var->Typ->Base == TypePointer)
                Addr = Var->Val->Pointer;
            else
            {
                Addr = (void *)Var->Val;
                if (Addr != NULL && Var->Typ->Base != TypePointer)
                    Base->CachedAddr = Addr;
            }
        }
        else if (!got && Addr == NULL)
            return 0;

        /* Prefer ParamTyp / LocalElemSize over VarGet (Body-IR frames
         * often have no VariableDefine for params). */
        if (Base->ParamSlot >= 0 && IrCallDepth > 0
            && Base->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams
            && IrCallStack[IrCallDepth - 1]->ParamIsPtr[Base->ParamSlot])
        {
            struct ValueType *PT = IrCallStack[IrCallDepth - 1]->ParamTyp[Base->ParamSlot];
            if (PT != NULL && PT->FromType != NULL)
            {
                struct ValueType *Ft = PT->FromType;
                ElemBase = Ft->Base;
                if (Ft->Base == TypeArray)
                {
                    /* `int a[][N]` adjusts to pointer-to-row. TypeArray.Sizeof
                     * may already be pointer-sized after parameter adjustment;
                     * reconstruct the row stride from its bound and leaf type. */
                    struct ValueType *Leaf = Ft;
                    int stride = 1;
                    while (Leaf != NULL && Leaf->Base == TypeArray && Leaf->FromType != NULL)
                    {
                        if (Leaf->ArraySize > 0 && stride <= 0x3fffffff / Leaf->ArraySize)
                            stride *= Leaf->ArraySize;
                        else { stride = 0; break; }
                        Leaf = Leaf->FromType;
                    }
                    if (stride > 0 && Leaf != NULL && Leaf->Sizeof > 0
                        && stride <= 0x3fffffff / Leaf->Sizeof)
                        ElemSize = stride * (int)Leaf->Sizeof;
                    else
                        ElemSize = Ft->Sizeof > 0 ? Ft->Sizeof : (int)sizeof(int);
                }
                else if (Ft->Base == TypePointer)
                    ElemSize = (int)sizeof(void *);
                else
                    ElemSize = Ft->Sizeof > 0 ? Ft->Sizeof : 1;
            }
            else
            {
                ElemBase = TypeChar;
                ElemSize = 1;
            }
        }
        else if (Base->LocalSlot >= 0 && IrCallDepth > 0
                 && Base->LocalSlot < IR_CALL_MAX_LOCALS
                 && IrCallStack[IrCallDepth - 1]->LocalIsPtr[Base->LocalSlot])
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            struct ValueType *LFT = Fr->LocalFromType[Base->LocalSlot];
            int Es = Fr->LocalElemSize[Base->LocalSlot];
            /* T *a[N] / fptr a[N]: element is a pointer word — check before
             * treating LocalFromType as the element struct size. */
            if (Fr->LocalArraySize[Base->LocalSlot] > 0
                && (Fr->LocalIsFptr[Base->LocalSlot]
                    || Fr->LocalElemIsPtr[Base->LocalSlot]))
            {
                int nd = (int)Fr->LocalNDims[Base->LocalSlot];
                int es = (int)sizeof(void *);
                if (nd >= 2)
                {
                    int stride = es, d;
                    for (d = 1; d < nd; d++)
                    {
                        int dim = Fr->LocalDims[Base->LocalSlot][d];
                        if (dim > 0) stride *= dim;
                    }
                    ElemBase = TypeArray;
                    ElemSize = stride;
                }
                else
                {
                    ElemBase = TypePointer;
                    ElemSize = es;
                }
            }
            else if (LFT != NULL && LFT->Base == TypePointer
                && LFT->FromType != NULL && LFT->FromType->Base == TypeArray
                && LFT->FromType->Sizeof > 0
                && Fr->LocalNDims[Base->LocalSlot] < 2)
            {
                /* Parameter `T a[][N]` is pointer-to-row after C parameter
                 * adjustment. The first index must advance by sizeof(T[N]),
                 * not by the scalar pointee size stored in LocalPointeeSize. */
                ElemBase = TypeArray;
                ElemSize = LFT->FromType->Sizeof;
            }
            else if (LFT != NULL && LFT->Sizeof > 0
                && (Fr->LocalNDims[Base->LocalSlot] < 2)
                && (LFT->Base == TypeStruct || LFT->Base == TypeUnion
                    || LFT->Base == TypeArray || LFT->Base == TypeFP))
            {
                ElemBase = LFT->Base;
                ElemSize = LFT->Base == TypeFP ? TypeFPStorageSize(LFT) : LFT->Sizeof;
            }
            else if (Fr->LocalArraySize[Base->LocalSlot] > 0
                     && Fr->LocalNDims[Base->LocalSlot] >= 2)
            {
                /* multi-dim local: first index steps by product of trailing dims */
                int es = Es > 0 ? Es : (int)sizeof(int);
                int stride = es;
                int d;
                for (d = 1; d < (int)Fr->LocalNDims[Base->LocalSlot]; d++)
                {
                    int dim = Fr->LocalDims[Base->LocalSlot][d];
                    if (dim > 0)
                        stride *= dim;
                }
                ElemBase = TypeArray;
                ElemSize = stride;
            }
            else if (Fr->LocalArraySize[Base->LocalSlot] > 0
                     && (Fr->LocalIsFptr[Base->LocalSlot]
                         || Fr->LocalElemIsPtr[Base->LocalSlot]))
            {
                /* local T *a[N] or int (*fp[N])(): element is a pointer word.
                 * Do NOT use Es==sizeof(void*) — on ILP32 sizeof(int)==sizeof(void*)
                 * and that misclassified int a[N] as pointer-array (printf XXX). */
                ElemBase = TypePointer;
                ElemSize = (int)sizeof(void *);
            }
            else if (LFT != NULL && LFT->Sizeof > 0 && LFT->Base != TypePointer
                     && LFT->Base != TypeArray && LFT->Base != TypeFunction)
            {
                /* char *p / int *p / unsigned *p: pointee is LFT */
                ElemBase = LFT->Base;
                ElemSize = LFT->Sizeof;
            }
            else if (IrFrameGetPointeeSize(Fr, Base->LocalSlot) > 0)
            {
                int ps = IrFrameGetPointeeSize(Fr, Base->LocalSlot);
                ElemSize = ps;
                ElemBase = (ps == 1) ? TypeChar : (ps == 2) ? TypeShort
                         : (ps == 8) ? TypeLongLong : TypeInt;
            }
            else if (Es == 1) { ElemBase = TypeChar; ElemSize = 1; }
            else if (Es == 2) { ElemBase = TypeShort; ElemSize = 2; }
            else if (Es == 8 && (LFT == NULL || LFT->Base == TypeLongLong
                                 || LFT->Base == TypeUnsignedLongLong))
            { ElemBase = TypeLongLong; ElemSize = 8; }
            else { ElemBase = TypeInt; ElemSize = (Es > 0 && Es != (int)sizeof(void *)) ? Es : 4; }
        }
        else if (Typ != NULL)
        {
            struct ValueType *Ft = Typ->FromType;
            if (Ft != NULL)
            {
                ElemBase = Ft->Base;
                if (Ft->Base == TypePointer)
                    ElemSize = (int)sizeof(void *);
                else
                    /* scalar or TypeArray row: use FromType Sizeof */
                    ElemSize = Ft->Sizeof > 0 ? Ft->Sizeof : 1;
            }
            else if (Typ->Base == TypePointer)
            {
                ElemBase = TypeChar;
                ElemSize = 1;
            }
        }
        else if (Base->LocalSlot >= 0 && IrCallDepth > 0
                 && Base->LocalSlot < IR_CALL_MAX_LOCALS
                 && IrCallStack[IrCallDepth - 1]->LocalIsPtr[Base->LocalSlot])
        {
            int Es = IrCallStack[IrCallDepth - 1]->LocalElemSize[Base->LocalSlot];
            if (Es == 1) { ElemBase = TypeChar; ElemSize = 1; }
            else if (Es == 2) { ElemBase = TypeShort; ElemSize = 2; }
            else if (Es == 8) { ElemBase = TypeLongLong; ElemSize = 8; }
            else { ElemBase = TypeInt; ElemSize = (Es > 0) ? Es : 4; }
        }
    }
    else if (Base->Kind == IrExMember)
    {
        /* s.arr / p->arr / a[i].arr : address of array (or scalar member) */
        void *MAddr = NULL;
        int MBase = TypeInt;
        struct ValueType *MTyp = NULL;
        if (!IrResolveMember(Parser, Base, &MAddr, &MBase))
            return 0;
        /* Recover full member type for arrays/pointers.
         * Covers: global s.m, param p->m, and a[i].m (Index base). */
        if (Base->Left != NULL && Base->Ident != NULL)
        {
            struct ValueType *ST = NULL;
            struct Value *MV = NULL;
            if (Base->Left->Kind == IrExIdent)
            {
                /* B.2: frame first for local/param root */
                if (ST == NULL && Base->Left->ParamSlot >= 0 && IrCallDepth > 0
                    && Base->Left->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
                {
                    struct ValueType *PT = IrCallStack[IrCallDepth - 1]->ParamTyp[Base->Left->ParamSlot];
                    if (Base->Op == TokenArrow && PT != NULL)
                        ST = PT->FromType;
                    else
                        ST = PT;
                }
                if (ST == NULL && Base->Left->LocalSlot >= 0 && IrCallDepth > 0
                    && Base->Left->LocalSlot < IR_CALL_MAX_LOCALS)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    struct ValueType *LFT = Fr->LocalFromType[Base->Left->LocalSlot];
                    struct ValueType *LT = Fr->LocalTyp[Base->Left->LocalSlot];
                    if (Base->Op == TokenArrow && Fr->LocalIsPtr[Base->Left->LocalSlot])
                        ST = LFT;
                    else if (LFT != NULL && (LFT->Base == TypeStruct || LFT->Base == TypeUnion))
                        ST = LFT;
                    else if (LT != NULL && (LT->Base == TypeStruct || LT->Base == TypeUnion))
                        ST = LT;
                    else if (Fr->LocalIsPtr[Base->Left->LocalSlot])
                        ST = LFT;
                }
                /* global only */
                if (ST == NULL && Base->Left->LocalSlot < 0 && Base->Left->ParamSlot < 0
                    && Base->Left->Ident != NULL
                    && (IrGlobalValSoft(Parser, Base->Left) != NULL))
                {
                    struct Value *Root = NULL;
                    Root = IrGlobalVal(Parser, Base->Left); /* global only via CachedVal */
                    if (Root != NULL && Root->Typ != NULL)
                    {
                        ST = Root->Typ;
                        if (Base->Op == TokenArrow && ST->Base == TypePointer)
                            ST = ST->FromType;
                    }
                }
            }
            else if (Base->Left->Kind == IrExUnary && Base->Left->Op == TokenAsterisk
                     && Base->Left->Left != NULL && Base->Left->Left->Kind == IrExIdent)
            {
                /* (*p).m — p is T* */
                IrExpr *P = Base->Left->Left;
                if (P->LocalSlot >= 0 && IrCallDepth > 0
                    && P->LocalSlot < IR_CALL_MAX_LOCALS
                    && IrCallStack[IrCallDepth - 1]->LocalFromType[P->LocalSlot] != NULL)
                    ST = IrCallStack[IrCallDepth - 1]->LocalFromType[P->LocalSlot];
                else if (P->ParamSlot >= 0 && IrCallDepth > 0
                         && P->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
                {
                    struct ValueType *PT = IrCallStack[IrCallDepth - 1]->ParamTyp[P->ParamSlot];
                    ST = (PT && PT->Base == TypePointer) ? PT->FromType : PT;
                }
                else if (P->LocalSlot < 0 && P->ParamSlot < 0
                         && P->Ident != NULL && (IrGlobalValSoft(Parser, P) != NULL))
                {
                    struct Value *Root = NULL;
                    Root = IrGlobalVal(Parser, P); /* global only via CachedVal */
                    if (Root != NULL && Root->Typ != NULL && Root->Typ->Base == TypePointer)
                        ST = Root->Typ->FromType;
                }
                if (ST != NULL && ST->Base == TypePointer && ST->FromType != NULL)
                    ST = ST->FromType;
            }
            else if (Base->Left->Kind == IrExIndex && Base->Left->Left != NULL)
            {
                /* a[i].m — element type of a is the struct */
                IrExpr *IdxBase = Base->Left->Left;
                if (IdxBase->Kind == IrExIdent && IdxBase->LocalSlot >= 0
                    && IrCallDepth > 0 && IdxBase->LocalSlot < IR_CALL_MAX_LOCALS)
                {
                    IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                    struct ValueType *LFT = Fr->LocalFromType[IdxBase->LocalSlot];
                    if (LFT != NULL && (LFT->Base == TypeStruct || LFT->Base == TypeUnion))
                        ST = LFT;
                    else if (LFT != NULL && LFT->Base == TypeArray && LFT->FromType != NULL)
                        ST = LFT->FromType;
                    else if (Fr->LocalElemSize[IdxBase->LocalSlot] > 0 && LFT != NULL)
                        ST = LFT;
                }
                else if (IdxBase->Kind == IrExIdent && IdxBase->ParamSlot >= 0
                         && IrCallDepth > 0
                         && IdxBase->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
                {
                    struct ValueType *PT = IrCallStack[IrCallDepth - 1]->ParamTyp[IdxBase->ParamSlot];
                    if (PT != NULL && PT->FromType != NULL)
                    {
                        /* param is T* or T[] — element is T */
                        ST = PT->FromType;
                        if (ST->Base == TypeArray && ST->FromType != NULL)
                            ST = ST->FromType;
                    }
                }
                else if (IdxBase->Kind == IrExIdent && IdxBase->Ident != NULL
                         && IdxBase->LocalSlot < 0 && IdxBase->ParamSlot < 0
                         && (IrGlobalValSoft(Parser, IdxBase) != NULL))
                {
                    struct Value *Root = NULL;
                    Root = IrGlobalVal(Parser, IdxBase); /* global only via CachedVal */
                    if (Root != NULL && Root->Typ != NULL)
                    {
                        struct ValueType *T = Root->Typ;
                        if (T->Base == TypePointer && T->FromType != NULL)
                            T = T->FromType;
                        if (T->Base == TypeArray && T->FromType != NULL)
                            ST = T->FromType;
                    }
                }
            }
            /* Any other s.m (call result, nested member, ...): the static
             * aggregate type of s names the member. */
            if (ST == NULL && Base->Op == TokenDot
                && Base->Left->CanonicalType != NULL
                && IrTypeIsCopyableAggregate(Base->Left->CanonicalType))
                ST = Base->Left->CanonicalType;
            if (ST != NULL && ST->Members != NULL
                && TableGet(ST->Members, Base->Ident, &MV, NULL, NULL, NULL)
                && MV != NULL)
                MTyp = MV->Typ;
        }
        if (MTyp != NULL && MTyp->Base == TypeArray && MTyp->FromType != NULL)
        {
            Addr = MAddr;
            ElemBase = MTyp->FromType->Base;
            ElemSize = MTyp->FromType->Sizeof > 0 ? MTyp->FromType->Sizeof : 1;
        }
        else if (MTyp != NULL && MTyp->Base == TypePointer && MTyp->FromType != NULL)
        {
            /* pointer member: load the pointer value */
            Addr = *(void **)MAddr;
            ElemBase = MTyp->FromType->Base;
            ElemSize = MTyp->FromType->Sizeof > 0 ? MTyp->FromType->Sizeof : 1;
        }
        else
        {
            Addr = MAddr;
            ElemBase = MBase;
            ElemSize = 1;
            if (MBase == TypeInt || MBase == TypeUnsignedInt) ElemSize = (int)sizeof(int);
            else if (MBase == TypeShort || MBase == TypeUnsignedShort) ElemSize = 2;
            else if (MBase == TypeLong || MBase == TypeUnsignedLong) ElemSize = (int)sizeof(long);
            else if (MBase == TypeLongLong) ElemSize = (int)sizeof(long long);
            else if (MBase == TypePointer) ElemSize = (int)sizeof(void *);
            /* TypeArray without MTyp: still usable as base for a[i] if MAddr set */
            else if (MBase == TypeArray)
            {
                Addr = MAddr;
                ElemBase = TypeInt;
                ElemSize = (int)sizeof(int);
            }
        }
        if (Addr == NULL)
            return 0;
        *AddrOut = Addr;
        *ElemBaseOut = ElemBase;
        *ElemSizeOut = ElemSize;
        return 1;
    }
    else if (Base->Kind == IrExIndex)
    {
        /* (p[i]) as base of further index: evaluate to address of element */
        void *BaseAddr;
        int Eb, Es;
        long long Idx;
        if (!IrResolvePtr(Parser, Base->Left, &BaseAddr, &Eb, &Es))
            return 0;
        Idx = IrEvalExpr(Parser, Base->Right);
        IrCheckIndexBounds(Parser, Base, Idx);
        Addr = (char *)BaseAddr + (ptrdiff_t)Idx * Es;
        /* Element type of p[i] when p is T*: T; if T is pointer, keep pointer */
        if (Eb == TypePointer)
        {
            ElemBase = TypePointer;
            ElemSize = (int)sizeof(void *);
            /* Addr points at a pointer value; for *p[i] we'd deref later.
             * For p[i][j], Addr is the char* value stored at p[i] — need load. */
            Addr = *(void **)Addr;
            if (Addr == NULL)
                return 0;
            /* Declared pointee (T *a[N]; a[i][j]) when the compiler proved
             * it; otherwise the historical char** (argv[i][j]) default. */
            if (Base->PointeeType != NULL && Base->PointeeType->Sizeof > 0)
            {
                ElemBase = Base->PointeeType->Base;
                ElemSize = (int)Base->PointeeType->Sizeof;
            }
            else
            {
                ElemBase = TypeChar;
                ElemSize = 1;
            }
        }
        else if (Eb == TypeArray)
        {
            /* Multi-dim a[i][j][k]: after each index, next stride = product of
             * remaining dims * elem_size. Prefer IR-frame dims for Body-IR locals. */
            struct ValueType *T = NULL;
            IrExpr *Root = Base;
            int Depth = 0;
            while (Root != NULL && Root->Kind == IrExIndex)
            {
                Depth++;
                Root = Root->Left;
            }
            /* Body-IR local multi-dim */
            if (Root != NULL && Root->Kind == IrExIdent
                && Root->LocalSlot >= 0 && IrCallDepth > 0
                && Root->LocalSlot < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                int nd = (int)Fr->LocalNDims[Root->LocalSlot];
                int es = Fr->LocalElemSize[Root->LocalSlot];
                if (es <= 0) es = (int)sizeof(int);
                if (nd >= 2 && Depth < nd)
                {
                    int stride = es;
                    int d;
                    for (d = Depth + 1; d < nd; d++)
                    {
                        int dim = Fr->LocalDims[Root->LocalSlot][d];
                        if (dim > 0)
                            stride *= dim;
                    }
                    if (Depth + 1 < nd)
                    {
                        ElemBase = TypeArray;
                        ElemSize = stride;
                    }
                    else if (Fr->LocalElemIsPtr[Root->LocalSlot]
                             || Fr->LocalIsFptr[Root->LocalSlot])
                    {
                        ElemBase = TypePointer;
                        ElemSize = (int)sizeof(void *);
                    }
                    else
                    {
                        ElemBase = IrLocalArrayLeafBase(Fr, Root->LocalSlot, es);
                        ElemSize = es;
                    }
                }
                else if (nd >= 2 && Depth >= nd - 1)
                {
                    if (Fr->LocalElemIsPtr[Root->LocalSlot]
                        || Fr->LocalIsFptr[Root->LocalSlot])
                    {
                        ElemBase = TypePointer;
                        ElemSize = (int)sizeof(void *);
                    }
                    else
                    {
                        ElemBase = IrLocalArrayLeafBase(Fr, Root->LocalSlot, es);
                        ElemSize = es;
                    }
                }
                else
                {
                    if (Fr->LocalElemIsPtr[Root->LocalSlot]
                        || Fr->LocalIsFptr[Root->LocalSlot])
                    {
                        ElemBase = TypePointer;
                        ElemSize = (int)sizeof(void *);
                    }
                    else
                    {
                        ElemBase = IrLocalArrayLeafBase(Fr, Root->LocalSlot, es);
                        ElemSize = es;
                    }
                }
            }
            else
            {
                if (Root != NULL && Root->Kind == IrExIdent && Root->Ident != NULL
                    && (IrGlobalValSoft(Parser, Root) != NULL)
                    )
                {
                    struct Value *RV = NULL;
                    RV = IrGlobalVal(Parser, Root); /* global only via CachedVal */
                    if (RV != NULL && RV->Typ != NULL)
                        T = RV->Typ;
                }
                {
                    int d;
                    for (d = 0; d < Depth && T != NULL && T->Base == TypeArray; d++)
                        T = T->FromType;
                }
                if (T != NULL && T->Base == TypeArray && T->FromType != NULL)
                {
                    ElemBase = T->FromType->Base;
                    ElemSize = T->FromType->Sizeof > 0 ? T->FromType->Sizeof : 1;
                }
                else if (T != NULL)
                {
                    ElemBase = T->Base;
                    ElemSize = T->Sizeof > 0 ? T->Sizeof : (int)sizeof(int);
                }
                else
                {
                    ElemBase = TypeInt;
                    ElemSize = (int)sizeof(int);
                }
            }
        }
        else
        {
            ElemBase = Eb;
            ElemSize = Es;
        }
    }
    else
    {
        /* p[i] where p is T* / array-of-T*: Base evaluates to a pointer value.
         * Default char* is wrong for int *a[N]; a[i][0] — recover pointee size. */
        Addr = (void *)(intptr_t)IrEvalExpr(Parser, Base);
        ElemBase = TypeChar;
        ElemSize = 1;
        if (Base != NULL && Base->Kind == IrExIndex && Base->Left != NULL
            && Base->Left->Kind == IrExIdent && Base->Left->LocalSlot >= 0
            && IrCallDepth > 0 && Base->Left->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            int ls = Base->Left->LocalSlot;
            if (Fr->LocalElemIsPtr[ls] || Fr->LocalIsFptr[ls])
            {
                /* array of pointers: element is pointer to int (default) */
                ElemBase = TypeInt;
                ElemSize = (int)sizeof(int);
            }
            else if (Fr->LocalElemSize[ls] > 0)
            {
                ElemSize = Fr->LocalElemSize[ls];
                ElemBase = (ElemSize == 1) ? TypeChar
                         : (ElemSize == 2) ? TypeShort
                         : (ElemSize == 8) ? TypeLongLong
                         : TypeInt;
            }
        }
        else if (Base != NULL && Base->Kind == IrExUnary
                 && (Base->Op == TokenIncrement || Base->Op == TokenDecrement)
                 && Base->Left != NULL && IrPtrElemSizeOf(Base->Left) > 0)
        {
            /* *p++ / *--p: the value (evaluated once above) is the operand's
             * pointer, so the element is the operand's pointee, not char. */
            ElemSize = IrPtrElemSizeOf(Base->Left);
            ElemBase = IrBaseFromElemSize(ElemSize, IrPointeeUnsigned(Base->Left));
        }
        else if (Base != NULL && Base->Kind == IrExIdent && Base->LocalSlot >= 0
                 && IrCallDepth > 0 && Base->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            int es = Fr->LocalElemSize[Base->LocalSlot];
            if (Fr->LocalIsPtr[Base->LocalSlot] && es > 0)
            {
                ElemSize = es;
                ElemBase = (es == 1) ? TypeChar : (es == 2) ? TypeShort
                         : (es == 8) ? TypeLongLong : TypeInt;
            }
        }
    }

    if (Addr == NULL)
        return 0;
    *AddrOut = Addr;
    *ElemBaseOut = ElemBase;
    *ElemSizeOut = ElemSize;
    return 1;
}

/* ---- Aggregate (struct/union) object designators ----
 * The IR represents a struct/union value by the address of its storage: an
 * owned local holds its blob address in LocalVal, a by-value parameter holds
 * the address of its copy, and calls/va_arg/assignments yield the address of
 * their result. An lvalue designator of aggregate type (s.m, a[i], *p, a
 * global) must therefore evaluate to the address of the designated object,
 * never to a scalar load of its first bytes. Consumers (by-value arguments,
 * variadic payloads, assignment, initialization, return) copy
 * sizeof(CanonicalType) bytes from that address. */

/* A struct/union with no members is an opaque builtin handle (va_list, FILE):
 * its IR value is the handle itself, not a copyable object representation. */
int IrTypeIsCopyableAggregate(struct ValueType *T)
{
    int i;
    if (T == NULL || (T->Base != TypeStruct && T->Base != TypeUnion)
        || T->Sizeof <= 0 || T->Members == NULL || T->Members->HashTable == NULL)
        return 0;
    for (i = 0; i < T->Members->Size; ++i)
        if (T->Members->HashTable[i] != NULL)
            return 1;
    return 0;
}

/* E is an lvalue of copyable aggregate type whose generic evaluation is not
 * already its object address. Frame-slot identifiers (locals, parameters)
 * already evaluate to their object address and keep their path. */
int IrExprDesignatesAggregateObject(IrExpr *E)
{
    if (E == NULL || E->CanonicalType == NULL || !(E->CanonicalFlags & IR_CF_VALID))
        return 0;
    switch (E->Kind)
    {
        case IrExMember:
        case IrExIndex:
            break;
        case IrExUnary:
            if (E->Op != TokenAsterisk) return 0;
            break;
        case IrExIdent:
            if (E->LocalSlot >= 0 || E->ParamSlot >= 0) return 0;
            break;
        default:
            return 0;
    }
    return IrTypeIsCopyableAggregate(E->CanonicalType);
}

/* Address of the object designated by an aggregate lvalue. Every operand is
 * evaluated exactly once (a[i++], *p++). */
int IrAggregateObjectAddr(struct ParseState *Parser, IrExpr *E, void **AddrOut)
{
    void *Addr = NULL;
    if (E == NULL || AddrOut == NULL)
        return 0;
    switch (E->Kind)
    {
        case IrExIdent:
        {
            long long PV = 0;
            struct Value *Var;
            if (E->LocalSlot >= 0 || E->ParamSlot >= 0 || IrLookupParam(E->Ident, &PV))
                Addr = (void *)(intptr_t)IrEvalExpr(Parser, E);
            else
            {
                Var = IrGlobalVal(Parser, E);
                if (Var == NULL || Var->Val == NULL)
                    return 0;
                Addr = (void *)Var->Val; /* struct/union payload storage */
            }
            break;
        }
        case IrExMember:
        {
            int MBase = TypeInt;
            if (!IrResolveMember(Parser, E, &Addr, &MBase))
                return 0;
            break;
        }
        case IrExIndex:
        {
            IrExpr *PtrE = E->Left, *IdxE = E->Right;
            void *Base = NULL;
            int Eb = TypeInt, Es = 0;
            long long Idx;
            /* C permits i[a]: the pointer operand is the one of pointer type. */
            if (PtrE != NULL && IdxE != NULL
                && !(PtrE->CanonicalFlags & IR_CF_PTR) && (IdxE->CanonicalFlags & IR_CF_PTR))
            {
                IrExpr *T = PtrE; PtrE = IdxE; IdxE = T;
            }
            if (PtrE == NULL || IdxE == NULL
                || !IrResolvePtr(Parser, PtrE, &Base, &Eb, &Es) || Base == NULL)
                return 0;
            Idx = IrEvalExpr(Parser, IdxE);
            if (PtrE == E->Left)
                IrCheckIndexBounds(Parser, E, Idx);
            Addr = (char *)Base + (ptrdiff_t)Idx * (ptrdiff_t)E->CanonicalType->Sizeof;
            break;
        }
        case IrExUnary:
            if (E->Op != TokenAsterisk || E->Left == NULL)
                return 0;
            Addr = (void *)(intptr_t)IrEvalExpr(Parser, E->Left);
            if (Addr == NULL)
                ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
            break;
        default:
            return 0;
    }
    if (Addr == NULL)
        return 0;
    *AddrOut = Addr;
    return 1;
}

int IrMemberPtrElemSize(struct ParseState *Parser, IrExpr *E)
{
    struct ValueType *MTyp = NULL;
    struct Value *MV = NULL;
    struct Value *Root = NULL;
    struct ValueType *ST = NULL;
    if (E == NULL || E->Kind != IrExMember || E->Ident == NULL || E->Left == NULL)
        return 0;
    /* B.2.2: frame first for local/param root — no VarGet */
    if (MTyp == NULL && E->Left->Kind == IrExIdent
        && E->Left->ParamSlot >= 0 && IrCallDepth > 0)
    {
        IrCallFrame *F = IrCallStack[IrCallDepth - 1];
        struct ValueType *PT = F->ParamTyp[E->Left->ParamSlot];
        ST = (E->Op == TokenArrow && PT) ? PT->FromType : PT;
        if (ST && ST->Members
            && TableGet(ST->Members, E->Ident, &MV, NULL, NULL, NULL)
            && MV && MV->Typ)
            MTyp = MV->Typ;
    }
    if (MTyp == NULL && E->Left->Kind == IrExIdent
        && E->Left->LocalSlot >= 0 && IrCallDepth > 0
        && E->Left->LocalSlot < IR_CALL_MAX_LOCALS)
    {
        IrCallFrame *F = IrCallStack[IrCallDepth - 1];
        /* Prefer LocalTyp (full type) then LocalFromType (pointee/elem) */
        ST = F->LocalTyp[E->Left->LocalSlot];
        if (ST == NULL)
            ST = F->LocalFromType[E->Left->LocalSlot];
        if (E->Op == TokenArrow && ST && ST->Base == TypePointer)
            ST = ST->FromType;
        if (ST && ST->Members
            && TableGet(ST->Members, E->Ident, &MV, NULL, NULL, NULL)
            && MV && MV->Typ)
            MTyp = MV->Typ;
    }
    /* global root only */
    if (MTyp == NULL && E->Left->Kind == IrExIdent && E->Left->Ident != NULL
        && E->Left->LocalSlot < 0 && E->Left->ParamSlot < 0
        && (IrGlobalValSoft(Parser, E->Left) != NULL))
    {
        Root = IrGlobalVal(Parser, E->Left); /* global only via CachedVal */
        if (Root && Root->Typ)
        {
            ST = Root->Typ;
            if (E->Op == TokenArrow && ST->Base == TypePointer)
                ST = ST->FromType;
            if (ST && ST->Members
                && TableGet(ST->Members, E->Ident, &MV, NULL, NULL, NULL)
                && MV && MV->Typ)
                MTyp = MV->Typ;
        }
    }
    /* Any other s.m: the static aggregate type of s (e.g. a call result or
     * a nested member) names the member directly. */
    if (MTyp == NULL && E->Op == TokenDot && E->Left->CanonicalType != NULL
        && IrTypeIsCopyableAggregate(E->Left->CanonicalType)
        && TableGet(E->Left->CanonicalType->Members, E->Ident, &MV,
                    NULL, NULL, NULL)
        && MV && MV->Typ)
        MTyp = MV->Typ;
    if (MTyp != NULL && MTyp->Base == TypePointer && MTyp->FromType != NULL
        && MTyp->FromType->Sizeof > 0)
        return MTyp->FromType->Sizeof;
    if (MTyp != NULL && MTyp->Base == TypeArray && MTyp->FromType != NULL
        && MTyp->FromType->Sizeof > 0)
        return MTyp->FromType->Sizeof;
    return 0;
}

/* Recover the declared pointee of a pointer-valued expression without
 * evaluating it. Pointer arithmetic and ++/-- preserve the root pointer type;
 * this is shared by arrow and (*p).member resolution so local struct arrays do
 * not depend on a global-type scan. */
static struct ValueType *IrMemberPointerPointeeType(struct ParseState *Parser,
                                                    IrExpr *E, int Depth)
{
    struct ValueType *T = NULL;
    if (E == NULL || Depth > 32)
        return NULL;
    if (E->Kind == IrExIdent)
    {
        if (E->LocalSlot >= 0 && IrCallDepth > 0
            && E->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
            if (Fr->LocalIsPtr[E->LocalSlot])
                T = Fr->LocalFromType[E->LocalSlot];
        }
        else if (E->ParamSlot >= 0 && IrCallDepth > 0
                 && E->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
        {
            T = IrCallStack[IrCallDepth - 1]->ParamTyp[E->ParamSlot];
            if (T != NULL && T->Base == TypePointer)
                T = T->FromType;
        }
        else if (E->Ident != NULL && E->LocalSlot < 0 && E->ParamSlot < 0)
        {
            struct Value *GV = IrGlobalValSoft(Parser, E);
            if (GV != NULL && GV->Typ != NULL && GV->Typ->Base == TypePointer)
                T = GV->Typ->FromType;
        }
        if (T != NULL && T->Base == TypePointer)
            T = T->FromType;
        return T;
    }
    if (E->Kind == IrExBinOp && (E->Op == TokenPlus || E->Op == TokenMinus))
    {
        T = IrMemberPointerPointeeType(Parser, E->Left, Depth + 1);
        if (T == NULL && E->Op == TokenPlus)
            T = IrMemberPointerPointeeType(Parser, E->Right, Depth + 1);
        return T;
    }
    if (E->Kind == IrExUnary
        && (E->Op == TokenIncrement || E->Op == TokenDecrement))
        return IrMemberPointerPointeeType(Parser, E->Left, Depth + 1);
    if (E->Kind == IrExUnary && E->Op == TokenAsterisk && E->Left != NULL)
    {
        T = IrMemberPointerPointeeType(Parser, E->Left, Depth + 1);
        if (T != NULL && T->Base == TypePointer)
            return T->FromType;
        return NULL;
    }
    if (E->Kind == IrExCall && E->Ident != NULL)
    {
        struct Value *FV = IrGlobalValSoft(Parser, E);
        if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
            && FV->Val != NULL && FV->Val->FuncDef.ReturnType != NULL)
        {
            T = FV->Val->FuncDef.ReturnType;
            if (T->Base == TypePointer)
                return T->FromType;
        }
    }
    return NULL;
}

int IrResolveMember(struct ParseState *Parser, IrExpr *E,
                           void **AddrOut, int *ElemBaseOut)
{
    void *BaseAddr = NULL;
    struct ValueType *StructType = NULL;
    struct Value *MemberValue = NULL;
    struct Value *RootVar = NULL;
    char *Field;

    if (E == NULL || E->Kind != IrExMember || E->Ident == NULL)
        return 0;
    Field = E->Ident;

    /* s.m where s is an aggregate rvalue (call result, assignment, ?:,
     * va_arg): the IR value of such an expression is the address of its
     * storage, so the member lives at that address plus its offset. */
    if (E->Op == TokenDot && E->Left != NULL
        && (E->Left->Kind == IrExCall || E->Left->Kind == IrExAssign
            || E->Left->Kind == IrExTernary || E->Left->Kind == IrExVaArg)
        && E->Left->CanonicalType != NULL
        && IrTypeIsCopyableAggregate(E->Left->CanonicalType))
    {
        struct Value *RvMember = NULL;
        int member_off = 0;
        void *RvBase = (void *)(intptr_t)IrEvalExpr(Parser, E->Left);
        if (RvBase == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        if (!TableGet(E->Left->CanonicalType->Members, Field, &RvMember,
                      NULL, NULL, NULL)
            || RvMember == NULL || RvMember->Val == NULL
            || !IrResolveMemberOffsetValue(RvMember, &member_off))
            return 0;
        *AddrOut = (char *)RvBase + member_off;
        *ElemBaseOut = RvMember->Typ ? RvMember->Typ->Base : TypeInt;
        E->BitOffset = 0;
        E->BitWidth = 0;
        if (RvMember->BitWidth > 0)
        {
            E->BitOffset = RvMember->BitOffset;
            E->BitWidth = RvMember->BitWidth;
            E->BitSigned = (RvMember->Typ
                && (RvMember->Typ->Base == TypeInt
                    || RvMember->Typ->Base == TypeShort
                    || RvMember->Typ->Base == TypeChar
                    || RvMember->Typ->Base == TypeLong)) ? 1 : 0;
        }
        return 1;
    }

    /* ((T*)p)->m / ((T*)p).m — pointer cast base carrying pointee type in CachedAddr */
    if (E->Left != NULL && E->Left->Kind == IrExUnary && E->Left->Op == TokenCast
        && E->Left->ConstVal > 0
        && (E->Op == TokenArrow || E->Op == TokenDot))
    {
        void *PtrVal = (void *)(intptr_t)IrEvalExpr(Parser, E->Left->Left);
        struct ValueType *ST = (struct ValueType *)E->Left->CachedAddr;
        struct Value *MemberValue2 = NULL;
        if (PtrVal == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        /* CachedAddr is the pointee struct/union type when stamped at compile */
        if (ST != NULL && ST->Base == TypePointer && ST->FromType != NULL)
            ST = ST->FromType;
        if (ST != NULL && (ST->Base == TypeStruct || ST->Base == TypeUnion)
            && ST->Members != NULL
            && TableGet(ST->Members, Field, &MemberValue2, NULL, NULL, NULL)
            && MemberValue2 != NULL && MemberValue2->Val != NULL)
        {
            int member_off = 0;
            if (!IrResolveMemberOffsetValue(MemberValue2, &member_off))
                return 0;
            *AddrOut = (char *)PtrVal + member_off;
            *ElemBaseOut = MemberValue2->Typ ? MemberValue2->Typ->Base : TypeInt;
            if (MemberValue2->Typ && MemberValue2->Typ->Base == TypeArray)
                *ElemBaseOut = TypeArray;
            E->BitOffset = 0;
            E->BitWidth = 0;
            if (MemberValue2->BitWidth > 0)
            {
                E->BitOffset = MemberValue2->BitOffset;
                E->BitWidth = MemberValue2->BitWidth;
                E->BitSigned = (MemberValue2->Typ
                    && (MemberValue2->Typ->Base == TypeInt
                        || MemberValue2->Typ->Base == TypeShort
                        || MemberValue2->Typ->Base == TypeChar
                        || MemberValue2->Typ->Base == TypeLong)) ? 1 : 0;
            }
            return 1;
        }
        /* Fallback: scan global struct types for a member named Field */
        {
            int hi;
            for (hi = 0; hi < Parser->pc->GlobalTable.Size; hi++)
            {
                struct TableEntry *Ent;
                for (Ent = Parser->pc->GlobalTable.HashTable[hi]; Ent != NULL; Ent = Ent->Next)
                {
                    struct Value *TV = Ent->p.v.Val;
                    struct ValueType *Cand;
                    if (TV == NULL || TV->Typ == NULL) continue;
                    Cand = TV->Typ;
                    if (Cand->Base == Type_Type && TV->Val != NULL && TV->Val->Typ != NULL)
                        Cand = TV->Val->Typ; /* typedef */
                    if (Cand == NULL
                        || (Cand->Base != TypeStruct && Cand->Base != TypeUnion)
                        || Cand->Members == NULL)
                        continue;
                    if (TableGet(Cand->Members, Field, &MemberValue2, NULL, NULL, NULL)
                        && MemberValue2 != NULL && MemberValue2->Val != NULL)
                    {
                        int member_off = 0;
                        if (!IrResolveMemberOffsetValue(MemberValue2, &member_off))
                            continue;
                        *AddrOut = (char *)PtrVal + member_off;
                        *ElemBaseOut = MemberValue2->Typ ? MemberValue2->Typ->Base : TypeInt;
                        if (MemberValue2->Typ && MemberValue2->Typ->Base == TypeArray)
                            *ElemBaseOut = TypeArray;
                        return 1;
                    }
                }
            }
        }
        return 0;
    }

    /* (*p).m / (*pp)->m — Unary deref base */
    if (E->Left != NULL && E->Left->Kind == IrExUnary && E->Left->Op == TokenAsterisk
        && (E->Op == TokenDot || E->Op == TokenArrow))
    {
        void *PtrVal;
        struct ValueType *ST = NULL;
        IrExpr *P = E->Left->Left;
        if (E->Op == TokenDot)
        {
            /* (*p).m — p points to struct */
            PtrVal = (void *)(intptr_t)IrEvalExpr(Parser, E->Left->Left);
        }
        else
        {
            /* (*pp)->m — *pp yields struct* ; load that pointer */
            void *Mid = (void *)(intptr_t)IrEvalExpr(Parser, E->Left);
            PtrVal = Mid;
        }
        if (PtrVal == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        if (P != NULL && P->Kind == IrExIdent && IrCallDepth > 0)
        {
            IrCallFrame *F = IrCallStack[IrCallDepth - 1];
            if (P->LocalSlot >= 0 && P->LocalSlot < IR_CALL_MAX_LOCALS)
                ST = F->LocalFromType[P->LocalSlot];
            else if (P->ParamSlot >= 0 && P->ParamSlot < F->NumParams)
            {
                struct ValueType *PT = F->ParamTyp[P->ParamSlot];
                ST = (PT && PT->Base == TypePointer) ? PT->FromType : PT;
            }
        }
        /* global only — frame path above already set ST for local/param */
        if (ST == NULL && P != NULL && P->Kind == IrExIdent
            && P->LocalSlot < 0 && P->ParamSlot < 0
            && P->Ident != NULL && (IrGlobalValSoft(Parser, P) != NULL))
        {
            struct Value *PV = NULL;
            PV = IrGlobalValSoft(Parser, P);
            if (PV && PV->Typ && PV->Typ->Base == TypePointer && PV->Typ->FromType)
            {
                ST = PV->Typ->FromType;
                if (ST->Base == TypePointer && ST->FromType)
                    ST = ST->FromType; /* ** */
            }
        }
        /* (*f()).m — f returns struct* */
        if (ST == NULL && P != NULL && P->Kind == IrExCall && P->Ident != NULL
            && (IrGlobalValSoft(Parser, P) != NULL))
        {
            struct Value *FV = NULL;
            FV = IrGlobalVal(Parser, P); /* global/func via CachedVal */
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                && FV->Val != NULL && FV->Val->FuncDef.ReturnType != NULL)
            {
                struct ValueType *RT = FV->Val->FuncDef.ReturnType;
                if (RT->Base == TypePointer && RT->FromType != NULL)
                    ST = RT->FromType;
                else if (RT->Base == TypeStruct || RT->Base == TypeUnion)
                    ST = RT;
            }
        }
        /* (*(T*)p).m — cast supplies pointee type via CachedAddr */
        if (ST == NULL && P != NULL && P->Kind == IrExUnary && P->Op == TokenCast
            && P->ConstVal > 0 && P->CachedAddr != NULL)
        {
            ST = (struct ValueType *)P->CachedAddr;
            if (ST->Base == TypePointer && ST->FromType != NULL)
                ST = ST->FromType;
        }
        /* Pointer arithmetic such as (*(p + 1)).field has no bare-identifier
         * root at this level. Recover the pointee type from the pointer AST. */
        if (ST == NULL && P != NULL)
            ST = IrMemberPointerPointeeType(Parser, P, 0);
        /* for Arrow, ST is struct*; for Dot, ST is struct */
        if (E->Op == TokenArrow && ST != NULL && ST->Base == TypePointer)
            ST = ST->FromType;
        if (ST != NULL && ST->Members != NULL
            && TableGet(ST->Members, Field, &MemberValue, NULL, NULL, NULL)
            && MemberValue != NULL && MemberValue->Val != NULL)
        {
            int member_off = 0;
            if (!IrResolveMemberOffsetValue(MemberValue, &member_off))
                return 0;
            *AddrOut = (char *)PtrVal + member_off;
            *ElemBaseOut = MemberValue->Typ ? MemberValue->Typ->Base : TypeInt;
            if (MemberValue->Typ && MemberValue->Typ->Base == TypeArray)
                *ElemBaseOut = TypeArray;
            E->BitOffset = 0;
            E->BitWidth = 0;
            if (MemberValue->BitWidth > 0)
            {
                E->BitOffset = MemberValue->BitOffset;
                E->BitWidth = MemberValue->BitWidth;
            }
            return 1;
        }
        /* Fallback: search globals for struct type owning Field (array/ptr of struct) */
        if (MemberValue == NULL)
        {
            int hi;
            for (hi = 0; hi < Parser->pc->GlobalTable.Size; hi++)
            {
                struct TableEntry *Ent;
                for (Ent = Parser->pc->GlobalTable.HashTable[hi]; Ent != NULL; Ent = Ent->Next)
                {
                    struct Value *TV = Ent->p.v.Val;
                    struct ValueType *Cand;
                    if (TV == NULL || TV->Typ == NULL) continue;
                    Cand = TV->Typ;
                    if (Cand->Base == TypeArray || Cand->Base == TypePointer)
                        Cand = Cand->FromType;
                    if (Cand == NULL
                        || (Cand->Base != TypeStruct && Cand->Base != TypeUnion)
                        || Cand->Members == NULL)
                        continue;
                    if (TableGet(Cand->Members, Field, &MemberValue, NULL, NULL, NULL)
                        && MemberValue != NULL)
                    {
                        int member_off = 0;
                        if (!IrResolveMemberOffsetValue(MemberValue, &member_off))
                            continue;
                        *AddrOut = (char *)PtrVal + member_off;
                        *ElemBaseOut = MemberValue->Typ ? MemberValue->Typ->Base : TypeInt;
                        if (MemberValue->Typ && MemberValue->Typ->Base == TypeArray)
                            *ElemBaseOut = TypeArray;
                        return 1;
                    }
                }
            }
        }
        return 0;
    }

    /* (*f()).m — call returns struct pointer, Dot after deref */
    if (E->Left != NULL && E->Left->Kind == IrExUnary && E->Left->Op == TokenAsterisk
        && E->Left->Left != NULL && E->Left->Left->Kind == IrExCall
        && E->Op == TokenDot)
    {
        void *PtrVal = (void *)(intptr_t)IrEvalExpr(Parser, E->Left->Left);
        struct ValueType *ST = NULL;
        if (PtrVal == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        if (E->Left->Left->Ident != NULL && (IrGlobalValSoft(Parser, E->Left->Left) != NULL))
        {
            struct Value *FV = NULL;
            FV = IrGlobalVal(Parser, E->Left->Left); /* global/func via CachedVal */
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                && FV->Val != NULL && FV->Val->FuncDef.ReturnType != NULL)
            {
                struct ValueType *RT = FV->Val->FuncDef.ReturnType;
                if (RT->Base == TypePointer && RT->FromType != NULL)
                    ST = RT->FromType;
            }
        }
        if (ST == NULL)
        {
            /* fall back: scan globals for a struct type owning Field */
            int hi;
            for (hi = 0; hi < Parser->pc->GlobalTable.Size && ST == NULL; hi++)
            {
                struct TableEntry *Entry;
                for (Entry = Parser->pc->GlobalTable.HashTable[hi]; Entry != NULL; Entry = Entry->Next)
                {
                    struct Value *GV = Entry->p.v.Val;
                    if (GV != NULL && GV->Typ != NULL)
                    {
                        struct ValueType *Cand = GV->Typ;
                        struct Value *MV = NULL;
                        if (Cand->Base == TypeArray || Cand->Base == TypePointer)
                            Cand = Cand->FromType;
                        if (Cand != NULL
                            && (Cand->Base == TypeStruct || Cand->Base == TypeUnion)
                            && Cand->Members != NULL
                            && TableGet(Cand->Members, Field, &MV, NULL, NULL, NULL)
                            && MV != NULL)
                        { ST = Cand; break; }
                    }
                }
            }
        }
        if (ST != NULL && ST->Members != NULL
            && TableGet(ST->Members, Field, &MemberValue, NULL, NULL, NULL)
            && MemberValue != NULL)
        {
            int member_off = 0;
            if (!IrResolveMemberOffsetValue(MemberValue, &member_off))
                return 0;
            *AddrOut = (char *)PtrVal + member_off;
            *ElemBaseOut = MemberValue->Typ ? MemberValue->Typ->Base : TypeInt;
            E->BitOffset = 0; E->BitWidth = 0;
            if (MemberValue->BitWidth > 0)
            { E->BitOffset = MemberValue->BitOffset; E->BitWidth = MemberValue->BitWidth; }
            return 1;
        }
        return 0;
    }

    /* General pointer expression -> member, including (p + n)->field and
     * (p++)->field. Evaluate the pointer expression once; derive the aggregate
     * type independently from its root declaration. */
    if (E->Op == TokenArrow && E->Left != NULL
        && (E->Left->Kind == IrExBinOp
            || (E->Left->Kind == IrExUnary
                && (E->Left->Op == TokenIncrement || E->Left->Op == TokenDecrement))))
    {
        struct ValueType *ST = IrMemberPointerPointeeType(Parser, E->Left, 0);
        void *PtrVal = (void *)(intptr_t)IrEvalExpr(Parser, E->Left);
        struct Value *MV = NULL;
        int member_off = 0;
        if (PtrVal == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        if (ST != NULL && ST->Base == TypePointer)
            ST = ST->FromType;
        if (ST == NULL || (ST->Base != TypeStruct && ST->Base != TypeUnion)
            || ST->Members == NULL
            || !TableGet(ST->Members, Field, &MV, NULL, NULL, NULL)
            || MV == NULL || !IrResolveMemberOffsetValue(MV, &member_off))
            return 0;
        *AddrOut = (char *)PtrVal + member_off;
        *ElemBaseOut = MV->Typ ? MV->Typ->Base : TypeInt;
        if (MV->Typ && MV->Typ->Base == TypeArray)
            *ElemBaseOut = TypeArray;
        E->BitOffset = 0; E->BitWidth = 0;
        if (MV->BitWidth > 0) { E->BitOffset = MV->BitOffset; E->BitWidth = MV->BitWidth; }
        return 1;
    }

    /* Typed pointer expression -> member (including va_arg and function calls). */
    if (E->Op == TokenArrow && E->Left != NULL && E->Left->VaType != NULL
        && E->Left->VaType->Base == TypePointer && E->Left->VaType->FromType != NULL
        && (E->Left->VaType->FromType->Base == TypeStruct
            || E->Left->VaType->FromType->Base == TypeUnion))
    {
        struct ValueType *ST = E->Left->VaType->FromType;
        void *PtrVal = (void *)(intptr_t)IrEvalExpr(Parser, E->Left);
        struct Value *MV = NULL;
        int member_off = 0;
        if (PtrVal == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        if (ST->Members == NULL || !TableGet(ST->Members, Field, &MV, NULL, NULL, NULL)
            || MV == NULL || !IrResolveMemberOffsetValue(MV, &member_off))
            return 0;
        *AddrOut = (char *)PtrVal + member_off;
        *ElemBaseOut = MV->Typ ? MV->Typ->Base : TypeInt;
        E->BitOffset = 0; E->BitWidth = 0;
        if (MV->BitWidth > 0) { E->BitOffset = MV->BitOffset; E->BitWidth = MV->BitWidth; }
        return 1;
    }

    /* f()->m — call returns struct pointer */
    if (E->Left != NULL && E->Left->Kind == IrExCall && E->Op == TokenArrow)
    {
        void *PtrVal = (void *)(intptr_t)IrEvalExpr(Parser, E->Left);
        struct ValueType *ST = NULL;
        if (PtrVal == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        /* Best-effort: look up callee return type if Ident call */
        if (E->Left->Ident != NULL && (IrGlobalValSoft(Parser, E->Left) != NULL))
        {
            struct Value *FV = NULL;
            FV = IrGlobalValSoft(Parser, E->Left);
            if (FV != NULL && FV->Typ != NULL && FV->Typ->Base == TypeFunction
                && FV->Val != NULL && FV->Val->FuncDef.ReturnType != NULL)
            {
                struct ValueType *RT = FV->Val->FuncDef.ReturnType;
                if (RT->Base == TypePointer && RT->FromType != NULL)
                    ST = RT->FromType;
                else if (RT->Base == TypeStruct || RT->Base == TypeUnion)
                    ST = RT;
            }
        }
        if (ST != NULL && ST->Members != NULL
            && TableGet(ST->Members, Field, &MemberValue, NULL, NULL, NULL)
            && MemberValue != NULL)
        {
            int member_off = 0;
            if (!IrResolveMemberOffsetValue(MemberValue, &member_off))
                return 0;
            *AddrOut = (char *)PtrVal + member_off;
            *ElemBaseOut = MemberValue->Typ ? MemberValue->Typ->Base : TypeInt;
            if (MemberValue->Typ && MemberValue->Typ->Base == TypeArray)
                *ElemBaseOut = TypeArray;
            E->BitOffset = 0;
            E->BitWidth = 0;
            if (MemberValue->BitWidth > 0)
            {
                E->BitOffset = MemberValue->BitOffset;
                E->BitWidth = MemberValue->BitWidth;
            }
            return 1;
        }
        /* fallback: still have pointer; try Variable table global struct types — fail soft */
        return 0;
    }

    /* Hot path: param->field with cached offset (score/n->x loops) */
    if (E->BindSlot == -2 && E->Left != NULL && E->Left->Kind == IrExIdent
        && E->Op == TokenArrow && E->Left->ParamSlot >= 0 && IrCallDepth > 0
        && E->Left->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
    {
        void *BA = (void *)(intptr_t)IrCallStack[IrCallDepth - 1]->ParamVal[E->Left->ParamSlot];
        if (BA == NULL)
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        *AddrOut = (char *)BA + (int)E->ConstVal;
        *ElemBaseOut = E->I32;
        return 1;
    }

    if (E->Left != NULL && E->Left->Kind == IrExIdent)
    {
        if (E->Left->ParamSlot >= 0 && IrCallDepth > 0
            && E->Left->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
        {
            IrCallFrame *F = IrCallStack[IrCallDepth - 1];
            struct ValueType *PT = F->ParamTyp[E->Left->ParamSlot];
            if (E->Op == TokenArrow)
            {
                BaseAddr = (void *)(intptr_t)F->ParamVal[E->Left->ParamSlot];
                if (BaseAddr == NULL)
                    ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
                StructType = (PT != NULL) ? PT->FromType : NULL;
            }
            else
            {
                /* struct/union by value: ParamVal always holds address of payload
                 * (set by caller ArgVals / ParamArray path). ParamIsPtr may be 0
                 * for pure by-value formals — still use ParamVal as base. */
                StructType = PT;
                BaseAddr = (void *)(intptr_t)F->ParamVal[E->Left->ParamSlot];
            }
        }
        /* IR local struct* or struct value blob (B.2.2: frame only) */
        if ((StructType == NULL || BaseAddr == NULL)
            && E->Left->LocalSlot >= 0 && IrCallDepth > 0
            && E->Left->LocalSlot < IR_CALL_MAX_LOCALS)
        {
            IrCallFrame *F = IrCallStack[IrCallDepth - 1];
            struct ValueType *LFT = F->LocalFromType[E->Left->LocalSlot];
            struct ValueType *LT = F->LocalTyp[E->Left->LocalSlot];
            /* Owned value blob + Dot: always value access, ignore LocalIsPtr
             * (nested FnCodeCompile may have polluted isptr on caller slots). */
            if (F->LocalOwned[E->Left->LocalSlot]
                && F->LocalArraySize[E->Left->LocalSlot] <= 0
                && E->Op == TokenDot)
            {
                BaseAddr = (void *)(intptr_t)F->LocalVal[E->Left->LocalSlot];
                StructType = (LFT != NULL && (LFT->Base == TypeStruct || LFT->Base == TypeUnion))
                    ? LFT : LT;
                if (BaseAddr == NULL)
                    ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
            }
            else if (E->Op == TokenArrow && F->LocalIsPtr[E->Left->LocalSlot])
            {
                BaseAddr = (void *)(intptr_t)F->LocalVal[E->Left->LocalSlot];
                if (BaseAddr == NULL)
                    ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
                StructType = LFT;
            }
            else if (LFT != NULL
                     && (LFT->Base == TypeStruct || LFT->Base == TypeUnion))
            {
                /* struct value local: LocalVal is address of blob */
                BaseAddr = (void *)(intptr_t)F->LocalVal[E->Left->LocalSlot];
                StructType = LFT;
            }
            else if (LT != NULL
                     && (LT->Base == TypeStruct || LT->Base == TypeUnion))
            {
                BaseAddr = (void *)(intptr_t)F->LocalVal[E->Left->LocalSlot];
                StructType = LT;
            }
        }
        /* global / non-slot only */
        if ((StructType == NULL || BaseAddr == NULL)
            && E->Left->LocalSlot < 0 && E->Left->ParamSlot < 0)
        {
            if (!IrVarTableDefined(Parser->pc, E->Left->Ident))
                return 0;
            RootVar = IrGlobalValSoft(Parser, E->Left); /* soft: already gated */
            if (RootVar == NULL)
                return 0;
            if (E->Op == TokenArrow)
            {
                if (RootVar->Typ->Base != TypePointer)
                    ProgramFail(Parser, "can't use '->' on non-pointer");
                BaseAddr = RootVar->Val->Pointer;
                if (BaseAddr == NULL)
                    ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
                StructType = RootVar->Typ->FromType;
            }
            else
            {
                if (RootVar->Typ->Base != TypeStruct && RootVar->Typ->Base != TypeUnion)
                    ProgramFail(Parser, "can't use '.' on non-struct");
                BaseAddr = (void *)RootVar->Val;
                StructType = RootVar->Typ;
            }
        }
    }
    else if (E->Left != NULL && E->Left->Kind == IrExIndex)
    {
        void *ElemBase;
        int Eb, Es;
        long long Idx;
        IrExpr *IdxBase = E->Left->Left;
        IrExpr *RootIdxBase = IdxBase;
        while (RootIdxBase != NULL && RootIdxBase->Kind == IrExIndex)
            RootIdxBase = RootIdxBase->Left;
        if (!IrResolvePtr(Parser, IdxBase, &ElemBase, &Eb, &Es))
            ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        Idx = IrEvalExpr(Parser, E->Left->Right);
        IrCheckIndexBounds(Parser, E->Left, Idx);
        BaseAddr = (char *)ElemBase + (ptrdiff_t)Idx * Es;
        /* Recover element type from the root array even when the lvalue has
         * multiple subscripts: grid[i][j].field has an Index as IdxBase,
         * but the element's struct type belongs to the root local slot. */
        if (RootIdxBase != NULL && RootIdxBase->Kind == IrExIdent)
        {
            if (RootIdxBase->ParamSlot >= 0 && IrCallDepth > 0
                && RootIdxBase->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
            {
                struct ValueType *PT = IrCallStack[IrCallDepth - 1]->ParamTyp[RootIdxBase->ParamSlot];
                if (PT != NULL && PT->FromType != NULL)
                {
                    /* Node *n; n[i].val → element is Node */
                    StructType = PT->FromType;
                    if (StructType->Base == TypeArray && StructType->FromType != NULL)
                        StructType = StructType->FromType;
                }
            }
            else if (RootIdxBase->LocalSlot >= 0 && IrCallDepth > 0
                     && RootIdxBase->LocalSlot < IR_CALL_MAX_LOCALS)
            {
                IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                struct ValueType *LFT = Fr->LocalFromType[RootIdxBase->LocalSlot];
                if (LFT != NULL)
                    StructType = LFT;
            }
            else if (RootIdxBase->LocalSlot < 0 && RootIdxBase->ParamSlot < 0
                     && RootIdxBase->Ident != NULL
                     && (IrGlobalValSoft(Parser, RootIdxBase) != NULL))
            {
                RootVar = IrGlobalValSoft(Parser, RootIdxBase);
                if (RootVar != NULL && RootVar->Typ != NULL)
                {
                    struct ValueType *T = RootVar->Typ;
                    if (T->Base == TypePointer && T->FromType != NULL)
                        T = T->FromType;
                    /* Strip all dimensions for grid[i][j].field; stopping
                     * after one FromType leaves an array with no Members. */
                    T = IrTypePeelArray(T);
                    if (T != NULL && (T->Base == TypeStruct || T->Base == TypeUnion))
                        StructType = T;
                    else if (T != NULL && T->FromType != NULL)
                        StructType = T->FromType;
                }
            }
        }
        else if (IdxBase != NULL && IdxBase->Kind == IrExMember)
        {
            /* p->pts[i].x : pts is array member of struct */
            void *ArrAddr;
            int Pb;
            struct Value *MV = NULL;
            if (IrResolveMember(Parser, IdxBase, &ArrAddr, &Pb))
            {
                /* re-lookup member type as array */
                if (IdxBase->Left != NULL && IdxBase->Ident != NULL)
                {
                    struct ValueType *ST = NULL;
                    /* Pure IR: param Path *p — no Variable shadow, use ParamTyp */
                    if (ST == NULL && IdxBase->Left->Kind == IrExIdent
                        && IdxBase->Left->ParamSlot >= 0 && IrCallDepth > 0
                        && IdxBase->Left->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
                    {
                        struct ValueType *PT = IrCallStack[IrCallDepth - 1]->ParamTyp[IdxBase->Left->ParamSlot];
                        if (PT != NULL)
                        {
                            ST = PT;
                            if (ST->Base == TypePointer && ST->FromType != NULL)
                                ST = ST->FromType;
                        }
                    }
                    /* Pure IR local struct* */
                    if (ST == NULL && IdxBase->Left->Kind == IrExIdent
                        && IdxBase->Left->LocalSlot >= 0 && IrCallDepth > 0
                        && IdxBase->Left->LocalSlot < IR_CALL_MAX_LOCALS)
                    {
                        ST = IrCallStack[IrCallDepth - 1]->LocalFromType[IdxBase->Left->LocalSlot];
                        if (ST == NULL)
                            ST = IrCallStack[IrCallDepth - 1]->LocalTyp[IdxBase->Left->LocalSlot];
                        if (ST != NULL && ST->Base == TypePointer && ST->FromType != NULL)
                            ST = ST->FromType;
                    }
                    if (ST == NULL && IdxBase->Left->Kind == IrExIdent
                        && IdxBase->Left->LocalSlot < 0 && IdxBase->Left->ParamSlot < 0
                        && (IrGlobalValSoft(Parser, IdxBase->Left) != NULL))
                    {
                        RootVar = IrGlobalVal(Parser, IdxBase->Left); /* global only via CachedVal */
                        if (RootVar && RootVar->Typ)
                        {
                            ST = RootVar->Typ;
                            if (ST->Base == TypePointer)
                                ST = ST->FromType;
                        }
                    }
                    else if (IdxBase->Left->Kind == IrExMember)
                    {
                        /* deeper nest: limited — StructType from parent field */
                        ST = NULL;
                    }
                    if (ST != NULL && ST->Members != NULL
                        && TableGet(ST->Members, IdxBase->Ident, &MV, NULL, NULL, NULL)
                        && MV != NULL && MV->Typ != NULL)
                    {
                        struct ValueType *AT = MV->Typ;
                        if (AT->Base == TypeArray && AT->FromType != NULL)
                            StructType = AT->FromType;
                        else
                            StructType = AT;
                    }
                }
            }
        }
        if (E->Op == TokenArrow)
        {
            BaseAddr = *(void **)BaseAddr;
            if (BaseAddr == NULL)
                ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
            if (StructType != NULL && StructType->Base == TypePointer)
                StructType = StructType->FromType;
        }
    }
    else if (E->Left != NULL && E->Left->Kind == IrExMember)
    {
        /* nested: resolve parent member as base (o.inner.x / p->next->value).
         * If parent field is a pointer (p->next), BaseAddr must be the *loaded*
         * pointer value, not the address of the pointer field itself. */
        void *ParentAddr;
        int Pb;
        struct Value *PMV = NULL;
        if (!IrResolveMember(Parser, E->Left, &ParentAddr, &Pb))
            return 0;
        BaseAddr = ParentAddr;
        if (Pb == TypePointer || Pb == TypeArray
            || E->Left->I32 == TypePointer)
        {
            BaseAddr = *(void **)ParentAddr;
            if (BaseAddr == NULL)
                ProgramFail(Parser, DiagnosticMsgNullPointerDereference);
        }
        /* Recover parent field type as StructType for this field lookup.
         * Walk up to root Ident/Index and TableGet each hop. */
        {
            IrExpr *Chain[8];
            int cn = 0;
            IrExpr *X = E->Left;
            while (X != NULL && cn < 8)
            {
                Chain[cn++] = X;
                if (X->Kind == IrExIdent || X->Kind == IrExIndex || X->Kind == IrExCall)
                    break;
                X = X->Left;
            }
            /* Chain[cn-1] is root (Ident, Index, or Call), Chain[0] is immediate parent Member */
            if (cn >= 1)
            {
                struct ValueType *ST = NULL;
                int k;
                IrExpr *Root = Chain[cn - 1];
                /* head()->next->x : root is Call (or Ident of function) */
                if (Root->Kind == IrExCall && Root->Ident != NULL
                    && (IrGlobalValSoft(Parser, Root) != NULL))
                {
                    RootVar = IrGlobalVal(Parser, Root); /* global/func via CachedVal */
                    if (RootVar != NULL && RootVar->Typ != NULL
                        && RootVar->Typ->Base == TypeFunction
                        && RootVar->Val != NULL
                        && RootVar->Val->FuncDef.ReturnType != NULL)
                    {
                        ST = RootVar->Val->FuncDef.ReturnType;
                        if (ST->Base == TypePointer)
                            ST = ST->FromType;
                    }
                }
                if (Root->Kind == IrExIdent && Root->Ident != NULL)
                {
                    if (Root->LocalSlot >= 0 && IrCallDepth > 0
                        && Root->LocalSlot < IR_CALL_MAX_LOCALS)
                    {
                        IrCallFrame *Fr = IrCallStack[IrCallDepth - 1];
                        ST = Fr->LocalFromType[Root->LocalSlot];
                        if (ST == NULL)
                            ST = Fr->LocalTyp[Root->LocalSlot];
                    }
                    else if (Root->ParamSlot >= 0 && IrCallDepth > 0
                             && Root->ParamSlot < IrCallStack[IrCallDepth - 1]->NumParams)
                    {
                        ST = IrCallStack[IrCallDepth - 1]->ParamTyp[Root->ParamSlot];
                        if (ST != NULL && (ST->Base == TypePointer || ST->Base == TypeArray)
                            && ST->FromType != NULL)
                            ST = ST->FromType;
                    }
                    else if (Root->LocalSlot < 0 && Root->ParamSlot < 0
                             && (IrGlobalValSoft(Parser, Root) != NULL))
                    {
                        RootVar = IrGlobalVal(Parser, Root); /* global only via CachedVal */
                        if (RootVar != NULL && RootVar->Typ != NULL)
                        {
                            ST = RootVar->Typ;
                            /* function ident used as root of call chain in older IR */
                            if (ST->Base == TypeFunction && RootVar->Val != NULL
                                && RootVar->Val->FuncDef.ReturnType != NULL)
                            {
                                ST = RootVar->Val->FuncDef.ReturnType;
                                if (ST->Base == TypePointer)
                                    ST = ST->FromType;
                            }
                            else if (ST->Base == TypePointer || ST->Base == TypeArray)
                                ST = ST->FromType;
                        }
                    }
                }
                else if (Root->Kind == IrExIndex && Root->Left != NULL)
                {
                    /* v[i].in.a — element type of array/pointer base */
                    IrExpr *B = Root->Left;
                    if (B->Kind == IrExIdent && B->LocalSlot >= 0 && IrCallDepth > 0
                        && B->LocalSlot < IR_CALL_MAX_LOCALS)
                    {
                        ST = IrCallStack[IrCallDepth - 1]->LocalFromType[B->LocalSlot];
                        if (ST == NULL)
                            ST = IrCallStack[IrCallDepth - 1]->LocalTyp[B->LocalSlot];
                    }
                    else if (B->Kind == IrExIdent && B->LocalSlot < 0 && B->ParamSlot < 0
                             && B->Ident != NULL
                             && (IrGlobalValSoft(Parser, B) != NULL))
                    {
                        RootVar = IrGlobalVal(Parser, B); /* global only via CachedVal */
                        if (RootVar != NULL && RootVar->Typ != NULL)
                        {
                            ST = RootVar->Typ;
                            if (ST->Base == TypePointer || ST->Base == TypeArray)
                                ST = ST->FromType;
                        }
                    }
                }
                /* walk Member hops from root toward parent */
                for (k = cn - 2; k >= 0 && ST != NULL && ST->Members != NULL; k--)
                {
                    if (Chain[k]->Kind != IrExMember || Chain[k]->Ident == NULL)
                        break;
                    if (!TableGet(ST->Members, Chain[k]->Ident, &PMV, NULL, NULL, NULL)
                        || PMV == NULL || PMV->Typ == NULL)
                    { ST = NULL; break; }
                    ST = PMV->Typ;
                    if (ST->Base == TypePointer)
                        ST = ST->FromType;
                }
                StructType = ST;
            }
        }
    }
    else
        return 0;

    /* Fast path: non-bitfield member with cached offset (structs/ps[i].x loops).
     * BaseAddr changes each iteration; offset and elem base are stable. */
    if (E->BindSlot == -2 && E->BitWidth == 0 && BaseAddr != NULL)
    {
        *AddrOut = (char *)BaseAddr + (int)E->ConstVal;
        *ElemBaseOut = E->I32;
        return 1;
    }

    if (StructType == NULL || StructType->Members == NULL)
        ProgramFail(Parser, "doesn't have a member called '%s'", Field);

    /* Bitfield: always re-resolve meta. Non-bitfield: TableGet once then BindSlot=-2. */
    if (!TableGet(StructType->Members, Field, &MemberValue, NULL, NULL, NULL))
        ProgramFail(Parser, "doesn't have a member called '%s'", Field);
    if (MemberValue == NULL || BaseAddr == NULL)
        return 0;
    {
        int member_off = 0;
        if (!IrResolveMemberOffsetValue(MemberValue, &member_off))
            return 0;
        *AddrOut = (char *)BaseAddr + member_off;
        E->ConstVal = member_off;
    }
    *ElemBaseOut = MemberValue->Typ ? MemberValue->Typ->Base : TypeInt;
#ifndef NO_FP
    /* double/float members: ensure FP elem base (some paths lose TypeFP) */
    if (MemberValue->Typ != NULL && MemberValue->Typ->Base == TypeFP)
        *ElemBaseOut = TypeFP;
#endif
    /* Keep TypeArray distinct from TypePointer:
     * - TypeArray → rvalue decays to address (no uninit leaf load)
     * - TypePointer → load the pointer word
     * Collapsing Array→Pointer caused false uninit on fill(s.a). */
    if (MemberValue->Typ != NULL && MemberValue->Typ->Base == TypePointer)
        *ElemBaseOut = TypePointer;
    else if (MemberValue->Typ != NULL && MemberValue->Typ->Base == TypeArray)
        *ElemBaseOut = TypeArray;
    /* cache offset + elem base for subsequent calls */
    E->I32 = *ElemBaseOut;
    E->BindSlot = -2;
    /* bitfield metadata */
    if (MemberValue->BitWidth > 0)
    {
        E->BitOffset = MemberValue->BitOffset;
        E->BitWidth = MemberValue->BitWidth;
        E->BitSigned = 1;
        if (MemberValue->Typ != NULL)
        {
            switch (MemberValue->Typ->Base)
            {
                case TypeUnsignedInt:
                case TypeUnsignedShort:
                case TypeUnsignedLong:
                case TypeUnsignedLongLong:
                case TypeUnsignedChar:
                    E->BitSigned = 0;
                    break;
                default:
                    break;
            }
        }
    }
    else
    {
        E->BitOffset = 0;
        E->BitWidth = 0;
        E->BitSigned = 0;
    }
    return 1;
}


/* ---- Shared struct brace pack (tree + linear, cold Decl) ---- */

/* Collect + sort top-level members. Names optional (NULL). Cold Decl only. */
int IrCollectSortedMembers(struct ValueType *ST,
                                  struct Value **MemberList, int *Offsets,
                                  char **Names, int cap)
{
    int MemberCount = 0, hi, i, j;
    struct TableEntry *Entry;
    if (ST == NULL || ST->Members == NULL || MemberList == NULL || Offsets == NULL)
        return 0;
    for (hi = 0; hi < ST->Members->Size; hi++)
    {
        for (Entry = ST->Members->HashTable[hi]; Entry != NULL; Entry = Entry->Next)
        {
            if (MemberCount >= cap) break;
            if (Entry->p.v.Val == NULL || Entry->p.v.Val->Val == NULL)
                continue;
            MemberList[MemberCount] = Entry->p.v.Val;
            if (!IrResolveMemberOffsetValue(Entry->p.v.Val, &Offsets[MemberCount]))
                continue;
            if (Names != NULL)
                Names[MemberCount] = Entry->p.v.Key;
            MemberCount++;
        }
    }
    for (i = 1; i < MemberCount; i++)
    {
        struct Value *mv = MemberList[i];
        int off = Offsets[i];
        char *nm = (Names != NULL) ? Names[i] : NULL;
        int bit = mv->BitOffset;
        int di = mv->DeclIndex;
        for (j = i; j > 0; j--)
        {
            int better;
            if (ST->Base == TypeUnion)
                better = (MemberList[j - 1]->DeclIndex > di);
            else
                better = (Offsets[j - 1] > off
                    || (Offsets[j - 1] == off
                        && MemberList[j - 1]->BitOffset > bit)
                    || (Offsets[j - 1] == off
                        && MemberList[j - 1]->BitOffset == bit
                        && MemberList[j - 1]->DeclIndex > di));
            if (!better) break;
            MemberList[j] = MemberList[j - 1];
            Offsets[j] = Offsets[j - 1];
            if (Names != NULL)
                Names[j] = Names[j - 1];
        }
        MemberList[j] = mv;
        Offsets[j] = off;
        if (Names != NULL)
            Names[j] = nm;
    }
    return MemberCount;
}


/* Brace-init placement plan (cold Decl). One layout walk shared by every
 * consumer: the tree/linear runtime pack (IrPackStructBraceInit) and the
 * linear lowering of non-constant initializers (ir_lin_compile.c), which
 * stores register values at Out[vi].Off. Out[vi].Off < 0 → value has no
 * target (still evaluated for side effects). Returns the number of values
 * consumed (designated: Count), 0 when nothing is placed. */
static int IrBracePlanSeq(struct ValueType *ST, int baseoff, int Count,
                          int vi, IrBracePlace *Out)
{
    struct Value *MemberList[128];
    int Offsets[128];
    int MemberCount, i;
    MemberCount = IrCollectSortedMembers(ST, MemberList, Offsets, NULL, 128);
    for (i = 0; i < MemberCount && vi < Count; i++)
    {
        struct Value *MV = MemberList[i];
        int off = baseoff + Offsets[i];
        enum BaseType B = (MV->Typ != NULL) ? MV->Typ->Base : TypeInt;
        if (MV->BitWidth > 0)
        {
            Out[vi].Off = off; Out[vi].MV = MV; Out[vi].EB = B; vi++;
        }
        else if (B == TypeArray && MV->Typ != NULL
                 && MV->Typ->FromType != NULL && MV->Typ->ArraySize > 0)
        {
            int k, asz = MV->Typ->ArraySize;
            int aes = MV->Typ->FromType->Sizeof > 0
                ? MV->Typ->FromType->Sizeof : 1;
            enum BaseType EB = MV->Typ->FromType->Base;
            for (k = 0; k < asz && vi < Count; k++)
            {
                Out[vi].Off = off + k * aes; Out[vi].MV = NULL;
                Out[vi].EB = EB; vi++;
            }
        }
        else if ((B == TypeStruct || B == TypeUnion)
                 && MV->Typ != NULL && MV->Typ->Members != NULL)
        {
            /* recursive nested aggregate (569: C{B{A{x},y},z}) */
            vi = IrBracePlanSeq(MV->Typ, off, Count, vi, Out);
        }
        else
        {
            Out[vi].Off = off; Out[vi].MV = MV; Out[vi].EB = B; vi++;
        }
    }
    return vi;
}

static int IrBracePlanDesignated(struct ValueType *ST, char **Names, int Count,
                                 IrBracePlace *Out)
{
    struct Value *MemberList[128];
    int Offsets[128];
    char *MemberNames[128];
    int MemberCount, hi, vi, seq = 0;
    struct TableEntry *Entry;
    MemberCount = IrCollectSortedMembers(ST, MemberList, Offsets, MemberNames, 128);
    for (vi = 0; vi < Count; vi++)
    {
        const char *nm = (Names != NULL) ? Names[vi] : NULL;
        int mi, found = 0;
        if (nm == NULL)
        {
            if (seq < MemberCount)
            {
                struct Value *MV = MemberList[seq];
                enum BaseType B = MV->Typ ? MV->Typ->Base : TypeInt;
                if (B != TypeStruct && B != TypeUnion)
                {
                    Out[vi].Off = Offsets[seq]; Out[vi].MV = MV; Out[vi].EB = B;
                }
                seq++;
            }
            continue;
        }
        for (mi = 0; mi < MemberCount && !found; mi++)
        {
            if (MemberNames[mi] != NULL && strcmp(MemberNames[mi], nm) == 0)
            {
                Out[vi].Off = Offsets[mi]; Out[vi].MV = MemberList[mi];
                Out[vi].EB = MemberList[mi]->Typ ? MemberList[mi]->Typ->Base : TypeInt;
                found = 1;
            }
        }
        /* Nested: .field inside union/struct member (699) */
        for (mi = 0; mi < MemberCount && !found; mi++)
        {
            struct Value *PM = MemberList[mi];
            struct ValueType *PT = PM->Typ;
            if (PT == NULL || PT->Members == NULL) continue;
            if (PT->Base != TypeUnion && PT->Base != TypeStruct) continue;
            for (hi = 0; hi < PT->Members->Size && !found; hi++)
            {
                for (Entry = PT->Members->HashTable[hi];
                     Entry != NULL; Entry = Entry->Next)
                {
                    if (Entry->p.v.Key != NULL
                        && strcmp(Entry->p.v.Key, nm) == 0
                        && Entry->p.v.Val != NULL
                        && Entry->p.v.Val->Val != NULL)
                    {
                        int nested_off = 0;
                        struct Value *NV = Entry->p.v.Val;
                        if (!IrResolveMemberOffsetValue(NV, &nested_off))
                            continue;
                        Out[vi].Off = Offsets[mi] + nested_off;
                        Out[vi].MV = NV;
                        Out[vi].EB = NV->Typ ? NV->Typ->Base : TypeInt;
                        found = 1;
                        break;
                    }
                }
            }
        }
    }
    return Count;
}

int IrBraceInitPlan(struct ValueType *ST, char **Names, int Count,
                    int narr, int esz, IrBracePlace *Out)
{
    int vi, elem;
    if (ST == NULL || ST->Members == NULL || Count <= 0 || Out == NULL)
        return 0;
    if (ST->Base != TypeStruct && ST->Base != TypeUnion)
        return 0;
    for (vi = 0; vi < Count; vi++)
    {
        Out[vi].Off = -1; Out[vi].MV = NULL; Out[vi].EB = TypeVoid;
    }
    if (IrBraceInitHasDesignated(Names, Count))
        return IrBracePlanDesignated(ST, Names, Count, Out);
    if (narr <= 0)
        narr = 1;
    if (esz <= 0)
        esz = ST->Sizeof > 0 ? ST->Sizeof : 0;
    if (esz <= 0)
        return 0;
    vi = 0;
    for (elem = 0; elem < narr && vi < Count; elem++)
        vi = IrBracePlanSeq(ST, elem * esz, Count, vi, Out);
    return vi;
}

/* Write one planned value (bitfield / float / scalar by base type). */
static inline void IrBracePlaceWrite(char *base, const IrBracePlace *P, long long cv)
{
    if (P->Off < 0)
        return;
    if (P->MV != NULL)
        IrWriteMemberScalar(base + P->Off, P->MV, cv);
    else
        IrWriteScalarBase(base + P->Off, P->EB, cv);
}

/* Unified brace pack entry (cold Decl). Single funnel for tree + linear. */
int IrPackStructBraceInit(struct ParseState *Parser, void *mem,
                          struct ValueType *ST,
                          const long long *Vals, char **Names,
                          IrExpr **Exprs, int Count,
                          int narr, int esz)
{
    IrBracePlace Local[64];
    IrBracePlace *Plan = Local;
    int vi, used;
    if (mem == NULL || ST == NULL || Vals == NULL || Count <= 0)
        return 0;
    if (Count > 64)
    {
        Plan = (IrBracePlace *)malloc(sizeof(IrBracePlace) * (size_t)Count);
        if (Plan == NULL)
            return 0;
    }
    used = IrBraceInitPlan(ST, Names, Count, narr, esz, Plan);
    for (vi = 0; vi < used; vi++)
    {
        long long cv = (Exprs != NULL && Exprs[vi] != NULL && Parser != NULL)
            ? IrEvalExpr(Parser, Exprs[vi]) : Vals[vi];
        IrBracePlaceWrite((char *)mem, &Plan[vi], cv);
    }
    if (Plan != Local)
        free(Plan);
    return used > 0;
}

/* Array-ident Decl init (cold). Shared tree + linear. */
int IrCopyArrayIdentInit(struct ParseState *Parser, IrCallFrame *Fr,
                         void *dst_mem, int n, int es, IrExpr *SrcE,
                         unsigned char *imap)
{
    void *srcp = NULL;
    size_t nbytes;
    int ss, src_es = 0;
    (void)Parser;
    if (dst_mem == NULL || n <= 0 || es <= 0 || SrcE == NULL || Fr == NULL)
        return 0;
    if (SrcE->Kind != IrExIdent || SrcE->LocalSlot < 0)
        return 0;
    ss = SrcE->LocalSlot;
    if (ss >= IR_CALL_MAX_LOCALS)
        return 0;
    if (Fr->LocalArraySize[ss] > 0 && Fr->LocalElemSize[ss] > 0)
        src_es = (int)Fr->LocalElemSize[ss];
    if (src_es > 0 && src_es != es)
    {
        if (Parser != NULL)
        {
            /* The parser has advanced by the time this shared helper runs;
             * restore the expression's captured source location instead of
             * inferring a column from identifier text. */
            if (SrcE->SourceLine > 0 && SrcE->CharacterPos > 0)
            {
                Parser->Line = SrcE->SourceLine;
                /* IrExpr::CharacterPos is already the lexer-captured end
                 * column for this identifier; adding its length again moved
                 * the caret past the token (and clamped it to the semicolon). */
                Parser->CharacterPos = SrcE->CharacterPos;
            }
            if (Fr->LocalTyp[ss] != NULL)
                ProgramFail(Parser, "can't assign int[] from %t[%d]", Fr->LocalTyp[ss], Fr->LocalArraySize[ss]);
            ProgramFail(Parser, "can't assign int[] from incompatible array");
        }
        return 0;
    }
    if (Fr->LocalVal[ss] != 0)
        srcp = (void *)(intptr_t)Fr->LocalVal[ss];
    nbytes = (size_t)n * (size_t)es;
    if (srcp != NULL && nbytes > 0)
        memcpy(dst_mem, srcp, nbytes);
    if (imap != NULL && n > 0)
        memset(imap, 1, (size_t)n);
    return srcp != NULL;
}

/* C7 — shared struct/union value copy + InitMap mark (tree Decl + lin Tok=3). */
void IrCopyStructBlob(void *dst, const void *src, int sz,
                      unsigned char *imap, int map_len)
{
    if (dst == NULL || src == NULL || sz <= 0)
        return;
    memcpy(dst, src, (size_t)sz);
    if (imap != NULL && map_len > 0)
        memset(imap, 1, (size_t)map_len);
}

