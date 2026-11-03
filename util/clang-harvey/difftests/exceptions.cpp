// Exceptions, unwinding and exception_ptr.
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

struct Custom : std::runtime_error {
    int code;
    explicit Custom(int c) : std::runtime_error("custom"), code(c) {}
};

static int depth(int n) {
    if (n == 0)
        throw Custom(7);
    return depth(n - 1) + 1;
}

static void rethrower(std::exception_ptr p) {
    try {
        std::rethrow_exception(p);
    } catch (const Custom &c) {
        std::printf("rethrow code=%d what=%s\n", c.code, c.what());
    }
}

int main() {
    try {
        depth(5);
    } catch (const Custom &c) {
        std::printf("caught code=%d\n", c.code);
    }

    try {
        throw std::out_of_range("range");
    } catch (const std::logic_error &e) {
        std::printf("logic %s\n", e.what());
    }

    std::exception_ptr p;
    try {
        depth(1);
    } catch (...) {
        p = std::current_exception();
    }
    std::printf("ptr=%d\n", p != nullptr);
    rethrower(p);

    int caught = 0;
    for (int k = 0; k < 100; k++) {
        try {
            if (k % 3 == 0)
                throw std::runtime_error(std::string("s") + std::to_string(k));
        } catch (const std::exception &e) {
            caught++;
        }
    }
    std::printf("caught=%d\n", caught);

    struct Guard {
        int *n;
        ~Guard() { (*n)++; }
    };
    int unwound = 0;
    try {
        Guard g{&unwound};
        throw 42;
    } catch (int v) {
        std::printf("int=%d unwound=%d\n", v, unwound);
    }
    return 0;
}
