// Containers, algorithms and strings.
#include <algorithm>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

int main() {
    std::vector<int> v(1000);
    std::iota(v.begin(), v.end(), 0);
    std::reverse(v.begin(), v.end());
    std::sort(v.begin(), v.end());
    long sum = std::accumulate(v.begin(), v.end(), 0L);
    std::printf("sum=%ld first=%d last=%d\n", sum, v.front(), v.back());

    std::map<std::string, int> m;
    for (int i = 0; i < 100; i++)
        m[std::string("key") + std::to_string(i)] = i * i;
    std::printf("map=%zu at42=%d it=%zu\n", m.size(), m["key42"], m.count("nope"));

    std::unordered_map<int, std::string> u;
    for (int i = 0; i < 50; i++)
        u[i] = std::to_string(i);
    int hits = 0;
    for (int i = 0; i < 60; i++)
        if (u.find(i) != u.end())
            hits++;
    std::printf("unordered=%zu hits=%d\n", u.size(), hits);

    std::set<int> s{5, 3, 1, 4, 2, 3};
    std::string joined;
    for (int x : s)
        joined += std::to_string(x);
    std::printf("set=%s\n", joined.c_str());

    std::string big;
    for (int i = 0; i < 200; i++)
        big += static_cast<char>('a' + i % 26);
    std::replace(big.begin(), big.end(), 'm', 'M');
    std::printf("big=%zu m=%zu M=%zu sub=%s\n", big.size(),
                static_cast<size_t>(std::count(big.begin(), big.end(), 'm')),
                static_cast<size_t>(std::count(big.begin(), big.end(), 'M')),
                big.substr(10, 8).c_str());
    return 0;
}
