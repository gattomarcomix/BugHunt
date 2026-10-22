/* stdarg.h for BabyC scripts — WITH_CSTDLIB_EXT (desktop builds). */
#include "../picoc.h"
#include "../interpreter.h"
#include "../ir_runtime.h"

/* A cursor may refer either to the shared Value ABI (token/Tree calls) or to
 * the raw 64-bit payload vector used by the IR/Linear call boundary. */
struct BabyCVaCursor
{
    struct StackFrame *Frame;
    IrCallFrame *IrFrame;
    int Index;
    int Active;
    union AnyValue Scratch; /* raw IR payload backing for the pointer-form helper */
};

enum StdargMacroIntrinsicKind
{
    StdargMacroNone = 0, StdargMacroStart, StdargMacroArg,
    StdargMacroEnd, StdargMacroCopy
};

int StdargMacroIntrinsicKind(Picoc *pc, struct MacroDef *MacroDefValue)
{
    unsigned i;
    for (i = 1; i < sizeof(((Picoc *)0)->StdargMacroValues) / sizeof(((Picoc *)0)->StdargMacroValues[0]); ++i)
        if (pc->StdargMacroValues[i] == MacroDefValue)
            return (int)i;
    return StdargMacroNone;
}

/* va_list is an opaque struct of sizeof(FILE) bytes (cstdlib/stdio.c) and a struct assignment (e.g. the
 * va_copy macro) copies that many bytes from the cursor block. Allocate at least that much, otherwise the copy
 * reads past the block (ASan heap-buffer-overflow: 176-byte cursor vs 216-byte va_list). */
#define BABYC_VA_CURSOR_ALLOC ((int)(sizeof(struct BabyCVaCursor) > sizeof(FILE) ? sizeof(struct BabyCVaCursor) : sizeof(FILE)))

static struct BabyCVaCursor *StdargCursorNew(struct ParseState *Parser,
    struct StackFrame *Frame, IrCallFrame *IrFrame)
{
    struct BabyCVaCursor *Cursor = (struct BabyCVaCursor *)HeapAllocMem(
        Parser->pc, BABYC_VA_CURSOR_ALLOC);
    if (Cursor == NULL) ProgramFail(Parser, "out of memory creating va_list");
    Cursor->Frame = Frame;
    Cursor->IrFrame = IrFrame;
    Cursor->Index = Frame != NULL ? Frame->CallAbi.FixedArgumentCount
                                  : IrFrame->CallAbiFixedArgumentCount;
    Cursor->Active = 1;
    return Cursor;
}

static struct BabyCVaCursor *StdargCursorFromValue(struct ParseState *Parser,
    struct Value *Value)
{
    struct BabyCVaCursor *Cursor;
    if (Value == NULL || Value->Val == NULL)
        ProgramFail(Parser, "invalid va_list argument");
    Cursor = (struct BabyCVaCursor *)Value->Val->Pointer;
    if (Cursor == NULL || !Cursor->Active)
        ProgramFail(Parser, "va_list is not active");
    return Cursor;
}

static struct BabyCVaCursor *StdargCursorFromPointer(struct ParseState *Parser,
    void *Pointer)
{
    struct BabyCVaCursor *Cursor = (struct BabyCVaCursor *)Pointer;
    if (Cursor == NULL || !Cursor->Active)
        ProgramFail(Parser, "va_list is not active");
    return Cursor;
}

