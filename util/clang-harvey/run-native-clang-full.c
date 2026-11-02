/*
 * The whole native toolchain, with no flags at all:
 *
 *     clang++ hello.cpp -o hello
 *
 * The driver loads clang++.cfg from its own directory, finds its resource
 * headers relative to itself, runs cc1, and invokes the ld.lld sitting next
 * to it.  Then the result runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    if (chdir("/n/host/build/clang-harvey") < 0) {
        perror("chdir");
        return 1;
    }

    int pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        char *argv[] = {
            "/n/host/build/clang-harvey/clang++",
            "hello.cpp", "-o", "hello",
            0,
        };
        execv(argv[0], argv);
        perror("execv clang++");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || status != 0) {
        fprintf(stderr, "clang++ failed, status %d\n", status);
        return 1;
    }

    char *hello[] = {"/n/host/build/clang-harvey/hello", 0};
    execv(hello[0], hello);
    perror("execv hello");
    return 1;
}
