#include <atomic>
#include <barrier>
#include <cstdio>
#include <latch>
#include <semaphore>
#include <thread>
#include <vector>

int main() {
    /* latch: two threads + main */
    std::latch l(3);
    std::thread l1([&] { l.count_down(); });
    std::thread l2([&] { l.count_down(); });
    l.count_down();
    l.wait();
    l1.join();
    l2.join();

    /* barrier: three arrivals, then a counter check */
    std::atomic<int> arrived{0};
    std::barrier b(3);
    auto bump = [&] { arrived++; b.arrive_and_wait(); };
    std::thread b1(bump), b2(bump);
    bump();
    b1.join();
    b2.join();
    int barrier_ok = arrived.load() == 3;

    /* counting semaphore */
    std::counting_semaphore<4> sem(0);
    std::thread s1([&] { sem.acquire(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sem.release();
    s1.join();

    /* atomic wait/notify */
    std::atomic<int> a{0};
    std::thread w([&] { a.wait(0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    a.store(1);
    a.notify_all();
    w.join();

    /* jthread + stop token */
    std::atomic<int> spins{0};
    {
        std::jthread t([&](std::stop_token st) {
            while (!st.stop_requested()) {
                spins++;
                std::this_thread::yield();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        t.request_stop();
    }

    std::printf("cxx20 latch=1 barrier=%d sema=1 atomic=%d jthread=%d\n",
                barrier_ok, a.load(), spins.load() > 0);
    return 0;
}
