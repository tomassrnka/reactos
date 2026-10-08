/*
 * PROJECT:     ReactOS Kernel (fork-only verifier extensions)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Minimal in-kernel runtime for GCC's -fsanitize=undefined.
 *
 * Build gate:  CONFIG_KERNEL_VERIFIER, with the instrumentation added only
 *              when KERNEL_UBSAN is set (see ntoskrnl/CMakeLists.txt).
 * Runtime gate: KvFlags & KV_UBSAN (the VERIFIER=UBSAN boot option).
 *
 * The compiler emits the recoverable (non-abort) handler calls; each handler
 * logs the source location and the check type once per site and returns, so
 * execution continues. There is no external runtime library and no data is
 * taken from any sanitizer implementation - only the published call ABI.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#if defined(CONFIG_KERNEL_VERIFIER)

#define KV_UBSAN_FN __attribute__((used)) __attribute__((no_sanitize_undefined))

/* --- GCC UBSan ABI structures (layout only, no implementation copied) --- */
struct kv_source_location
{
    const char *FileName;
    UINT32 Line;
    UINT32 Column;
};

struct kv_type_descriptor
{
    UINT16 TypeKind;
    UINT16 TypeInfo;
    char TypeName[1];
};

struct kv_type_mismatch_v1
{
    struct kv_source_location Loc;
    struct kv_type_descriptor *Type;
    unsigned char LogAlignment;
    unsigned char TypeCheckKind;
};

struct kv_overflow_data
{
    struct kv_source_location Loc;
    struct kv_type_descriptor *Type;
};

struct kv_shift_oob_data
{
    struct kv_source_location Loc;
    struct kv_type_descriptor *LhsType;
    struct kv_type_descriptor *RhsType;
};

struct kv_out_of_bounds_data
{
    struct kv_source_location Loc;
    struct kv_type_descriptor *ArrayType;
    struct kv_type_descriptor *IndexType;
};

struct kv_invalid_value_data
{
    struct kv_source_location Loc;
    struct kv_type_descriptor *Type;
};

struct kv_pointer_overflow_data
{
    struct kv_source_location Loc;
};

static const char *
KvUbsanType(struct kv_type_descriptor *Type)
{
    return (Type != NULL) ? Type->TypeName : "?";
}

/*
 * Reports are queued, never printed from the handler: a check can fire inside
 * the debug print, trap or exception path while it holds the debugger's lock,
 * and a DbgPrint from there re-enters that lock and bugchecks. The watchdog
 * thread drains the ring at PASSIVE_LEVEL; a bug-check callback drains it on
 * a crash. A slot is published by writing Loc last.
 */
#define KV_UBSAN_RING 2048   /* holds the boot burst before the drainer starts */

typedef struct _KV_UBSAN_ENTRY
{
    struct kv_source_location *volatile Loc;
    PCSTR Kind;
    PCSTR Detail;
} KV_UBSAN_ENTRY;

static KV_UBSAN_ENTRY KvUbsanRing[KV_UBSAN_RING];
static volatile LONG KvUbsanHead;
static volatile LONG KvUbsanTail;
static volatile LONG KvUbsanDropped;
static KBUGCHECK_CALLBACK_RECORD KvUbsanBugCheckRecord;

static KV_UBSAN_FN VOID
KvUbsanReport(struct kv_source_location *Loc, PCSTR Kind, PCSTR Detail)
{
    LONG Index;
    KV_UBSAN_ENTRY *Entry;

    if (!(KvFlags & KV_UBSAN) || Loc == NULL)
        return;
    /* Key the one-shot on the static location record (unique per site). */
    if (!KvLogOnce((PVOID)Loc))
        return;

    Index = InterlockedIncrement(&KvUbsanHead) - 1;
    Entry = &KvUbsanRing[(ULONG)Index % KV_UBSAN_RING];
    if ((ULONG)(Index - KvUbsanTail) >= KV_UBSAN_RING || Entry->Loc != NULL)
    {
        InterlockedIncrement(&KvUbsanDropped);
        return;
    }
    Entry->Kind = Kind;
    Entry->Detail = Detail ? Detail : "";
    InterlockedExchangePointer((PVOID volatile *)&Entry->Loc, Loc);
}

/* Single consumer: the watchdog thread, or the bug-check callback. */
KV_UBSAN_FN VOID
NTAPI
KvUbsanDrain(VOID)
{
    for (;;)
    {
        KV_UBSAN_ENTRY *Entry = &KvUbsanRing[(ULONG)KvUbsanTail % KV_UBSAN_RING];
        struct kv_source_location *Loc = Entry->Loc;

        if (Loc == NULL)
            break;
        DbgPrint("KVERIFY: [UBSAN] %s at %s:%lu:%lu %s\n",
                 Entry->Kind,
                 Loc->FileName ? Loc->FileName : "?",
                 (ULONG)Loc->Line, (ULONG)Loc->Column,
                 Entry->Detail);
        InterlockedExchangePointer((PVOID volatile *)&Entry->Loc, NULL);
        InterlockedIncrement(&KvUbsanTail);
    }
    if (KvUbsanDropped != 0)
    {
        DbgPrint("KVERIFY: [UBSAN] %ld reports dropped (ring full)\n",
                 InterlockedExchange(&KvUbsanDropped, 0));
    }
}

