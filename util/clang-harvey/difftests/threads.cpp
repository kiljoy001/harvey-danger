// Threads: mutex, condition_variable, async, thread_local, atomics.
#include <atomic>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

static std::mutex mtx;
static std::condition_variable cv;
static int turn = 0;
static long shared = 0;
static thread_local int tls_counter = 0;

int main() {
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++)
        ts.emplace_back([] {
            long local = 0;
            for (int i = 0; i < 20000; i++)
                local += i;
            tls_counter = local;
            std::lock_guard<std::mutex> l(mtx);
            shared += local;
        });
    for (auto &t : ts)
        t.join();
    std::printf("shared=%ld tls=%d\n", shared, tls_counter);

    const int rounds = 50;
    int counter = 0;
    std::thread a([&] {
        for (int i = 0; i < rounds; i++) {
            std::unique_lock<std::mutex> l(mtx);
            cv.wait(l, [&] { return turn == 0; });
            counter++;
            turn = 1;
            cv.notify_one();
        }
    });
    std::thread b([&] {
        for (int i = 0; i < rounds; i++) {
            std::unique_lock<std::mutex> l(mtx);
            cv.wait(l, [&] { return turn == 1; });
            counter++;
            turn = 0;
            cv.notify_one();
        }
    });
    a.join();
    b.join();
    std::printf("counter=%d\n", counter);

    std::atomic<long> total{0};
    std::vector<std::future<long>> fs;
    for (int k = 1; k <= 6; k++)
        fs.push_back(std::async(std::launch::async, [k, &total] {
            long s = 0;
            for (int i = 1; i <= k * 1000; i++)
                s += i;
            total += s;
            return s;
        }));
    long sum = 0;
    for (auto &f : fs)
        sum += f.get();
    std::printf("async=%ld total=%ld\n", sum, total.load());
    return 0;
}
