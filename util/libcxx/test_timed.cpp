/*
 * Sub-second sleeps and timed waits.  Harvey's sleep syscall is
 * millisecond-granular, so nanosleep/usleep round up instead of doing
 * nothing, and pthread_cond_timedwait blocks in the kernel with a timeout
 * rather than polling.
 *
 * The exact durations are not printed; only whether each stayed in its
 * expected window.
 */
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono;

int main() {
    auto t0 = steady_clock::now();
    std::this_thread::sleep_for(milliseconds(50));
    long long slept = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    int ok_sleep = slept >= 40 && slept < 500;

    std::mutex m;
    std::condition_variable cv;
    bool ready = false;

    t0 = steady_clock::now();
    bool got;
    long long waited;
    {
        std::unique_lock<std::mutex> l(m);
        got = cv.wait_for(l, milliseconds(30), [&] { return ready; });
        waited = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    }
    int ok_timeout = !got && waited >= 20 && waited < 500;

    std::thread t([&] {
        std::this_thread::sleep_for(milliseconds(20));
        {
            std::lock_guard<std::mutex> l(m);
            ready = true;
        }
        cv.notify_one();
    });

    t0 = steady_clock::now();
    {
        std::unique_lock<std::mutex> l(m);
        got = cv.wait_for(l, milliseconds(500), [&] { return ready; });
        waited = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    }
    t.join();
    int ok_signal = got && waited >= 10 && waited < 450;

    auto fut = std::async(std::launch::async, [] {
        std::this_thread::sleep_for(milliseconds(20));
        return 7;
    });
    auto early = fut.wait_for(milliseconds(5));
    int ok_future = early == std::future_status::timeout && fut.get() == 7;

    std::printf("timed sleep=%d timeout=%d signal=%d future=%d\n",
                ok_sleep, ok_timeout, ok_signal, ok_future);
    return ok_sleep && ok_timeout && ok_signal && ok_future ? 0 : 1;
}