static KV_UBSAN_FN VOID
NTAPI
KvUbsanBugCheckCallback(IN PVOID Buffer, IN ULONG Length)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    KvUbsanDrain();
}

KV_UBSAN_FN VOID
NTAPI
KvUbsanInitDrain(VOID)
{
    KeInitializeCallbackRecord(&KvUbsanBugCheckRecord);
    if (!KeRegisterBugCheckCallback(&KvUbsanBugCheckRecord,
                                    KvUbsanBugCheckCallback,
                                    NULL, 0, (PUCHAR)"KVUBSAN"))
    {
        DbgPrint("KVERIFY: [UBSAN] bug-check callback not registered\n");
    }
}

/*
 * Only the recoverable (non-abort) handlers are defined. We pass
 * -fsanitize-recover for every enabled check, so the compiler emits these and
 * never the _abort variants (which the compiler declares noreturn).
 */
#define KV_OVERFLOW_HANDLER(name)                                             \
    KV_UBSAN_FN VOID __ubsan_handle_##name(struct kv_overflow_data *Data,     \
                                           ULONG_PTR Lhs, ULONG_PTR Rhs)      \
    {                                                                         \
        UNREFERENCED_PARAMETER(Lhs); UNREFERENCED_PARAMETER(Rhs);             \
        KvUbsanReport(&Data->Loc, #name, KvUbsanType(Data->Type));            \
    }

KV_OVERFLOW_HANDLER(add_overflow)
KV_OVERFLOW_HANDLER(sub_overflow)
KV_OVERFLOW_HANDLER(mul_overflow)

KV_UBSAN_FN VOID __ubsan_handle_negate_overflow(struct kv_overflow_data *Data, ULONG_PTR Old)
{
    UNREFERENCED_PARAMETER(Old);
    KvUbsanReport(&Data->Loc, "negate_overflow", KvUbsanType(Data->Type));
}

KV_UBSAN_FN VOID __ubsan_handle_divrem_overflow(struct kv_overflow_data *Data, ULONG_PTR Lhs, ULONG_PTR Rhs)
{
    UNREFERENCED_PARAMETER(Lhs); UNREFERENCED_PARAMETER(Rhs);
    KvUbsanReport(&Data->Loc, "divrem_overflow", KvUbsanType(Data->Type));
}

/* --- Shift --- */
KV_UBSAN_FN VOID __ubsan_handle_shift_out_of_bounds(struct kv_shift_oob_data *Data, ULONG_PTR Lhs, ULONG_PTR Rhs)
{
    UNREFERENCED_PARAMETER(Lhs); UNREFERENCED_PARAMETER(Rhs);
    KvUbsanReport(&Data->Loc, "shift_out_of_bounds", KvUbsanType(Data->LhsType));
}

/* --- Array bounds --- */
KV_UBSAN_FN VOID __ubsan_handle_out_of_bounds(struct kv_out_of_bounds_data *Data, ULONG_PTR Index)
{
    UNREFERENCED_PARAMETER(Index);
    KvUbsanReport(&Data->Loc, "out_of_bounds", KvUbsanType(Data->ArrayType));
}

/* --- Null / alignment / object-size (all arrive as type_mismatch_v1) --- */
static KV_UBSAN_FN VOID
KvUbsanTypeMismatch(struct kv_type_mismatch_v1 *Data, ULONG_PTR Pointer)
{
    ULONG_PTR Alignment = (ULONG_PTR)1 << Data->LogAlignment;
    PCSTR Detail;
    if (Pointer == 0)
        Detail = "null pointer";
    else if (Alignment != 0 && (Pointer & (Alignment - 1)) != 0)
        Detail = "misaligned pointer";
    else
        Detail = "object too small";
    KvUbsanReport(&Data->Loc, "type_mismatch", Detail);
}
KV_UBSAN_FN VOID __ubsan_handle_type_mismatch_v1(struct kv_type_mismatch_v1 *Data, ULONG_PTR Pointer)
{
    KvUbsanTypeMismatch(Data, Pointer);
}

/* --- Pointer overflow --- */
KV_UBSAN_FN VOID __ubsan_handle_pointer_overflow(struct kv_pointer_overflow_data *Data, ULONG_PTR Base, ULONG_PTR Result)
{
    UNREFERENCED_PARAMETER(Base); UNREFERENCED_PARAMETER(Result);
    KvUbsanReport(&Data->Loc, "pointer_overflow", "");
}

/* --- Invalid enum/bool load --- */
KV_UBSAN_FN VOID __ubsan_handle_load_invalid_value(struct kv_invalid_value_data *Data, ULONG_PTR Value)
{
    UNREFERENCED_PARAMETER(Value);
    KvUbsanReport(&Data->Loc, "load_invalid_value", KvUbsanType(Data->Type));
}

#endif /* CONFIG_KERNEL_VERIFIER */
