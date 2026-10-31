#include <u.h>
#include <libc.h>
#include <mman.h>

void
main(int argc, char *argv[])
{
	char *p;
	usize sz = 2 * 1024 * 1024; /* 2MB (BIGPGSZ) */
	int i;

	print("=== Running Harvey mmap test ===\n");

	/* 1. Test anonymous mmap */
	p = mmap(nil, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if(p == (void*)-1 || p == nil){
		fprint(2, "mmap anonymous failed: %r\n");
		exits("mmap fail");
	}
	print("1. Anonymous mmap succeeded at %p (size %ld)\n", p, sz);

	/* 2. Verify demand-zeroing */
	for(i = 0; i < 4096; i++){
		if(p[i] != 0){
			fprint(2, "mmap byte %d not zero: %d\n", i, p[i]);
			exits("nonzero");
		}
	}
	print("2. Demand zero-fill verified\n");

	/* 3. Test writing and reading memory */
	strcpy(p, "Harvey OS Native mmap is working!");
	if(strcmp(p, "Harvey OS Native mmap is working!") != 0){
		fprint(2, "mmap memory readback mismatch\n");
		exits("mismatch");
	}
	print("3. Write/read verified: \"%s\"\n", p);

	/* 4. Test mprotect */
	if(mprotect(p, sz, PROT_READ) < 0){
		fprint(2, "mprotect failed: %r\n");
		exits("mprotect fail");
	}
	print("4. mprotect(PROT_READ) succeeded\n");

	/* 5. Test munmap */
	if(munmap(p, sz) < 0){
		fprint(2, "munmap failed: %r\n");
		exits("munmap fail");
	}
	print("5. munmap succeeded\n");

	print("=== All mmap tests passed! ===\n");
	exits(nil);
}
