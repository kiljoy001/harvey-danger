/*
 * Minimal AddressSanitizer runtime for Harvey/ARM64.
 *
 * This is a user-space, POSIX/APEX runtime for the checks clang emits with
 * -fsanitize=address.  It does NOT install a SIGSEGV handler and does not
 * use compiler-rt's Linux syscall layer: the Harvey ARM64 kernel currently
 * turns an unhandled user fault into pexit("user fault"), so a signal-based
 * wild-pointer report is not possible without changing the kernel.  The
 * compiler-instrumented redzone and use-after-free checks do not need a
 * signal and work through the shadow below.
 *
 * Layout (matches clang's AArch64 shadow offset, 1<<36 = 64 GiB):
 *
 *   16 GiB   program text/data
 *   64 GiB   shadow (64 GiB, covering 0..512 GiB of program addresses)
 *   128 GiB  heap arena (64 GiB)
 *   512 GiB  stack
 *
 * The heap starts after the shadow, so no program or heap page overlaps the
 * shadow.  The APEX mmap arena lives near the stack and is outside both.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

typedef uintptr_t uptr;
typedef uint8_t u8;

#define SHADOW_BASE	0x1000000000ULL
#define SHADOW_SIZE	0x1000000000ULL
#define HEAP_BASE	0x2000000000ULL
#define HEAP_SIZE	0x1000000000ULL
#define HEAP_END	(HEAP_BASE + HEAP_SIZE)

#define REDZONE		64
#define HDR_SIZE	16
#define HDR_MAGIC	0x4153414e48445231ULL

enum {
	HeapLeftRedzone	= 0xfa,
	HeapRightRedzone = 0xfb,
	HeapFreed	= 0xfd,
	GlobalRedzone	= 0xf9,
	StackRedzone	= 0xf1,
	StackAfterReturn = 0xf8,
};

struct Header {
	uptr size;
	uptr magic;
};

struct Global {
	uptr beg;
	uptr size;
	uptr size_with_redzone;
	const char *name;
	const char *module_name;
	uptr has_dynamic_init;
	void *gcc_location;
	uptr odr_indicator;
};

static uptr heaptop;
static int initialized;

static u8 *
shadowp(uptr a)
{
	return (u8 *)(SHADOW_BASE + (a >> 3));
}

static void
poison_range(uptr addr, uptr size, u8 magic)
{
	uptr a, end;

	if(size == 0)
		return;
	end = addr + size;
	for(a = addr & ~7ULL; a < end; a += 8)
		*shadowp(a) = magic;
}

static void
unpoison_range(uptr addr, uptr size)
{
	uptr a, end, rem;

	if(size == 0)
		return;
	end = addr + size;
	for(a = addr & ~7ULL; a + 8 <= end; a += 8)
		*shadowp(a) = 0;
	rem = end - a;
	if(rem != 0)
		*shadowp(a) = (u8)rem;
}

static void
init(void)
{
	void *p;

	if(initialized)
		return;
	initialized = 1;
	p = mmap((void *)SHADOW_BASE, SHADOW_SIZE, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
	if(p == (void *)-1){
		static const char msg[] = "asan: cannot map shadow\n";
		write(2, msg, sizeof msg - 1);
		_exit(1);
	}
	p = mmap((void *)HEAP_BASE, HEAP_SIZE, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
	if(p == (void *)-1){
		static const char msg[] = "asan: cannot map heap\n";
		write(2, msg, sizeof msg - 1);
		_exit(1);
	}
	heaptop = HEAP_BASE;
}

static void
report(uptr addr, int fatal)
{
	u8 v;
	const char *kind;
	char buf[160];
	int n;

	v = *shadowp(addr);
	switch(v){
	case HeapLeftRedzone:
	case HeapRightRedzone:
		kind = "heap-buffer-overflow";
		break;
	case HeapFreed:
		kind = "heap-use-after-free";
		break;
	case GlobalRedzone:
		kind = "global-buffer-overflow";
		break;
	case StackRedzone:
		kind = "stack-buffer-overflow";
		break;
	case StackAfterReturn:
		kind = "stack-use-after-return";
		break;
	default:
		if(addr >= HEAP_BASE && addr < HEAP_END)
			kind = "heap-buffer-overflow";
		else
			kind = "unknown-crash";
		break;
	}
	n = snprintf(buf, sizeof buf,
		"AddressSanitizer: %s at 0x%lx (shadow 0x%x)\n",
		kind, (unsigned long)addr, v);
	if(n > 0)
		write(2, buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
	if(fatal)
		abort();
}

#define REPORT(name) \
	void __asan_report_##name(uptr addr) { report(addr, 1); } \
	void __asan_report_##name##_noabort(uptr addr) { report(addr, 0); }

REPORT(load1)
REPORT(load2)
REPORT(load4)
REPORT(load8)
REPORT(load16)
REPORT(store1)
REPORT(store2)
REPORT(store4)
REPORT(store8)
REPORT(store16)

void
__asan_report_load_n(uptr addr, uptr size)
{
	(void)size;
	report(addr, 1);
}

void
__asan_report_load_n_noabort(uptr addr, uptr size)
{
	(void)size;
	report(addr, 0);
}

void
__asan_report_store_n(uptr addr, uptr size)
{
	(void)size;
	report(addr, 1);
}

void
__asan_report_store_n_noabort(uptr addr, uptr size)
{
	(void)size;
	report(addr, 0);
}

void
__asan_init(void)
{
	init();
}

void
__asan_version_mismatch_check_v8(void)
{
}

int __asan_option_detect_stack_use_after_return;

void *
__asan_stack_malloc_0(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_1(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_2(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_3(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_4(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_5(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_6(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_7(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_8(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_9(uptr size) { (void)size; return NULL; }
void *
__asan_stack_malloc_10(uptr size) { (void)size; return NULL; }

void
__asan_stack_free_0(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_1(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_2(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_3(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_4(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_5(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_6(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_7(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_8(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_9(void *p, uptr size) { (void)p; (void)size; }
void
__asan_stack_free_10(void *p, uptr size) { (void)p; (void)size; }

void
__asan_alloca_poison(uptr addr, uptr size)
{
	(void)addr;
	(void)size;
}

void
__asan_allocas_unpoison(uptr start, uptr end)
{
	(void)start;
	(void)end;
}

void
__asan_handle_no_return(void)
{
}

void
__asan_poison_stack_memory(uptr addr, uptr size)
{
	poison_range(addr, size, StackRedzone);
}

void
__asan_unpoison_stack_memory(uptr addr, uptr size)
{
	unpoison_range(addr, size);
}

void *
__asan_memcpy(void *dst, const void *src, uptr size)
{
	return memcpy(dst, src, size);
}

void *
__asan_memmove(void *dst, const void *src, uptr size)
{
	return memmove(dst, src, size);
}

void *
__asan_memset(void *dst, int c, uptr size)
{
	return memset(dst, c, size);
}

void
__asan_register_globals(struct Global *g, uptr n)
{
	uptr i;

	for(i = 0; i < n; i++){
		unpoison_range(g[i].beg, g[i].size);
		if(g[i].size_with_redzone > g[i].size)
			poison_range((g[i].beg + g[i].size + 7) & ~7ULL,
				g[i].size_with_redzone - g[i].size,
				GlobalRedzone);
	}
}

void
__asan_unregister_globals(struct Global *g, uptr n)
{
	(void)g;
	(void)n;
}

void
__asan_register_elf_globals(uptr *flag, void *start, void *stop)
{
	uptr n;

	if(flag != NULL && *flag != 0)
		return;
	n = ((char *)stop - (char *)start) / sizeof(struct Global);
	__asan_register_globals((struct Global *)start, n);
	if(flag != NULL)
		*flag = 1;
}

void
__asan_unregister_elf_globals(uptr *flag, void *start, void *stop)
{
	(void)flag;
	(void)start;
	(void)stop;
}

void
__asan_register_image_globals(uptr *flag)
{
	(void)flag;
}

void
__asan_unregister_image_globals(uptr *flag)
{
	(void)flag;
}

int
__asan_address_is_poisoned(const void *addr)
{
	return *shadowp((uptr)addr) != 0;
}

void *
__asan_region_is_poisoned(void *beg, uptr size)
{
	uptr a, end;

	end = (uptr)beg + size;
	for(a = (uptr)beg; a < end; a += 8)
		if(*shadowp(a) != 0)
			return (void *)a;
	return NULL;
}

void
__asan_describe_address(void *addr)
{
	(void)addr;
}

void
__asan_set_error_report_callback(void (*cb)(const char *))
{
	(void)cb;
}

void
__asan_on_error(void)
{
}

void
__sanitizer_annotate_contiguous_container(const void *beg, const void *end,
    const void *old_mid, const void *new_mid)
{
	(void)beg;
	(void)end;
	(void)old_mid;
	(void)new_mid;
}

void *
malloc(size_t size)
{
	uptr user, start, total;
	struct Header *h;

	init();
	if(size == 0)
		size = 1;
	start = heaptop;
	user = (start + REDZONE + HDR_SIZE + 15) & ~15ULL;
	total = user + size + REDZONE;
	if(total > HEAP_END || total < user)
		return NULL;
	h = (struct Header *)(user - HDR_SIZE);
	h->size = size;
	h->magic = HDR_MAGIC;
	poison_range(start, user - start, HeapLeftRedzone);
	unpoison_range(user, size);
	poison_range((user + size + 7) & ~7ULL, REDZONE, HeapRightRedzone);
	heaptop = (total + 7) & ~7ULL;
	return (void *)user;
}

void
free(void *p)
{
	struct Header *h;
	uptr user = (uptr)p;

	if(p == NULL)
		return;
	if(user < HEAP_BASE || user >= HEAP_END){
		/* Not ours; nothing sensible to do in this minimal runtime. */
		return;
	}
	h = (struct Header *)(user - HDR_SIZE);
	if(h->magic != HDR_MAGIC)
		return;
	poison_range(user, h->size, HeapFreed);
	h->magic = 0;
}

