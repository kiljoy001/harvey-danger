// C++ runtime probe for Harvey/ARM64 APEX + libc++.
// Each feature prints one `CXXPROBE <name> OK` line; test-cxx.py requires
// all of them plus CXXPROBE-DONE.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <exception>
#include <memory>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <locale>
#include <regex>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <random>
#include <filesystem>
#include <format>
#include <charconv>
#include <fcntl.h>
#include <unistd.h>
#include <latch>
#include <barrier>
#include <semaphore>
#include <ranges>
#include <algorithm>
#include <source_location>
#include <span>
#include <tuple>
#include <optional>
#include <variant>
#include <fstream>
#include <print>

static void ok(const char *n, const char *d = "") {
    std::printf("CXXPROBE %-24s OK %s\n", n, d);
}
static void bad(const char *n, const char *d = "") {
    std::printf("CXXPROBE %-24s FAIL %s\n", n, d);
}

int main() {
    /* The kernel starts the first process with no descriptors. */
    open("#c/cons", 0);
    open("#c/cons", 1);
    open("#c/cons", 1);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    try {
        std::string s = "hello";
        std::vector<int> v{3, 1, 2};
        std::map<std::string, int> m{{"a", 1}};
        m["b"] = 2;
        if (s == "hello" && v.size() == 3 && m.size() == 2)
            ok("containers");
        else
            bad("containers");
    } catch (...) { bad("containers", "exception"); }

    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        if (std::strcmp(e.what(), "boom") == 0) ok("exceptions");
        else bad("exceptions", e.what());
    } catch (...) { bad("exceptions", "wrong type"); }

    try {
        std::exception_ptr ep;
        try { throw 42; } catch (...) { ep = std::current_exception(); }
        try { std::rethrow_exception(ep); }
        catch (int x) { if (x == 42) ok("exception_ptr"); else bad("exception_ptr"); }
    } catch (...) { bad("exception_ptr"); }

    try {
        std::ostringstream os;
        os << "n=" << 42 << " hex=" << std::hex << 255 << " pad="
           << std::setw(4) << std::setfill('0') << 7;
        if (os.str() == "n=42 hex=ff pad=0007") ok("iostream");
        else bad("iostream", os.str().c_str());
    } catch (...) { bad("iostream"); }

    try {
        std::cout << "CXXPROBE cout-line OK\n";
        std::cout.flush();
    } catch (...) { bad("cout"); }

    try {
        std::locale loc("C");
        std::ostringstream os;
        os.imbue(loc);
        os << 1234.5;
        std::ostringstream os2;
        os2.imbue(std::locale::classic());
        os2 << std::fixed << std::setprecision(2) << 1234.5;
        if (os2.str() == "1234.50") ok("locale", os.str().c_str());
        else bad("locale", os2.str().c_str());
    } catch (const std::exception &e) { bad("locale", e.what()); }
    catch (...) { bad("locale"); }

    try {
        std::regex re("(a+)(b+)");
        std::smatch m;
        std::string in = "xxaaabbbzz";
        if (std::regex_search(in, m, re) && m.str(1) == "aaa" && m.str(2) == "bbb")
            ok("regex");
        else
            bad("regex");
        std::string out = std::regex_replace(in, re, "$2-$1");
        if (out == "xxbbb-aaazz") ok("regex_replace"); else bad("regex_replace", out.c_str());
    } catch (const std::exception &e) { bad("regex", e.what()); }
    catch (...) { bad("regex"); }

    try {
        auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms >= 10) ok("chrono_sleep");
        else bad("chrono_sleep");
    } catch (...) { bad("chrono_sleep"); }

    try {
        std::atomic<int> n{0};
        std::mutex mu;
        std::condition_variable cv;
        bool ready = false;
        std::thread t([&] {
            { std::lock_guard<std::mutex> g(mu); ready = true; }
            cv.notify_one();
            n.fetch_add(1);
        });
        {
            std::unique_lock<std::mutex> g(mu);
            cv.wait_for(g, std::chrono::seconds(2), [&] { return ready; });
        }
        t.join();
        if (n == 1) ok("threads"); else bad("threads");
    } catch (const std::exception &e) { bad("threads", e.what()); }
    catch (...) { bad("threads"); }

    try {
        std::mt19937 rng(1234);
        unsigned x = rng();
        if (x != 0) ok("random");
        else bad("random");
    } catch (...) { bad("random"); }

    try {
        namespace fs = std::filesystem;
        fs::path p("/");
        if (fs::exists(p) && p.is_absolute()) ok("filesystem");
        else bad("filesystem");
    } catch (const std::exception &e) { bad("filesystem", e.what()); }
    catch (...) { bad("filesystem"); }

    try {
        char b[64];
        auto r = std::to_chars(b, b + sizeof b, 3.5);
        *r.ptr = 0;
        std::string s = std::format("{} {:.2f}", 42, 3.5);
        if (std::strcmp(b, "3.5") == 0 && s == "42 3.50") ok("format_charconv");
        else bad("format_charconv", s.c_str());
    } catch (const std::exception &e) { bad("format_charconv", e.what()); }
    catch (...) { bad("format_charconv"); }

    try {
        std::shared_ptr<int> p = std::make_shared<int>(7);
        std::unique_ptr<int> q(new int(8));
        if (*p == 7 && *q == 8) ok("smart_ptr"); else bad("smart_ptr");
    } catch (...) { bad("smart_ptr"); }

    try {
        struct comma_punct : std::numpunct<char> {
            char do_thousands_sep() const override { return ','; }
            std::string do_grouping() const override { return "\3"; }
        };
        std::ostringstream os;
        os.imbue(std::locale(std::locale::classic(), new comma_punct));
        os << 1234567;
        if (os.str() == "1,234,567") ok("locale_facet");
        else bad("locale_facet", os.str().c_str());
    } catch (const std::exception &e) { bad("locale_facet", e.what()); }
    catch (...) { bad("locale_facet"); }

    try {
        std::latch l(1);
        l.count_down();
        l.wait();
        std::counting_semaphore<1> sem(0);
        sem.release();
        sem.acquire();
        ok("latch_semaphore");
    } catch (const std::exception &e) { bad("latch_semaphore", e.what()); }
    catch (...) { bad("latch_semaphore"); }

    try {
        std::barrier b(2);
        std::thread t([&] { b.arrive_and_wait(); });
        b.arrive_and_wait();
        t.join();
        std::jthread jt([] {});
        jt.join();
        if (jt.joinable()) bad("barrier_jthread"); else ok("barrier_jthread");
    } catch (const std::exception &e) { bad("barrier_jthread", e.what()); }
    catch (...) { bad("barrier_jthread"); }

    try {
        namespace fs = std::filesystem;
        fs::path p("/a/b.txt");
        if (p.filename() == "b.txt" && p.extension() == ".txt" &&
            p.parent_path() == "/a" && p.lexically_normal() == "/a/b.txt")
            ok("path_ops");
        else
            bad("path_ops", p.lexically_normal().c_str());
    } catch (const std::exception &e) { bad("path_ops", e.what()); }
    catch (...) { bad("path_ops"); }

    try {
        std::vector<int> v{3, 1, 2};
        std::ranges::sort(v);
        std::span<int> sp(v);
        auto [a, b] = std::make_tuple(1, 2);
        std::optional<int> o = 7;
        std::variant<int, const char *> var = "x";
        auto loc = std::source_location::current();
        if (v[0] == 1 && sp.size() == 3 && a == 1 && b == 2 && *o == 7 &&
            std::get<const char *>(var) != nullptr && loc.line() > 0)
            ok("ranges_span_variant");
        else
            bad("ranges_span_variant");
    } catch (const std::exception &e) { bad("ranges_span_variant", e.what()); }
    catch (...) { bad("ranges_span_variant"); }

    try {
        namespace fs = std::filesystem;
        fs::path dir("/initramfs");
        if (!fs::exists(dir)) {
            std::printf("CXXPROBE fstream_filesystem     SKIP (no writable /initramfs)\n");
        } else {
            fs::path f = dir / "cxxprobe.tmp";
            {
                std::ofstream o(f, std::ios::trunc);
                o << "hello " << 42;
            }
            std::string a;
            int b = 0;
            {
                std::ifstream in(f);
                in >> a >> b;
            }
            fs::remove(f);
            if (a == "hello" && b == 42) ok("fstream_filesystem");
            else bad("fstream_filesystem", a.c_str());
        }
    } catch (const std::exception &e) { bad("fstream_filesystem", e.what()); }
    catch (...) { bad("fstream_filesystem"); }

    try {
        std::println("CXXPROBE print                    OK");
    } catch (const std::exception &e) { bad("print", e.what()); }
    catch (...) { bad("print"); }

    std::printf("CXXPROBE-DONE\n");
    return 0;
}