static long long StdargValueBits(struct Value *Value)
{
    if (Value == NULL || Value->Val == NULL) return 0;
    if (Value->Typ != NULL)
    {
        switch (Value->Typ->Base)
        {
#ifndef NO_FP
            case TypeFP:
            {
                long long Bits = 0;
                memcpy(&Bits, &Value->Val->FP, sizeof(Bits));
                return Bits;
            }
#endif
            case TypePointer: case TypeArray: case TypeFunction:
                return (long long)(intptr_t)Value->Val->Pointer;
            case TypeChar: return (long long)(int)Value->Val->Character;
            case TypeUnsignedChar: return (long long)(unsigned int)Value->Val->UnsignedCharacter;
            case TypeShort: return (long long)(int)Value->Val->ShortInteger;
            case TypeUnsignedShort: return (long long)(unsigned int)Value->Val->UnsignedShortInteger;
            case TypeUnsignedInt: return (long long)(unsigned int)Value->Val->UnsignedInteger;
            case TypeLong: return (long long)Value->Val->LongInteger;
            case TypeUnsignedLong: return (long long)Value->Val->UnsignedLongInteger;
            case TypeLongLong: return Value->Val->LongLongInteger;
            case TypeUnsignedLongLong: return (long long)Value->Val->UnsignedLongLongInteger;
            default: return (long long)Value->Val->Integer;
        }
    }
    return Value->Val->LongLongInteger;
}

static int StdargVaTypeCompatible(struct ValueType *Actual, struct ValueType *Requested)
{
    int A, R;
    if (Actual == NULL || Requested == NULL) return 0;
    A = Actual->Base; R = Requested->Base;
    if (A == TypeStruct || A == TypeUnion || R == TypeStruct || R == TypeUnion)
        return A == R && Actual == Requested;
    if (A == R) {
        if (A == TypePointer) return 1;
        return 1;
    }
    /* C11 7.16.1.1: corresponding signed/unsigned types of equal rank. */
    if ((A == TypeInt && R == TypeUnsignedInt) || (A == TypeUnsignedInt && R == TypeInt)
        || (A == TypeLong && R == TypeUnsignedLong) || (A == TypeUnsignedLong && R == TypeLong)
        || (A == TypeLongLong && R == TypeUnsignedLongLong)
        || (A == TypeUnsignedLongLong && R == TypeLongLong)) return 1;
    return 0;
}

long long StdargVaArgBits(struct ParseState *Parser, void *CursorPointer,
    struct ValueType *RequestedType)
{
    struct BabyCVaCursor *Cursor = StdargCursorFromPointer(Parser, CursorPointer);
    long long Bits;
    if (Cursor->Frame != NULL)
    {
        struct CallAbiFrame *Abi = &Cursor->Frame->CallAbi;
        struct Value *Argument;
        if (Cursor->Index < 0 || Cursor->Index >= Abi->ArgumentCount)
            ProgramFail(Parser, "va_arg past end of arguments");
        Argument = Abi->Arguments[Cursor->Index++];
        if (Argument == NULL || Argument->Val == NULL)
            ProgramFail(Parser, "invalid variadic argument");
        if (!StdargVaTypeCompatible(Argument->Typ, RequestedType))
            ProgramFail(Parser, "va_arg type mismatch");
        Bits = StdargValueBits(Argument);
    }
    else
    {
        IrCallAbiSlot *Slot;
        if (Cursor->Index < 0
            || Cursor->Index >= Cursor->IrFrame->CallAbiArgumentCount)
            ProgramFail(Parser, "va_arg past end of arguments");
        Slot = &Cursor->IrFrame->CallAbiArguments[Cursor->Index++];
        if (Slot->Type == NULL)
            ProgramFail(Parser, "untyped variadic slot");
        if (!StdargVaTypeCompatible(Slot->Type, RequestedType))
            ProgramFail(Parser, "va_arg type mismatch");
        switch (Slot->Type->Base)
        {
#ifndef NO_FP
            case TypeFP:
                memcpy(&Bits, &Slot->Payload.D, sizeof(Bits));
                break;
#endif
            case TypePointer: case TypeArray: case TypeFunction:
                Bits = (long long)(intptr_t)Slot->Payload.P;
                break;
            case TypeUnsignedInt: case TypeUnsignedLong: case TypeUnsignedLongLong:
                Bits = (long long)Slot->Payload.U;
                break;
            default:
                Bits = Slot->Payload.I;
                break;
        }
    }
    return Bits;
}

