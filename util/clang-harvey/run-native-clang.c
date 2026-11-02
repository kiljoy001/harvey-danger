/*
 * Boot harness: run the Harvey-native clang on a fixed input.
 *
 * A 9vm guest starts the -boot program as PID 1 with no argv, so this tiny
 * program supplies the command line clang needs.  It also keeps the test
 * simple to reproduce: build, boot, check build/clang-harvey/test.o.
 */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    if (chdir("/n/host/build/clang-harvey") < 0) {
        perror("chdir");
        return 1;
    }

    char *argv[] = {
        "clang",
        "-cc1",
        "-triple", "x86_64-unknown-elf",
        "-emit-obj",
        "-o", "test.o",
        "test.c",
        0,
    };

    execv("/n/host/build/clang-harvey/clang", argv);
    perror("execv clang");
    return 1;
}
