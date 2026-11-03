#include <cxxabi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main() {
    int status = -1;
    char *s = abi::__cxa_demangle("_Z3fooi", nullptr, nullptr, &status);
    std::printf("demangle status=%d name=%s\n", status, s ? s : "(null)");
    std::free(s);
    return status == 0 && s && std::strcmp(s, "foo(int)") == 0 ? 0 : 1;
}