static struct BabyCVaCursor *StdargCreateTreeCursor(struct ParseState *Parser)
{
    struct StackFrame *Owner = Parser->pc->TopStackFrame;
    struct Value *Function = NULL;
    while (Owner != NULL)
    {
        if (Owner->FuncName != NULL)
        {
            VariableGet(Parser->pc, Parser, Owner->FuncName, &Function);
            if (Function != NULL && Function->Typ != NULL
                && Function->Typ->Base == TypeFunction
                && Function->Val->FuncDef.VarArgs)
                break;
        }
        Owner = Owner->PreviousStackFrame;
        Function = NULL;
    }
    if (Owner == NULL || Function == NULL || Owner->CallAbi.Arguments == NULL
        || !Owner->CallAbi.IsVariadic)
        ProgramFail(Parser, "va_start outside variadic function");
    return StdargCursorNew(Parser, Owner, NULL);
}

void StdargVaStart(struct ParseState *Parser, struct Value *RV,
    struct Value **Param, int N)
{
    struct BabyCVaCursor *Cursor;
    if (N == 1)
    {
        IrCallFrame *Owner = (IrCallDepth > 0) ? IrCallStack[IrCallDepth - 1] : NULL;
        if (Owner == NULL || Owner->CallAbiArguments == NULL
            || !Owner->CallAbiIsVariadic)
            ProgramFail(Parser, "va_start outside variadic function");
        Cursor = StdargCursorNew(Parser, NULL, Owner);
        if (RV == NULL || RV->Val == NULL)
            ProgramFail(Parser, "invalid va_start result");
        RV->Val->Pointer = Cursor;
        ExpressionMarkInitialized(RV);
        return;
    }
    if (Param == NULL || Param[0] == NULL || Param[0]->Val == NULL)
        ProgramFail(Parser, "invalid va_list target");
    Cursor = StdargCreateTreeCursor(Parser);
    Param[0]->Val->Pointer = Cursor;
    ExpressionMarkInitialized(Param[0]);
    if (RV != NULL && RV->Val != NULL) RV->Val->Pointer = Cursor;
}

struct Value *StdargVaArgValue(struct ParseState *Parser, struct Value *List,
    struct ValueType *RequestedType)
{
    struct Value *Result;
    long long RawBits = StdargVaArgBits(Parser,
        (List != NULL && List->Val != NULL) ? List->Val->Pointer : NULL,
        RequestedType);
    Result = VariableAllocValueFromType(Parser->pc, Parser, RequestedType,
        FALSE, NULL, FALSE);
    if (Result == NULL) ProgramFail(Parser, "out of memory retrieving variadic argument");
    if (RequestedType->Base == TypePointer || RequestedType->Base == TypeArray)
        Result->Val->Pointer = (void *)(intptr_t)RawBits;
#ifndef NO_FP
    else if (RequestedType->Base == TypeFP)
    {
        memcpy(&Result->Val->FP, &RawBits, sizeof(Result->Val->FP));
        if (RequestedType->FPFormat == FPStorageBinary32)
            Result->Val->FP = (double)(float)Result->Val->FP;
    }
#endif
    else if (RequestedType->Base == TypeLongLong)
        Result->Val->LongLongInteger = RawBits;
    else if (RequestedType->Base == TypeUnsignedLongLong)
        Result->Val->UnsignedLongLongInteger = (unsigned long long)RawBits;
    else if (RequestedType->Base == TypeLong)
        Result->Val->LongInteger = (long)RawBits;
    else if (RequestedType->Base == TypeUnsignedLong)
        Result->Val->UnsignedLongInteger = (unsigned long)RawBits;
    else if (RequestedType->Base == TypeUnsignedInt)
        Result->Val->UnsignedInteger = (unsigned int)RawBits;
    else if (RequestedType->Base == TypeShort)
        Result->Val->ShortInteger = (short)RawBits;
    else if (RequestedType->Base == TypeUnsignedShort)
        Result->Val->UnsignedShortInteger = (unsigned short)RawBits;
    else if (RequestedType->Base == TypeChar)
        Result->Val->Character = (char)RawBits;
    else if (RequestedType->Base == TypeUnsignedChar)
        Result->Val->UnsignedCharacter = (unsigned char)RawBits;
    else
        Result->Val->Integer = (int)RawBits;
    Result->IsInitialized = TRUE;
    return Result;
}

