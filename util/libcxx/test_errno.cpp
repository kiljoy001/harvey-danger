/*
 * errno is per thread.  Threads here are rfork(RFMEM) processes sharing data
 * and bss, so a plain global would let a failing syscall in one thread
 * clobber another's error.  Each thread parks with its own errno, waits for
 * the others, then performs a failing open(): the value it set and the value
 * the failure produced must both stay local, and the main thread's must be
 * untouched.
 */
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <thread>
#include <unistd.h>

static std::atomic<int> parked{0};
static std::atomic<int> failed{0};
static std::atomic<int> saw_a{0}, after_a{0};
static std::atomic<int> saw_b{0}, after_b{0};

static void worker(int value, std::atomic<int> &saw, std::atomic<int> &after) {
    errno = value;
    parked++;
    while (parked.load() < 3)
        std::this_thread::yield();

    saw = errno;

    int fd = open("/no-such-file-errno-test", O_RDONLY);
    after = errno;
    if (fd >= 0)
        close(fd);

    failed++;
}

int main() {
    std::thread a(worker, 11, std::ref(saw_a), std::ref(after_a));
    std::thread b(worker, 22, std::ref(saw_b), std::ref(after_b));

    errno = 33;
    parked++;
    while (parked.load() < 3)
        std::this_thread::yield();
    while (failed.load() < 2)
        std::this_thread::yield();

    int mine = errno;
    a.join();
    b.join();

    std::printf("errno-ok main=%d a=%d/%d b=%d/%d\n",
                mine, saw_a.load(), after_a.load(), saw_b.load(), after_b.load());

    return mine == 33 && saw_a == 11 && after_a == ENOENT &&
                   saw_b == 22 && after_b == ENOENT
               ? 0
               : 1;
}
