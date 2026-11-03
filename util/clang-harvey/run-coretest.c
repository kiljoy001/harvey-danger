/*
 * Build LLVMCore in the guest, then build and run a program linked against
 * it.  Both steps are hbuild manifests.
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

    char *core[] = { "/n/host/build/llvm-guest/hbuild", "-j", "4",
                     "-f", "LLVMCore.build", 0 };
    fprintf(stderr, "== hbuild LLVMCore.build\n");
    int st = run(core);
    fprintf(stderr, "== LLVMCore status %d\n", st);
    if (st) return 1;

    char *test[] = { "/n/host/build/llvm-guest/hbuild", "-j", "2",
                     "-f", "coretest.build", 0 };
    fprintf(stderr, "== hbuild coretest.build\n");
    st = run(test);
    fprintf(stderr, "== coretest build status %d\n", st);
    if (st) return 1;

    char *app[] = { "/n/host/build/llvm-guest/bin/coretest", 0 };
    execv(app[0], app);
    perror("execv coretest");
    return 1;
}
