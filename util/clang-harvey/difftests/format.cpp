// Number formatting and parsing, including floating point.
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

int main() {
    std::printf("int %d %u %ld %llu %x %o %e %g\n", -42, 42u, -42L,
                42ull, 255, 8, 1.5, 0.001);

    char buf[64];
    std::to_chars_result r = std::to_chars(buf, buf + sizeof buf, 3.14159265358979);
    *r.ptr = 0;
    std::printf("chars=%s\n", buf);
    r = std::to_chars(buf, buf + sizeof buf, 1234567890123LL);
    *r.ptr = 0;
    std::printf("chars_int=%s\n", buf);

    double d = 0;
    std::from_chars("2.718281828", buf + 10, d);
    std::printf("from_chars=%.9f\n", d);
    int i = 0;
    std::from_chars("1234567", buf + 10, i);
    std::printf("from_chars_int=%d\n", i);

    std::ostringstream os;
    os << std::setprecision(6) << std::fixed << 1.0 / 3.0 << " "
       << std::scientific << 12345.678 << " " << std::setw(8) << 42;
    std::printf("stream=%s\n", os.str().c_str());

    std::printf("hexfloat=%a\n", 1.0 / 3.0);

    std::cout << 1 << ' ' << 2.5 << ' ' << true << ' ' << -7LL << '\n';
    std::cout << std::hex << 255 << std::dec << ' ' << 42 << '\n';

    double sum = 0;
    for (int k = 1; k <= 100; k++)
        sum += 1.0 / k;
    std::printf("harmonic=%.15f\n", sum);
    return 0;
}
