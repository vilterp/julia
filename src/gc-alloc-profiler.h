// This file is a part of Julia. License is MIT: https://julialang.org/license

#ifndef JL_GC_ALLOC_PROFILER_H
#define JL_GC_ALLOC_PROFILER_H

#include "julia.h"
#include "ios.h"

#ifdef __cplusplus
extern "C" {
#endif

void _report_gc_started(void);
void _report_gc_finished(uint64_t pause, uint64_t freed, uint64_t allocd);
JL_DLLEXPORT void jl_start_alloc_profile(ios_t *stream);
JL_DLLEXPORT void jl_stop_alloc_profile();

void _record_allocated_value(jl_value_t *val);

// ---------------------------------------------------------------------
// functions to call from GC when alloc profiling is enabled
// ---------------------------------------------------------------------

extern ios_t *g_alloc_profile_out;

static inline void record_allocated_value(jl_value_t *val) {
    if (__unlikely(g_alloc_profile_out != 0)) {
        _record_allocated_value(val);
    }
}

#ifdef __cplusplus
}
#endif


#endif  // JL_GC_ALLOC_PROFILER_H
