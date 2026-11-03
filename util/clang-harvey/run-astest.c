/*
 * Build llvm-as in the guest, then assemble a small .ll file.
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

    char *build[] = { "/n/host/build/llvm-guest/hbuild", "-j", "4",
                      "-f", "llvm-as.build", 0 };
    fprintf(stderr, "== hbuild llvm-as.build\n");
    int st = run(build);
    fprintf(stderr, "== llvm-as build status %d\n", st);
    if (st) return 1;

    char *as[] = { "/n/host/build/llvm-guest/bin/llvm-as",
                   "test.ll", "-o", "test.bc", 0 };
    execv(as[0], as);
    perror("execv llvm-as");
    return 1;
}
