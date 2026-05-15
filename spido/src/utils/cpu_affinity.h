#pragma once

#include <pthread.h>

#include <cstddef>
#include <vector>

namespace spido::util {

unsigned                online_cpus();
std::vector<unsigned>   default_cpu_set(unsigned threads);

// Pin the given pthread to a single CPU. Returns false on failure; caller
// can log and continue (affinity is an optimization, not a correctness req).
bool pin_thread(pthread_t t, unsigned cpu);

} // namespace spido::util
