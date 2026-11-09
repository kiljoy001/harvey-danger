/*
 * Multicall shim for the Harvey/ARM64 SDK.
 *
 * Plan 9 has no symbolic links, so the SDK cannot install the usual
 * llvm-foo -> llvm links.  Instead every tool name is a tiny copy of this
 * program.  It calls the real multicall driver with an explicit subcommand
 * (`llvm nm ...`), which is the driver's documented dispatch path; relying
 * on argv[0] does not survive the driver's own cc1 re-exec.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDK_ROOT
#define SDK_ROOT "/initramfs/sdk"
#endif

int
main(int argc, char **argv)
{
	char path[256], **av, *base;
	const char *sub;
	int i;

	base = strrchr(argv[0], '/');
	base = base ? base + 1 : argv[0];
	sub = strncmp(base, "llvm-", 5) == 0 ? base + 5 : base;

	av = malloc((argc + 2) * sizeof(char *));
	if(av == NULL){
		fprintf(stderr, "%s: out of memory\n", argv[0]);
		return 127;
	}
	av[1] = (char *)sub;
	for(i = 1; i < argc; i++)
		av[i + 1] = argv[i];
	av[argc + 1] = NULL;
	snprintf(path, sizeof path, "%s/bin/llvm", SDK_ROOT);
	av[0] = path;
	execv(path, av);
	fprintf(stderr, "%s: cannot exec %s\n", argv[0], path);
	return 127;
}
