#include "fault.h"
#include <csignal>
#include <map>
#include <mutex>
#include <unistd.h>

namespace fault {

std::atomic<bool> g_armed{false};

namespace {
struct Point {
    uint64_t hits = 0;
    uint64_t nth = 0;       // 0 = not armed
    uint64_t repeat = 1;
    Action   action = Action::Throw;
};
std::mutex g_mu;
std::map<std::string, Point> g_points;

// Returns true when this hit is the armed one; fires at most once per arm().
bool fires(const char* point, Action* out) {
    std::lock_guard<std::mutex> lock(g_mu);
    Point& p = g_points[point];
    ++p.hits;
    if (p.nth != 0 && p.hits >= p.nth) {
        if (p.hits >= p.nth + p.repeat - 1) p.nth = 0;   // last firing: disarm
        *out = p.action;
        return true;
    }
    return false;
}
}  // namespace

void hitSlow(const char* point) {
    Action a;
    if (!fires(point, &a)) return;
    if (a == Action::Crash) {
        ::kill(::getpid(), SIGKILL);
        ::_exit(137);   // unreachable; SIGKILL cannot be handled
    }
    if (a == Action::Throw) throw Injected(point);
    // Action::Fail armed on a hit() point: nothing to do.
}

void crashPointSlow(const char* point) {
    Action a;
    if (fires(point, &a) && a == Action::Crash) {
        ::kill(::getpid(), SIGKILL);
        ::_exit(137);
    }
}

bool shouldFailSlow(const char* point) {
    Action a;
    return fires(point, &a) && a == Action::Fail;
}

void arm(const std::string& point, uint64_t nth, Action action, uint64_t repeat) {
    std::lock_guard<std::mutex> lock(g_mu);
    Point& p = g_points[point];
    p.hits = 0;
    p.nth = nth;
    p.repeat = repeat ? repeat : 1;
    p.action = action;
    g_armed.store(true);
}

void disarmAll() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_points.clear();
    g_armed.store(false);
}

uint64_t hitCount(const std::string& point) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_points.find(point);
    return it == g_points.end() ? 0 : it->second.hits;
}

}  // namespace fault
