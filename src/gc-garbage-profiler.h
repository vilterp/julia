// This file is a part of Julia. License is MIT: https://julialang.org/license

#ifndef JL_GC_GARBAGE_PROFILER_H
#define JL_GC_GARBAGE_PROFILER_H

#include "julia.h"
#include "ios.h"

#ifdef __cplusplus
extern "C" {
#endif

JL_DLLEXPORT void jl_start_alloc_profile(void);
JL_DLLEXPORT void jl_finish_and_write_alloc_profile(ios_t *stream);

void _report_gc_started(void);
void _report_gc_finished(uint64_t pause, uint64_t freed, uint64_t allocd);
void _record_allocated_value(jl_value_t *val);
void _record_freed_value(jl_taggedvalue_t *tagged_val);

// ---------------------------------------------------------------------
// functions to call from GC when garbage profiling is enabled
// ---------------------------------------------------------------------

extern int g_alloc_profile_enabled;

static inline void record_allocated_value(jl_value_t *val) {
    if (__unlikely(g_alloc_profile_enabled != 0)) {
        _record_allocated_value(val);
    }
}

static inline void record_freed_value(jl_taggedvalue_t *tagged_val) {
    if (__unlikely(g_alloc_profile_enabled != 0)) {
        _record_freed_value(tagged_val);
    }
}

#ifdef __cplusplus
}
#endif


#endif  // JL_GC_GARBAGE_PROFILER_H
