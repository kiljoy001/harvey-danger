// Chrono durations and a fixed time point.
#include <chrono>
#include <cstdio>
#include <ctime>

int main() {
    using namespace std::chrono;
    auto d = milliseconds(3723456);
    std::printf("ms=%lld s=%lld\n", static_cast<long long>(d.count()),
                static_cast<long long>(duration_cast<seconds>(d).count()));
    std::printf("mod=%lld\n", static_cast<long long>((d % 1000).count()));

    auto start = steady_clock::now();
    long spin = 0;
    for (int i = 0; i < 200000; i++)
        spin += i;
    auto ns = duration_cast<nanoseconds>(steady_clock::now() - start).count();
    std::printf("spin=%ld elapsed_positive=%d\n", spin, ns > 0);

    auto epoch = system_clock::time_point(seconds(1700000000));
    std::time_t t = system_clock::to_time_t(epoch);
    std::tm *g = std::gmtime(&t);
    std::printf("utc=%04d-%02d-%02d %02d:%02d:%02d\n", g->tm_year + 1900,
                g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
    return 0;
}
