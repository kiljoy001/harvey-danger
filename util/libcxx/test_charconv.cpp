#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>

int main() {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, 3.14159);
    *r.ptr = 0;

    double d = 0;
    auto p = std::from_chars(buf, r.ptr, d);

    std::string s = std::format("{} {} {:.2f}", 42, "harvey", 3.14159);

    char ib[16];
    auto ir = std::to_chars(ib, ib + sizeof ib, 12345);
    *ir.ptr = 0;
    long v = 0;
    std::from_chars(ib, ir.ptr, v);

    std::printf("charconv float=%s parsed=%.5f err=%d int=%s/%ld format=%s\n",
                buf, d, (int)p.ec, ib, v, s.c_str());
    return 0;
}
