/* fmt library probe, built by the guest's own clang++. */
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>

int main() {
    open("#c/cons", 0);
    open("#c/cons", 1);
    open("#c/cons", 1);
    std::vector<int> v{1, 2, 3};
    std::string s = fmt::format("{} {:03d} {:.2f} {}", "harvey", 42, 3.5, v);
    fmt::print("{}\n", s);
    std::printf("FMT-DONE\n");
    return 0;
}
