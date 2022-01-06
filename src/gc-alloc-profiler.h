// This file is a part of Julia. License is MIT: https://julialang.org/license

#ifndef JL_GC_ALLOC_PROFILER_H
#define JL_GC_ALLOC_PROFILER_H

#include "julia.h"
#include "ios.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RawAllocResults {
    void *allocs; // Alloc* (see gc-alloc-profiler.cpp)
    size_t num_allocs;
};

JL_DLLEXPORT void jl_start_alloc_profile(double sample_rate);
JL_DLLEXPORT struct RawAllocResults jl_fetch_alloc_profile(void);
JL_DLLEXPORT void jl_stop_alloc_profile(void);
JL_DLLEXPORT void jl_free_alloc_profile(void);

void _record_allocated_value(jl_value_t *val, size_t size) JL_NOTSAFEPOINT;

// ---------------------------------------------------------------------
// functions to call from GC when alloc profiling is enabled
// ---------------------------------------------------------------------

extern int g_alloc_profile_enabled;

static inline void record_allocated_value(jl_value_t *val, size_t size) JL_NOTSAFEPOINT {
    if (__unlikely(g_alloc_profile_enabled)) {
        _record_allocated_value(val, size);
    }
}

#ifdef __cplusplus
}
#endif


#endif  // JL_GC_ALLOC_PROFILER_H
