// Regular expressions and string searching.
#include <cstdio>
#include <regex>
#include <string>

int main() {
    std::regex word("([a-z]+)@([a-z]+)\\.com");
    std::string text = "write to alice@example.com or bob@test.com";
    int n = 0;
    std::string first;
    for (std::sregex_iterator it(text.begin(), text.end(), word), end;
         it != end; ++it) {
        if (n == 0)
            first = (*it)[1].str() + "/" + (*it)[2].str();
        n++;
    }
    std::printf("matches=%d first=%s\n", n, first.c_str());

    std::string sub = std::regex_replace(text, word, "<$1 at $2>");
    std::printf("replaced=%s\n", sub.c_str());

    std::printf("search=%d\n",
                std::regex_search(text, std::regex("b.b@")));

    std::regex digits("[0-9]+");
    std::smatch m;
    std::string nums = "abc 123 def 4567";
    if (std::regex_search(nums, m, digits))
        std::printf("first_num=%s pos=%zu\n", m[0].str().c_str(),
                    static_cast<size_t>(m.position(0)));
    return 0;
}
