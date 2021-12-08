// This file is a part of Julia. License is MIT: https://julialang.org/license

#ifndef JL_GC_ALLOC_PROFILER_H
#define JL_GC_ALLOC_PROFILER_H

#include "julia.h"
#include "ios.h"

#ifdef __cplusplus
extern "C" {
#endif

struct TypeNamePair {
    size_t addr;
    jl_value_t *name;
};

struct RawAllocResults {
    void *allocs; // Alloc* (see gc-alloc-profiler.cpp)
    size_t num_allocs;

    struct TypeNamePair *type_names; // an array
    size_t num_type_names;
};

void _report_gc_started(void);
void _report_gc_finished(uint64_t pause, uint64_t freed, uint64_t allocd);
JL_DLLEXPORT void jl_start_alloc_profile(int skip_every);
JL_DLLEXPORT struct RawAllocResults jl_stop_alloc_profile(void);
JL_DLLEXPORT void jl_free_alloc_profile(void);

void _record_allocated_value(jl_value_t *val, size_t size);
void _record_freed_value(jl_taggedvalue_t *tagged_val);

// ---------------------------------------------------------------------
// functions to call from GC when alloc profiling is enabled
// ---------------------------------------------------------------------

extern int g_alloc_profile_enabled;

static inline void record_allocated_value(jl_value_t *val, size_t size) {
    if (__unlikely(g_alloc_profile_enabled)) {
        _record_allocated_value(val, size);
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


#endif  // JL_GC_ALLOC_PROFILER_H
