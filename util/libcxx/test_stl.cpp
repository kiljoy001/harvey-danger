#include <string>
#include <vector>
#include <map>
#include <cstdio>

int main() {
    std::string s = "hello";
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    std::map<std::string, int> m;
    m[s] = (int)v.size();
    std::string n = std::to_string(40 + 2);
    int x = std::stoi("41") + 1;
    std::printf("%s %zu %d %s %d\n", s.c_str(), v.size(), m[s], n.c_str(), x);
    return 0;
}
