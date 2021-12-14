// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "gc-alloc-profiler.h"

#include "julia_internal.h"
#include "gc.h"

#include <string>
#include <unordered_map>
#include <vector>

using std::unordered_map;
using std::string;
using std::vector;

// == global variables manipulated by callbacks ==

int g_alloc_profile_enabled = false;
RawAllocProfile *g_alloc_profile = nullptr;

// === stack stuff ===

void push_raw_alloc(RawAllocProfile *profile, size_t type_tag, size_t bytes_allocated) {
    jl_bt_element_t bt_data[JL_MAX_BT_SIZE];

    // TODO: tune the number of frames that are skipped
    size_t bt_size = rec_backtrace(bt_data, JL_MAX_BT_SIZE, 1);
    
    jl_array_t *bt = NULL, *bt2 = NULL;
    JL_GC_PUSH2(&bt, &bt2);
    decode_backtrace(bt_data, bt_size, &bt, &bt2);
    JL_GC_POP();

    // jl_array_ptr_1d_push(profile->alloc_types, type_tag);
    // jl_array_ptr_1d_push(profile->alloc_sizes, bytes_allocated);
    jl_array_ptr_1d_push(profile->alloc_bts, (jl_value_t *)bt);
    jl_array_ptr_1d_push(profile->alloc_bt2s, (jl_value_t *)bt2);
}

// == exported interface ==

JL_DLLEXPORT void jl_start_alloc_profile(int skip_every, RawAllocProfile *profile) {
    g_alloc_profile_enabled = true;
    g_alloc_profile = profile;
}

extern "C" {  // Needed since the function doesn't take any arguments.

JL_DLLEXPORT void jl_stop_alloc_profile() {
    g_alloc_profile_enabled = false;
    // TODO: frees
}

}

// == callbacks called into by the outside ==

void _record_allocated_value(jl_value_t *val, size_t size) JL_NOTSAFEPOINT {
    auto profile = g_alloc_profile;
    profile->alloc_counter++;
    auto diff = profile->alloc_counter - profile->last_recorded_alloc;
    if (diff < profile->skip_every) {
        return;
    }
    profile->last_recorded_alloc = profile->alloc_counter;

    size_t type_tag = (size_t) jl_typeof(val);

    // profile->type_address_by_value_address[(size_t)val] = (size_t)type;

    auto bytes_allocated = 5; // TODO: where were we getting this from?
    push_raw_alloc(profile, type_tag, size);
}

void _record_freed_value(jl_taggedvalue_t *tagged_val) JL_NOTSAFEPOINT {
    jl_value_t *val = jl_valueof(tagged_val);

    // TODO: get this working again

    // auto value_address = (size_t)val;
    // auto type_address = g_alloc_profile.type_address_by_value_address.find(value_address);
    // if (type_address == g_alloc_profile.type_address_by_value_address.end()) {
    //     return; // TODO: warn
    // }
    // auto frees = g_alloc_profile.frees_by_type_address.find(type_address->second);

    // if (frees == g_alloc_profile.frees_by_type_address.end()) {
    //     g_alloc_profile.frees_by_type_address[type_address->second] = 1;
    // } else {
    //     g_alloc_profile.frees_by_type_address[type_address->second] = frees->second + 1;
    // }
}

// TODO: remove these or make them toggle-able.

void _report_gc_started() JL_NOTSAFEPOINT {
    // ...
}

// TODO: figure out how to pass all of these in as a struct
void _report_gc_finished(
    uint64_t pause, uint64_t freed, uint64_t allocd, int full, int recollect
) JL_NOTSAFEPOINT {
    // TODO: figure out how to put in commas
    jl_safe_printf("GC: pause %fms. collected %fMB. %lld allocs total. %s %s\n",
        pause/1e6, freed/1e6, allocd,
        full ? "full" : "incr", recollect ? "recollect" : ""
    );
}
