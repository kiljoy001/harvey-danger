/*
 * Standalone UBSan runtime for Harvey/ARM64.
 *
 * compiler-rt's standalone runtime brings sanitizer_common, whose Linux
 * layer is built around raw syscalls and /proc.  Harvey has no Linux
 * syscall interface, but the UBSan handler ABI itself is small: the
 * compiler passes a pointer to a descriptor that already contains the
 * source location and type names.  This file implements that ABI directly
 * and prints "file:line:col: runtime error: ..." using APEX's stdio.
 *
 * The recoverable handlers return so execution continues; the _abort
 * variants and the unrecoverable checks abort.  Stack traces and
 * symbolization are deliberately left to the caller.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	const char *Filename;
	uint32_t Line;
	uint32_t Column;
} SourceLocation;

typedef struct {
	uint16_t TypeKind;
	uint16_t TypeInfo;
	char TypeName[1];
} TypeDescriptor;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *Type;
	unsigned char LogAlignment;
	unsigned char TypeCheckKind;
} TypeMismatchData;

typedef struct {
	SourceLocation Loc;
	SourceLocation AssumptionLoc;
	const TypeDescriptor *Type;
} AlignmentAssumptionData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *Type;
} OverflowData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *LHSType;
	const TypeDescriptor *RHSType;
} ShiftOutOfBoundsData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *ArrayType;
	const TypeDescriptor *IndexType;
} OutOfBoundsData;

typedef struct {
	SourceLocation Loc;
} UnreachableData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *Type;
} VLABoundData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *FromType;
	const TypeDescriptor *ToType;
} FloatCastOverflowDataV2;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *Type;
} InvalidValueData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *FromType;
	const TypeDescriptor *ToType;
	unsigned char Kind;
	unsigned int BitfieldBits;
} ImplicitConversionData;

typedef struct {
	SourceLocation Loc;
	unsigned char Kind;
} InvalidBuiltinData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *ExpectedType;
} InvalidObjCCast;

typedef struct {
	SourceLocation AttrLoc;
} NonNullReturnData;

typedef struct {
	SourceLocation Loc;
	SourceLocation AttrLoc;
	int ArgIndex;
} NonNullArgData;

typedef struct {
	SourceLocation Loc;
} PointerOverflowData;

typedef struct {
	unsigned char CheckKind;
	SourceLocation Loc;
	const TypeDescriptor *Type;
} CFICheckFailData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *Type;
} FunctionTypeMismatchData;

typedef struct {
	SourceLocation Loc;
	const TypeDescriptor *Type;
	void *TypeInfo;
	unsigned char TypeCheckKind;
} DynamicTypeCacheMissData;

static const char *
typename_of(const TypeDescriptor *t)
{
	/*
	 * Some descriptors (notably array/index types) do not carry a
	 * printable name at TypeName.  Do not dereference it: the check name
	 * and source location are the useful part, and a bad descriptor must
	 * not crash the reporting path.
	 */
	(void)t;
	return "type";
}

static void
report(const SourceLocation *loc, const char *fmt, ...)
{
	char buf[768];
	va_list ap;
	int n;

	if(loc != NULL && loc->Filename != NULL)
		n = snprintf(buf, sizeof buf, "%s:%u:%u: runtime error: ",
		    loc->Filename, loc->Line, loc->Column);
	else
		n = snprintf(buf, sizeof buf, "runtime error: ");
	if(n < 0)
		n = 0;
	if(n > (int)sizeof buf - 1)
		n = sizeof buf - 1;
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof buf - n, fmt, ap);
	va_end(ap);
	write(2, buf, strlen(buf));
	write(2, "\n", 1);
}

static void
die(void)
{
	abort();
}

#define RECOVER(name) \
	void __ubsan_handle_##name(void *data, uintptr_t a) \
	{ (void)data; (void)a; }

#define RECOVER2(name) \
	void __ubsan_handle_##name(void *data, uintptr_t a, uintptr_t b) \
	{ (void)data; (void)a; (void)b; }

#define ABORT(name, args) \
	void __ubsan_handle_##name##_abort args { die(); }

void
__ubsan_handle_type_mismatch_v1(void *data, uintptr_t pointer)
{
	TypeMismatchData *d = data;

	report(&d->Loc, "type-mismatch on address 0x%lx (type %s, kind %u)",
	    (unsigned long)pointer, typename_of(d->Type), d->TypeCheckKind);
}

void
__ubsan_handle_type_mismatch_v1_abort(void *data, uintptr_t pointer)
{
	__ubsan_handle_type_mismatch_v1(data, pointer);
	die();
}

void
__ubsan_handle_alignment_assumption(void *data, uintptr_t pointer,
    uintptr_t alignment, uintptr_t offset)
{
	AlignmentAssumptionData *d = data;

	report(&d->Loc, "alignment-assumption on 0x%lx (alignment 0x%lx, offset 0x%lx)",
	    (unsigned long)pointer, (unsigned long)alignment, (unsigned long)offset);
}

