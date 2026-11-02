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

    /* Use the real driver, not -cc1: this also exercises fork/exec of the
     * cc1 process inside the guest. */
    char *argv[] = {
        "clang",
        "-target", "x86_64-unknown-elf",
        "-c",
        "test.c",
        "-o", "test.o",
        0,
    };

    execv("/n/host/build/clang-harvey/clang", argv);
    perror("execv clang");
    return 1;
}
