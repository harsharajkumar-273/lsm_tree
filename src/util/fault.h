#pragma once
// Deterministic fault injection for crash- and error-path tests.
//
// Production code marks interesting moments with fault::hit("name") or asks
// fault::shouldFail("name") whether an I/O result should be turned into an
// error. Nothing is armed in normal operation, and the disarmed fast path is a
// single relaxed atomic load.
//
// A test arms a point to fire on its Nth hit:
//   Throw  - hit() throws fault::Injected (models an I/O error surfacing)
//   Crash  - hit() SIGKILLs the process (models a process crash: no destructors,
//            no flush; page-cache contents survive, which is the failure model
//            the crash tests check - see DURABILITY.md)
//   Fail   - shouldFail() returns true once (models a failed/short completion)
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace fault {

struct Injected : std::runtime_error {
    explicit Injected(const std::string& point)
        : std::runtime_error("injected fault at " + point) {}
};

enum class Action { Throw, Crash, Fail };

extern std::atomic<bool> g_armed;

void hitSlow(const char* point);
bool shouldFailSlow(const char* point);

inline void hit(const char* point) {
    if (g_armed.load(std::memory_order_relaxed)) hitSlow(point);
}
inline bool shouldFail(const char* point) {
    return g_armed.load(std::memory_order_relaxed) && shouldFailSlow(point);
}

// Like hit(), but only a Crash action fires. Used where production code has no
// failure to report - e.g. after the commit point of a compaction, where real
// errors are logged rather than thrown - so a Throw there would model something
// that cannot happen and only produce false alarms.
inline void crashPoint(const char* point);

// Suspends all armed faults (and their hit counting) for the lifetime of the
// object. Tests use it while a second engine - e.g. one recovering from a copied
// directory - runs in the same process, so the fault meant for the engine under
// test is not consumed by the recovery engine instead.
class Pause {
public:
    Pause() : was_(g_armed.exchange(false)) {}
    ~Pause() { g_armed.store(was_); }
    Pause(const Pause&) = delete;
    Pause& operator=(const Pause&) = delete;
private:
    bool was_;
};

// Test-side control.
void arm(const std::string& point, uint64_t nth, Action action);
void disarmAll();
uint64_t hitCount(const std::string& point);   // counted only while something is armed

void crashPointSlow(const char* point);
inline void crashPoint(const char* point) {
    if (g_armed.load(std::memory_order_relaxed)) crashPointSlow(point);
}

}  // namespace fault
