/* fmt library probe: compile format.cc separately, archive it, link. */
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
    std::printf("FMTLIB-DONE\n");
    return 0;
}
