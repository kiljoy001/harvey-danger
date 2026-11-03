/*
 * Assemble and disassemble inside the guest: build llvm-as and llvm-dis,
 * turn test.ll into test.bc, then test.bc back into roundtrip.ll.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int run(char *const av[]) {
    int pid = fork();
    if (pid == 0) { execv(av[0], av); perror(av[0]); _exit(127); }
    if (pid < 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

int main(void) {
    if (chdir("/n/host/build/llvm-guest") < 0) { perror("chdir"); return 1; }
    setenv("TMPDIR", "/n/host/build/llvm-guest/tmp", 1);

    char *asbuild[] = { "/n/host/build/llvm-guest/hbuild", "-j", "4",
                        "-f", "llvm-as.build", 0 };
    fprintf(stderr, "== hbuild llvm-as.build\n");
    int st = run(asbuild);
    fprintf(stderr, "== llvm-as build status %d\n", st);
    if (st) return 1;

    char *as[] = { "/n/host/build/llvm-guest/bin/llvm-as",
                   "test.ll", "-o", "test.bc", 0 };
    st = run(as);
    fprintf(stderr, "== llvm-as run status %d\n", st);
    if (st) return 1;

    char *disbuild[] = { "/n/host/build/llvm-guest/hbuild", "-j", "4",
                         "-f", "llvm-dis.build", 0 };
    fprintf(stderr, "== hbuild llvm-dis.build\n");
    st = run(disbuild);
    fprintf(stderr, "== llvm-dis build status %d\n", st);
    if (st) return 1;

    char *dis[] = { "/n/host/build/llvm-guest/bin/llvm-dis",
                    "test.bc", "-o", "roundtrip.ll", 0 };
    st = run(dis);
    fprintf(stderr, "== llvm-dis run status %d\n", st);
    return st;
}
