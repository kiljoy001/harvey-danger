/*
 * Run a manifest of raw compiler commands.  Unlike run-hbuild.c the driver
 * config stays enabled here, because these commands are the guest's own:
 * they rely on the installed clang++.cfg for the target and include paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    if (chdir("/n/host/build/llvm-guest") < 0) { perror("chdir"); return 1; }
    setenv("TMPDIR", "/n/host/build/llvm-guest/tmp", 1);

    char *av[] = { "/n/host/build/llvm-guest/hbuild", "-j", "4",
                   "-f", "diff.build", 0 };
    int pid = fork();
    if (pid == 0) { execv(av[0], av); perror(av[0]); _exit(127); }
    int st = 0;
    waitpid(pid, &st, 0);
    fprintf(stderr, "== hbuild status %d\n", st);
    return st;
}
