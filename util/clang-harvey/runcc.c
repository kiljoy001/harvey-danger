/* Compile /n/host/build/clang-harvey/input.cpp inside the guest. */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    if (chdir("/n/host/build/clang-harvey") < 0) { perror("chdir"); return 1; }
    int pid = fork();
    if (pid == 0) {
        char *cc[] = {
            "clang", "-target", "x86_64-unknown-elf", "-c", "-nostdinc",
            "-I/n/host/build/libcxx/include/c++/v1",
            "-I/n/host/build/libcxx/include",
            "-I/n/host/apex/include",
            "-I/n/host/apex/amd64/include",
            "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
            "-D_GNU_SOURCE", "-DHARVEY", "-D_WCHAR_T",
            "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
            "-include", "/n/host/build/libcxx/include/compat.h",
            "input.cpp", "-o", "input.o",
            0,
        };
        execv("/n/host/build/clang-harvey/clang", cc);
        perror("execv");
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("child status %d\n", st);
    return 0;
}
