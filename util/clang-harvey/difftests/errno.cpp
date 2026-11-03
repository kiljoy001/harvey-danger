// errno after a missing file: ENOENT, in the main thread and in a thread.
//
// The walk of a nonexistent name is reported as Rwalk with zero qids, and
// the client used to leave the caller's error string untouched, so errno
// depended on what the process had failed at earlier.  In a fresh thread
// the string was empty and the POSIX mapping fell back to EINVAL.
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

int main() {
    errno = 0;
    int fd_main = open("/no-such-file-difftest", O_RDONLY);
    int e_main = errno;

    int e_thread = 0, fd_thread = 0;
    std::thread t([&] {
        errno = 0;
        fd_thread = open("/no-such-file-difftest", O_RDONLY);
        e_thread = errno;
    });
    t.join();

    std::printf("main=%d thread=%d\n", e_main, e_thread);

    errno = 0;
    int fd_nested = open("/no/such/directory/at/all", O_RDONLY);
    int e_nested = errno;
    std::printf("nested=%d\n", e_nested);

    return fd_main == -1 && e_main == ENOENT && fd_thread == -1 &&
                   e_thread == ENOENT && fd_nested == -1 && e_nested == ENOENT
               ? 0
               : 1;
}