void *
calloc(size_t n, size_t size)
{
	uptr total;
	void *p;

	if(n != 0 && size > (size_t)-1 / n)
		return NULL;
	total = n * size;
	p = malloc(total);
	if(p != NULL)
		memset(p, 0, total);
	return p;
}

void *
realloc(void *p, size_t size)
{
	struct Header *h;
	void *q;
	size_t old;

	if(p == NULL)
		return malloc(size);
	if(size == 0){
		free(p);
		return NULL;
	}
	h = (struct Header *)((uptr)p - HDR_SIZE);
	if(h->magic != HDR_MAGIC)
		return NULL;
	old = h->size;
	q = malloc(size);
	if(q == NULL)
		return NULL;
	memcpy(q, p, old < size ? old : size);
	free(p);
	return q;
}

__attribute__((weak)) void *
memalign(size_t align, size_t size)
{
	uptr user, start, total;
	struct Header *h;

	if(align < 16)
		align = 16;
	init();
	if(size == 0)
		size = 1;
	start = heaptop;
	user = (start + REDZONE + HDR_SIZE + align - 1) & ~(uptr)(align - 1);
	total = user + size + REDZONE;
	if(total > HEAP_END || total < user)
		return NULL;
	h = (struct Header *)(user - HDR_SIZE);
	h->size = size;
	h->magic = HDR_MAGIC;
	poison_range(start, user - start, HeapLeftRedzone);
	unpoison_range(user, size);
	poison_range((user + size + 7) & ~7ULL, REDZONE, HeapRightRedzone);
	heaptop = (total + 7) & ~7ULL;
	return (void *)user;
}

__attribute__((weak)) int
posix_memalign(void **p, size_t align, size_t size)
{
	void *q;

	if(align < sizeof(void *) || (align & (align - 1)) != 0)
		return 22; /* EINVAL */
	q = memalign(align, size);
	if(q == NULL)
		return 12; /* ENOMEM */
	*p = q;
	return 0;
}

__attribute__((weak)) void *
aligned_alloc(size_t align, size_t size)
{
	return memalign(align, size);
}

__attribute__((weak)) void *
valloc(size_t size)
{
	return memalign(4096, size);
}

const char *
__asan_default_options(void)
{
	return "handle_segv=0:detect_stack_use_after_return=0";
}
