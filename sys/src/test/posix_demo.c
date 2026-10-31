#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv) {
    printf("Hello from POSIX C world on Harvey OS!\n");
    printf("Compiled via APEX + Tiny C Compiler (TCC)!\n");
    void *ptr = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (ptr != MAP_FAILED) {
        snprintf((char*)ptr, 4096, "mmap inside POSIX test works!");
        printf("mmap string: %s\n", (char*)ptr);
        munmap(ptr, 4096);
    }
    return 0;
}
