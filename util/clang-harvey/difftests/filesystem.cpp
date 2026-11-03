// Filesystem: create, write, read, iterate, rename, remove.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    const char *tmp = std::getenv("TMPDIR");
    if (tmp == nullptr)
        tmp = "/tmp";
    fs::path root = fs::path(tmp) / "harvey-difftest";
    fs::remove_all(root);
    fs::create_directories(root / "a" / "b");

    {
        std::ofstream out(root / "a" / "b" / "file.txt");
        out << "hello file\n";
    }
    {
        std::ifstream in(root / "a" / "b" / "file.txt");
        std::string line;
        std::getline(in, line);
        std::printf("read=%s\n", line.c_str());
    }
    std::printf("size=%ju\n",
                static_cast<uintmax_t>(fs::file_size(root / "a" / "b" / "file.txt")));

    std::vector<std::string> names;
    for (const auto &e : fs::recursive_directory_iterator(root))
        names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    std::printf("entries=%zu", names.size());
    for (const auto &n : names)
        std::printf(" %s", n.c_str());
    std::printf("\n");

    fs::rename(root / "a" / "b" / "file.txt", root / "moved.txt");
    std::printf("exists=%d moved=%d\n", fs::exists(root / "a" / "b" / "file.txt"),
                fs::exists(root / "moved.txt"));

    fs::copy_file(root / "moved.txt", root / "copy.txt");
    std::printf("copied=%d\n", fs::exists(root / "copy.txt"));
    std::printf("isreg=%d isdir=%d\n", fs::is_regular_file(root / "copy.txt"),
                fs::is_directory(root / "a"));

    fs::remove_all(root);
    std::printf("gone=%d\n", fs::exists(root));
    return 0;
}