void
__ubsan_handle_alignment_assumption_abort(void *data, uintptr_t pointer,
    uintptr_t alignment, uintptr_t offset)
{
	__ubsan_handle_alignment_assumption(data, pointer, alignment, offset);
	die();
}

#define OVERFLOW(name, op) \
	void __ubsan_handle_##name(void *data, uintptr_t lhs, uintptr_t rhs) \
	{ \
		OverflowData *d = data; \
		report(&d->Loc, #name " of type %s: 0x%lx " op " 0x%lx", \
		    typename_of(d->Type), (unsigned long)lhs, (unsigned long)rhs); \
	} \
	void __ubsan_handle_##name##_abort(void *data, uintptr_t lhs, uintptr_t rhs) \
	{ \
		__ubsan_handle_##name(data, lhs, rhs); \
		die(); \
	}

OVERFLOW(add_overflow, "+")
OVERFLOW(sub_overflow, "-")
OVERFLOW(mul_overflow, "*")
OVERFLOW(divrem_overflow, "/")

void
__ubsan_handle_negate_overflow(void *data, uintptr_t old)
{
	OverflowData *d = data;

	report(&d->Loc, "negate-overflow of type %s: -0x%lx",
	    typename_of(d->Type), (unsigned long)old);
}

void
__ubsan_handle_negate_overflow_abort(void *data, uintptr_t old)
{
	__ubsan_handle_negate_overflow(data, old);
	die();
}

void
__ubsan_handle_shift_out_of_bounds(void *data, uintptr_t lhs, uintptr_t rhs)
{
	ShiftOutOfBoundsData *d = data;

	report(&d->Loc, "shift-out-of-bounds: 0x%lx shifted by 0x%lx (%s, %s)",
	    (unsigned long)lhs, (unsigned long)rhs, typename_of(d->LHSType),
	    typename_of(d->RHSType));
}

void
__ubsan_handle_shift_out_of_bounds_abort(void *data, uintptr_t lhs,
    uintptr_t rhs)
{
	__ubsan_handle_shift_out_of_bounds(data, lhs, rhs);
	die();
}

void
__ubsan_handle_out_of_bounds(void *data, uintptr_t index)
{
	OutOfBoundsData *d = data;

	(void)index;
	report(&d->Loc, "out-of-bounds");
}

void
__ubsan_handle_out_of_bounds_abort(void *data, uintptr_t index)
{
	__ubsan_handle_out_of_bounds(data, index);
	die();
}

void
__ubsan_handle_local_out_of_bounds(void)
{
	report(NULL, "local-out-of-bounds");
}

void
__ubsan_handle_local_out_of_bounds_abort(void)
{
	report(NULL, "local-out-of-bounds");
	die();
}

void
__ubsan_handle_vla_bound_not_positive(void *data, uintptr_t bound)
{
	VLABoundData *d = data;

	report(&d->Loc, "vla-bound-not-positive: 0x%lx (%s)",
	    (unsigned long)bound, typename_of(d->Type));
}

void
__ubsan_handle_vla_bound_not_positive_abort(void *data, uintptr_t bound)
{
	__ubsan_handle_vla_bound_not_positive(data, bound);
	die();
}

void
__ubsan_handle_float_cast_overflow(void *data, uintptr_t from)
{
	FloatCastOverflowDataV2 *d = data;

	report(&d->Loc, "float-cast-overflow: 0x%lx (%s -> %s)",
	    (unsigned long)from, typename_of(d->FromType), typename_of(d->ToType));
}

void
__ubsan_handle_float_cast_overflow_abort(void *data, uintptr_t from)
{
	__ubsan_handle_float_cast_overflow(data, from);
	die();
}

void
__ubsan_handle_load_invalid_value(void *data, uintptr_t val)
{
	InvalidValueData *d = data;

	report(&d->Loc, "load-invalid-value: 0x%lx (%s)",
	    (unsigned long)val, typename_of(d->Type));
}

void
__ubsan_handle_load_invalid_value_abort(void *data, uintptr_t val)
{
	__ubsan_handle_load_invalid_value(data, val);
	die();
}

void
__ubsan_handle_implicit_conversion(void *data, uintptr_t src, uintptr_t dst)
{
	ImplicitConversionData *d = data;

	report(&d->Loc, "implicit-conversion: 0x%lx -> 0x%lx (%s -> %s)",
	    (unsigned long)src, (unsigned long)dst, typename_of(d->FromType),
	    typename_of(d->ToType));
}

void
__ubsan_handle_implicit_conversion_abort(void *data, uintptr_t src,
    uintptr_t dst)
{
	__ubsan_handle_implicit_conversion(data, src, dst);
	die();
}

void
__ubsan_handle_invalid_builtin(void *data)
{
	InvalidBuiltinData *d = data;

	report(&d->Loc, "invalid-builtin (kind %u)", d->Kind);
}

