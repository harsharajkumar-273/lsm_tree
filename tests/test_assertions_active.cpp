// Canary: proves assert() is live in this build. If someone adds -DNDEBUG to the
// test flags, the many assert()-based tests would silently verify nothing; this
// test fails to compile (or run) instead of letting that pass unnoticed.
#include <cassert>
#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

#ifdef NDEBUG
#error "NDEBUG is defined: assert() is compiled out, so tests would pass without checking anything"
#endif

int main() {
    pid_t pid = fork();
    if (pid == 0) {
        std::signal(SIGABRT, SIG_DFL);
        assert(false && "canary");
        _exit(0);   // reached only if assert() was a no-op
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)) {
        std::fprintf(stderr, "assert(false) did not abort: assertions are not active\n");
        return 1;
    }
    std::puts("assertions are active");
    return 0;
}
