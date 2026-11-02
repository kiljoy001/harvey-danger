#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

int main() {
    std::mutex m;
    int n = 0;
    {
        std::lock_guard<std::mutex> g(m);
        n = 7;
    }

    std::atomic<int> a{0};
    a.fetch_add(3);
    a++;

    auto t0 = std::chrono::steady_clock::now();
    auto t1 = std::chrono::steady_clock::now();
    (void)t0; (void)t1;

    std::thread th([&] { a += 10; });
    th.join();

    std::printf("threads-ok %d %d\n", n, a.load());
    return 0;
}
