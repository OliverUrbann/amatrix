/* Forced include for the unit tests on AmigaOS (libnix has no threads).
 * doctest 2.4.7 always uses std::mutex; give it a no-op one. */
#pragma once
#include <mutex>
#ifndef _GLIBCXX_HAS_GTHREADS
namespace std {
struct mutex {
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }
};
}
#endif
