/*
 * Full native loop inside the guest: clang compiles hello.cpp to an object,
 * ld.lld links it against the Harvey runtime, and the result runs.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

static int run(char **argv) {
    int pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return 1;
    return st != 0;
}

int main(void) {
    if (chdir("/n/host/build/clang-harvey") < 0) {
        perror("chdir");
        return 1;
    }

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
        "hello.cpp", "-o", "hello.o",
        0,
    };
    if (run(cc)) {
        fprintf(stderr, "compile failed\n");
        return 1;
    }
    printf("compiled hello.o\n");

    char *ld[] = {
        "/n/host/build/clang-harvey/ld.lld", "-static",
        "-z", "max-page-size=0x200000",
        "-o", "hello.elf",
        "/n/host/apex/amd64/lib/crt0.o",
        "/n/host/apex/amd64/lib/crti.o",
        "hello.o",
        "/n/host/build/libcxx/lib/libc++.a",
        "/n/host/apex/amd64/lib/libcxxabi.a",
        "/n/host/apex/amd64/lib/libap.a",
        "/n/host/build/libc.a",
        "/n/host/apex/amd64/lib/crtn.o",
        0,
    };
    if (run(ld)) {
        fprintf(stderr, "link failed\n");
        return 1;
    }
    printf("linked hello.elf\n");

    char *hello[] = {"/n/host/build/clang-harvey/hello.elf", 0};
    execv(hello[0], hello);
    perror("execv hello.elf");
    return 1;
}
