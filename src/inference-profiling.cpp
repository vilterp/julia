// Implementation for type inference profiling

#include "julia.h"
#include "julia_internal.h"

#include <vector>

using std::vector;

jl_mutex_t typeinf_profiling_lock;

// Guarded by jl_typeinf_profiling_lock.
// A julia Vector{jl_value_t}
jl_array_t* inference_profiling_results_array;

// == exported interface ==

extern "C" {

JL_DLLEXPORT jl_array_t* jl_typeinf_profiling_clear_and_fetch(jl_value_t *array_timing_type)
{
    JL_LOCK(&typeinf_profiling_lock);

    if (inference_profiling_results_array == nullptr) {
        // Return an empty array
        return jl_alloc_array_1d(array_timing_type, 0);
    }

    size_t len = jl_array_len(inference_profiling_results_array);

    jl_array_t *out = jl_alloc_array_1d(array_timing_type, len);
    JL_GC_PUSH1(&out);

    memcpy(out->data, inference_profiling_results_array->data, len * sizeof(void*));

    jl_array_del_end(inference_profiling_results_array, len);

    JL_UNLOCK(&typeinf_profiling_lock);

    JL_GC_POP();
    return out;
}

JL_DLLEXPORT void jl_typeinf_profiling_push_timing(jl_value_t *timing)
{
    JL_LOCK(&typeinf_profiling_lock);

    if (inference_profiling_results_array == nullptr) {
        inference_profiling_results_array = jl_alloc_array_1d(jl_array_any_type, 0);
    }

    jl_array_ptr_1d_push(inference_profiling_results_array, timing);

    JL_UNLOCK(&typeinf_profiling_lock);
}

}  // extern "C"
