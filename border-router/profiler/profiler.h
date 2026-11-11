#ifndef __PROFILER_H__
#define __PROFILER_H__

#include <rte_lcore.h>
#include <rte_cycles.h>
#include <stdint.h>

#ifdef PROFILER_ENABLE

/* Definition of profiler. */
#define PROFILER_DEF(name) uint64_t _profile_##name[32] = {0};

#define PROFILER_DECL(name) extern uint64_t _profile_##name[32]

/* Beginning of a profiler. */
#define PROFILER_BEGIN(name) uint64_t _profile_ ## name ## _start = rte_rdtsc()

/* End of a profiler. */
#define PROFILER_END(name) uint64_t _profile_##name##_end = rte_rdtsc(); \
    extern uint64_t _profile_##name[32]; \
    _profile_##name[rte_lcore_id()] += _profile_##name##_end - _profile_##name##_start

#define PROFILER_GET(name, lcore) (double)_profile_##name[lcore] / rte_get_tsc_hz()

/* Declare profiler's data variable (double t_name). */
#define PROFILER_DATA(name, lcore) PROFILER_DECL(name); double t_##name = PROFILER_GET(name, lcore)

#else
#define PROFILER_DEF(name) 
#define PROFILER_DECL(name)
#define PROFILER_BEGIN(name)
#define PROFILER_END(name)
#define PROFILER_GET(name, lcore)
#define PROFILER_DATA(name, lcore)
#endif
#endif