static void StdargVaArg(struct ParseState *Parser, struct Value *RV,
    struct Value **Param, int N)
{
    struct BabyCVaCursor *Cursor;
    void *Address;
    int Size;
    (void)N;
    if (Param == NULL || Param[0] == NULL || Param[0]->Val == NULL
        || Param[1] == NULL || Param[1]->Val == NULL)
        ProgramFail(Parser, "invalid va_arg arguments");
    Cursor = StdargCursorFromValue(Parser, Param[0]);
    Size = Param[1]->Val->Integer;
    if (Size <= 0 || Size > (int)sizeof(union AnyValue))
        ProgramFail(Parser, "unsupported va_arg object size");
    if (Cursor->Frame != NULL)
    {
        struct CallAbiFrame *Abi = &Cursor->Frame->CallAbi;
        struct Value *Argument;
        if (Cursor->Index < 0 || Cursor->Index >= Abi->ArgumentCount)
            ProgramFail(Parser, "va_arg past end of arguments");
        Argument = Abi->Arguments[Cursor->Index++];
        if (Argument == NULL || Argument->Val == NULL)
            ProgramFail(Parser, "invalid variadic argument");
        Address = (void *)Argument->Val;
    }
    else
    {
        long long Bits;
        if (Cursor->Index < 0
            || Cursor->Index >= Cursor->IrFrame->CallAbiArgumentCount)
            ProgramFail(Parser, "va_arg past end of arguments");
        IrCallAbiSlot *Slot = &Cursor->IrFrame->CallAbiArguments[Cursor->Index++];
        Bits = Slot->Payload.I;
        Cursor->Scratch.LongLongInteger = Bits;
        Address = (void *)&Cursor->Scratch;
    }
    RV->Val->Pointer = Address;
}

void StdargVaCopy(struct ParseState *Parser, struct Value *RV,
    struct Value **Param, int N)
{
    struct BabyCVaCursor *Source;
    struct BabyCVaCursor *Copy;
    if (N == 1)
    {
        Source = StdargCursorFromValue(Parser, Param[0]);
        Copy = (struct BabyCVaCursor *)HeapAllocMem(Parser->pc, BABYC_VA_CURSOR_ALLOC);
        if (Copy == NULL) ProgramFail(Parser, "out of memory copying va_list");
        *Copy = *Source;
        if (RV == NULL || RV->Val == NULL) ProgramFail(Parser, "invalid va_copy result");
        RV->Val->Pointer = Copy;
        return;
    }
    if (Param == NULL || Param[0] == NULL || Param[0]->Val == NULL
        || Param[1] == NULL || Param[1]->Val == NULL)
        ProgramFail(Parser, "invalid va_copy argument");
    Source = (struct BabyCVaCursor *)(*(struct BabyCVaCursor **)Param[1]->Val);
    if (Source == NULL || !Source->Active)
        ProgramFail(Parser, "va_list is not active");
    Copy = (struct BabyCVaCursor *)HeapAllocMem(Parser->pc, BABYC_VA_CURSOR_ALLOC);
    if (Copy == NULL) ProgramFail(Parser, "out of memory copying va_list");
    *Copy = *Source;
    Param[0]->Val->Pointer = Copy;
    ExpressionMarkInitialized(Param[0]);
    if (RV != NULL && RV->Val != NULL) RV->Val->Pointer = Copy;
}

