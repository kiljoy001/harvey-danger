/*
 * Run a staged build inside the guest with hbuild, then the tool it built.
 *
 *     run-selfbuild <manifest> [args to the built tool...]
 *
 * The default manifest is llvm-tblgen.build and the default tool is
 * bin/llvm-tblgen --version.
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

int main(int argc, char **argv) {
    const char *dir = "/n/host/build/llvm-guest";
    if (chdir(dir) < 0) { perror("chdir"); return 1; }
    setenv("TMPDIR", "/n/host/build/llvm-guest/tmp", 1);

    char *hb[] = { "/n/host/build/llvm-guest/hbuild", "-j", "1",
                   "-f", argc > 1 ? argv[1] : "llvm-tblgen.build", 0 };
    fprintf(stderr, "== hbuild %s\n", hb[4]);
    int st = run(hb);
    fprintf(stderr, "== hbuild status %d\n", st);
    if (st) return 1;

    char *tool[] = { "/n/host/build/llvm-guest/bin/llvm-tblgen",
                     "--version", 0 };
    execv(tool[0], tool);
    perror("execv llvm-tblgen");
    return 1;
}
