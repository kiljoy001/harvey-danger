/*
 * stdio is thread-safe.  Four threads write 200 records each through one
 * FILE; every record is a single fprintf() of a 200-byte payload plus an
 * id and a sequence number.  With no locking the threads interleave inside
 * the FILE buffer and the records come back corrupted, so the test re-reads
 * the file and checks that every line is intact and that all 800 records
 * are present exactly once.
 */
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static const int NT = 4;
static const int N = 200;
static const char *path = "/n/host/build/libcxx/stdio-lock.txt";

int main() {
    FILE *f = std::fopen(path, "w");
    if (!f) {
        std::perror("fopen");
        return 1;
    }

    std::vector<std::thread> ts;
    for (int t = 0; t < NT; t++) {
        ts.emplace_back([f, t] {
            char payload[201];
            std::memset(payload, 'A' + t, 200);
            payload[200] = 0;
            for (int k = 0; k < N; k++)
                std::fprintf(f, "%s %d %d\n", payload, t, k);
        });
    }
    for (auto &th : ts)
        th.join();

    if (std::fclose(f) != 0) {
        std::perror("fclose");
        return 1;
    }

    f = std::fopen(path, "r");
    if (!f) {
        std::perror("fopen r");
        return 1;
    }

    static int seen[NT][N];
    char line[512];
    int lines = 0, bad = 0;
    while (std::fgets(line, sizeof line, f)) {
        lines++;
        char *sp = std::strchr(line, ' ');
        if (!sp) {
            bad++;
            continue;
        }
        if (sp - line != 200) {
            bad++;
            continue;
        }
        char want = line[0];
        int t = want - 'A';
        if (t < 0 || t >= NT) {
            bad++;
            continue;
        }
        int ok = 1;
        for (int i = 0; i < 200; i++)
            if (line[i] != want) {
                ok = 0;
                break;
            }
        int id = -1, k = -1;
        if (std::sscanf(sp + 1, "%d %d", &id, &k) != 2)
            ok = 0;
        if (!ok || id != t || k < 0 || k >= N || seen[t][k])
            bad++;
        else
            seen[t][k] = 1;
    }
    std::fclose(f);

    int got = 0;
    for (int t = 0; t < NT; t++)
        for (int k = 0; k < N; k++)
            got += seen[t][k];

    std::printf("stdio-ok lines=%d bad=%d seen=%d\n", lines, bad, got);
    return lines == NT * N && bad == 0 && got == NT * N ? 0 : 1;
}