void StdargVaEnd(struct ParseState *Parser, struct Value *RV,
    struct Value **Param, int N)
{
    struct BabyCVaCursor *Cursor;
    (void)N;
    if (Param == NULL || Param[0] == NULL || Param[0]->Val == NULL)
        ProgramFail(Parser, "invalid va_list target");
    Cursor = (struct BabyCVaCursor *)Param[0]->Val->Pointer;
    if (Cursor != NULL)
    {
        /* The cursor may still be referenced by an expression-stack Value
         * or a copied va_list during frame teardown. Invalidate it here;
         * allocator ownership is released with the interpreter heap. */
        Cursor->Active = 0;
        Param[0]->Val->Pointer = NULL;
    }
}


void StdargSetupFunc(Picoc *pc)
{
    static const char *Guard = "__babyc_va_list_ok";
    if (!VariableDefined(pc, TableStrRegister(pc, Guard)))
    {
        /* The opaque pointer cursor is shared by token and Linear execution. */
        if (!VariableDefined(pc, TableStrRegister(pc, "va_list")))
            PicocParse(pc, "stdarg.h", "typedef void *va_list;",
                (int)strlen("typedef void *va_list;"), TRUE, TRUE, FALSE, FALSE);
        /* Define each macro independently: a partially preloaded header must
         * never cause ParseMacroDefinition to collide with an existing name. */
        if (!VariableDefined(pc, TableStrRegister(pc, "va_start")))
            PicocParse(pc, "stdarg.h",
                "#define va_start(ap, last) (ap = __babyc_va_start(0))\n",
                (int)strlen("#define va_start(ap, last) (ap = __babyc_va_start(0))\n"),
                TRUE, TRUE, FALSE, FALSE);
        if (!VariableDefined(pc, TableStrRegister(pc, "va_arg")))
            PicocParse(pc, "stdarg.h",
                "#define va_arg(ap, type) (*(type *)__babyc_va_arg(ap, sizeof(type)))\n",
                (int)strlen("#define va_arg(ap, type) (*(type *)__babyc_va_arg(ap, sizeof(type)))\n"),
                TRUE, TRUE, FALSE, FALSE);
        if (!VariableDefined(pc, TableStrRegister(pc, "va_end")))
            PicocParse(pc, "stdarg.h",
                "#define va_end(ap) (ap = __babyc_va_end(ap))\n",
                (int)strlen("#define va_end(ap) (ap = __babyc_va_end(ap))\n"),
                TRUE, TRUE, FALSE, FALSE);
        if (!VariableDefined(pc, TableStrRegister(pc, "va_copy")))
            PicocParse(pc, "stdarg.h",
                "#define va_copy(dst, src) (dst = __babyc_va_copy(src))\n",
                (int)strlen("#define va_copy(dst, src) (dst = __babyc_va_copy(src))\n"),
                TRUE, TRUE, FALSE, FALSE);
        {
            static const char *Names[] = {NULL, "va_start", "va_arg", "va_end", "va_copy"};
            unsigned i;
            for (i = 1; i < sizeof(Names) / sizeof(Names[0]); ++i) {
                struct Value *MacroValue = NULL;
                if (TableGet(&pc->GlobalTable, TableStrRegister(pc, Names[i]),
                    &MacroValue, NULL, NULL, NULL) && MacroValue != NULL
                    && MacroValue->Typ != NULL && MacroValue->Typ->Base == TypeMacro)
                    pc->StdargMacroValues[i] = &MacroValue->Val->MacroDef;
            }
        }
        VariableDefine(pc, NULL, TableStrRegister(pc, Guard), NULL, &pc->VoidType, FALSE);
    }
}

struct LibraryFunction StdargFunctions[] =
{
    {StdargVaStart, "void *__babyc_va_start(int);"},
    {StdargVaArg, "void *__babyc_va_arg(void *, int);"},
    {StdargVaEnd, "void *__babyc_va_end(void *);"},
    {StdargVaCopy, "void *__babyc_va_copy(void *);"},
    {NULL, NULL}
};
