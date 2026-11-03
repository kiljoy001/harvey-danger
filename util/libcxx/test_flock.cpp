#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    FILE *f = stdout;
    std::atomic<int> stage{0}, busy{0};

    std::thread t([&] {
        while (stage.load() < 1)
            std::this_thread::yield();
        if (ftrylockfile(f) != 0)
            busy = 1;
        else
            funlockfile(f);
    });

    flockfile(f);
    flockfile(f);          /* nesting keeps the lock held */
    stage = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::printf("flock inside=1\n");
    funlockfile(f);        /* still held */
    funlockfile(f);        /* released */

    t.join();

    if (ftrylockfile(f) == 0)
        funlockfile(f);
    else
        busy = 2;

    std::printf("flock busy=%d\n", busy.load());
    return busy.load() == 1 ? 0 : 1;
}