void
__ubsan_handle_invalid_builtin_abort(void *data)
{
	__ubsan_handle_invalid_builtin(data);
	die();
}

void
__ubsan_handle_invalid_objc_cast(void *data, uintptr_t pointer)
{
	InvalidObjCCast *d = data;

	report(&d->Loc, "invalid-objc-cast: 0x%lx (%s)",
	    (unsigned long)pointer, typename_of(d->ExpectedType));
}

void
__ubsan_handle_invalid_objc_cast_abort(void *data, uintptr_t pointer)
{
	__ubsan_handle_invalid_objc_cast(data, pointer);
	die();
}

void
__ubsan_handle_nonnull_arg(void *data)
{
	NonNullArgData *d = data;

	report(&d->Loc, "nonnull-arg: argument %d is null", d->ArgIndex);
}

void
__ubsan_handle_nonnull_arg_abort(void *data)
{
	__ubsan_handle_nonnull_arg(data);
	die();
}

void
__ubsan_handle_nullability_arg(void *data)
{
	NonNullArgData *d = data;

	report(&d->Loc, "nullability-arg: argument %d is null", d->ArgIndex);
}

void
__ubsan_handle_nullability_arg_abort(void *data)
{
	__ubsan_handle_nullability_arg(data);
	die();
}

void
__ubsan_handle_nonnull_return_v1(void *data, SourceLocation *loc)
{
	NonNullReturnData *d = data;

	report(loc != NULL ? loc : &d->AttrLoc, "nonnull-return: null returned");
}

void
__ubsan_handle_nonnull_return_v1_abort(void *data, SourceLocation *loc)
{
	__ubsan_handle_nonnull_return_v1(data, loc);
	die();
}

void
__ubsan_handle_nullability_return_v1(void *data, SourceLocation *loc)
{
	NonNullReturnData *d = data;

	report(loc != NULL ? loc : &d->AttrLoc, "nullability-return: null returned");
}

void
__ubsan_handle_nullability_return_v1_abort(void *data, SourceLocation *loc)
{
	__ubsan_handle_nullability_return_v1(data, loc);
	die();
}

void
__ubsan_handle_pointer_overflow(void *data, uintptr_t base, uintptr_t result)
{
	PointerOverflowData *d = data;

	report(&d->Loc, "pointer-overflow: 0x%lx -> 0x%lx",
	    (unsigned long)base, (unsigned long)result);
}

void
__ubsan_handle_pointer_overflow_abort(void *data, uintptr_t base,
    uintptr_t result)
{
	__ubsan_handle_pointer_overflow(data, base, result);
	die();
}

void
__ubsan_handle_function_type_mismatch(void *data, uintptr_t function)
{
	FunctionTypeMismatchData *d = data;

	report(&d->Loc, "function-type-mismatch: 0x%lx (%s)",
	    (unsigned long)function, typename_of(d->Type));
}

void
__ubsan_handle_function_type_mismatch_abort(void *data, uintptr_t function)
{
	__ubsan_handle_function_type_mismatch(data, function);
	die();
}

void
__ubsan_handle_dynamic_type_cache_miss(void *data, uintptr_t obj,
    uintptr_t hash)
{
	DynamicTypeCacheMissData *d = data;

	report(&d->Loc, "dynamic-type-cache-miss: 0x%lx hash 0x%lx (%s)",
	    (unsigned long)obj, (unsigned long)hash, typename_of(d->Type));
}

void
__ubsan_handle_dynamic_type_cache_miss_abort(void *data, uintptr_t obj,
    uintptr_t hash)
{
	__ubsan_handle_dynamic_type_cache_miss(data, obj, hash);
	die();
}

void
__ubsan_handle_cfi_check_fail(void *data, uintptr_t function,
    uintptr_t vtable)
{
	CFICheckFailData *d = data;

	report(&d->Loc, "cfi-check-fail (kind %u): function 0x%lx vtable 0x%lx (%s)",
	    d->CheckKind, (unsigned long)function, (unsigned long)vtable,
	    typename_of(d->Type));
}

void
__ubsan_handle_cfi_check_fail_abort(void *data, uintptr_t function,
    uintptr_t vtable)
{
	__ubsan_handle_cfi_check_fail(data, function, vtable);
	die();
}

void
__ubsan_handle_cfi_bad_type(void *data, uintptr_t vtable, uintptr_t function)
{
	CFICheckFailData *d = data;

	report(&d->Loc, "cfi-bad-type: vtable 0x%lx function 0x%lx (%s)",
	    (unsigned long)vtable, (unsigned long)function, typename_of(d->Type));
}

void
__ubsan_handle_builtin_unreachable(void *data)
{
	UnreachableData *d = data;

	report(&d->Loc, "builtin-unreachable");
	die();
}

void
__ubsan_handle_missing_return(void *data)
{
	UnreachableData *d = data;

	report(&d->Loc, "missing-return");
	die();
}
