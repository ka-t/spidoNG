#include "utils/cpu_affinity.h"

#include <sched.h>
#include <unistd.h>

namespace spido::util {

unsigned online_cpus() {
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<unsigned>(n) : 1u;
}

std::vector<unsigned> default_cpu_set(unsigned threads) {
    unsigned n_cpu = online_cpus();
    if (threads == 0) threads = n_cpu;
    std::vector<unsigned> cpus;
    cpus.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) cpus.push_back(i % n_cpu);
    return cpus;
}

bool pin_thread(pthread_t t, unsigned cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(t, sizeof(set), &set) == 0;
}

} // namespace spido::util
