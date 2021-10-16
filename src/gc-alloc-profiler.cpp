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

struct StackFrame {
    string func_name;
    string file_name;
    intptr_t line_no;
};

struct Alloc {
    size_t type_address;
    vector<StackFrame> stack;
};

struct StackTrieNode {
    jl_bt_element_t frame;
    size_t id;
    vector<StackTrieNode> children;
    unordered_map<size_t, size_t> allocs_by_type_address;
};

struct AllocProfile {
    StackTrieNode root;
    unordered_map<size_t, string> type_name_by_address;
};

struct RawBacktrace {
    jl_bt_element_t *data;
    size_t size;
};

// Insert a record into the trie indicating that we allocated the
// given type at the given stack.
//
// TODO: move to method on StackTrieNode
// I don't know how to C++
void record_alloc(AllocProfile *profile, RawBacktrace stack, size_t type_address) {
    vector<jl_bt_element_t*> stack_vec;

    for (int i = 0; i < stack.size; i += jl_bt_entry_size(stack.data + i)) {
        jl_bt_element_t *bt_entry = stack.data + i;

        stack_vec.push_back(bt_entry);
    }
}

void alloc_profile_serialize(ios_t *out, AllocProfile *profile) {
    ios_printf(out, "TODO: serialize trie\n");
}

// == global variables manipulated by callbacks ==

AllocProfile *g_alloc_profile;
ios_t *g_alloc_profile_out;

// == utility functions ==

string _type_as_string(jl_datatype_t *type) {
    if ((uintptr_t)type < 4096U) {
        return "<corrupt>";
    } else if (type == (jl_datatype_t*)jl_buff_tag) {
        return "<buffer>";
    } else if (type == (jl_datatype_t*)jl_malloc_tag) {
        return "<malloc>";
    } else if (type == jl_string_type) {
        return "<string>";
    } else if (type == jl_symbol_type) {
        return "<symbol>";
    } else if (jl_is_datatype(type)) {
        ios_t str_;
        ios_mem(&str_, 10024);
        JL_STREAM* str = (JL_STREAM*)&str_;

        jl_static_show(str, (jl_value_t*)type);

        string type_str = string((const char*)str_.buf, str_.size);
        ios_close(&str_);

        return type_str;
    } else {
        return "<missing>";
    }
}

// == exported interface ==

JL_DLLEXPORT void jl_start_alloc_profile(ios_t *stream) {
    g_alloc_profile_out = stream;
    g_alloc_profile = new AllocProfile{};
}

JL_DLLEXPORT void jl_stop_alloc_profile() {
    g_alloc_profile_out = nullptr;
    
    // TODO: something to free the alloc profile?
    // I don't know how to C++
    g_alloc_profile = nullptr;
}

// == callbacks called into by the outside ==

void register_type_string(jl_datatype_t *type) {
    auto id = g_alloc_profile->type_name_by_address.find((size_t)type);
    if (id != g_alloc_profile->type_name_by_address.end()) {
        return;
    }

    string type_str = _type_as_string(type);
    g_alloc_profile->type_name_by_address[(size_t)type] = type_str;
}

// Print function, file and line containing native instruction pointer `ip` by
// looking up debug info. Prints multiple such frames when `ip` points to
// inlined code.
StackFrame get_native_frame(uintptr_t ip) JL_NOTSAFEPOINT {
    StackFrame out_frame;

    // This function is not allowed to reference any TLS variables since
    // it can be called from an unmanaged thread on OSX.
    // it means calling getFunctionInfo with noInline = 1
    jl_frame_t *frames = NULL;
    int n = jl_getFunctionInfo(&frames, ip, 0, 0);
    int i;

    for (i = 0; i < n; i++) {
        jl_frame_t frame = frames[i];
        if (!frame.func_name) {
            jl_safe_printf("unknown function (ip: %p)\n", (void*)ip);
        }
        else {
            out_frame.func_name = frame.func_name;
            out_frame.file_name = frame.file_name;
            out_frame.line_no = frame.line;

            free(frame.func_name);
            free(frame.file_name);
        }
    }
    free(frames);

    return out_frame;
}

RawBacktrace get_stack() {
    // TODO: don't allocate this every time
    jl_bt_element_t *bt_data = (jl_bt_element_t*) malloc(JL_MAX_BT_SIZE);

    // TODO: tune the number of frames that are skipped
    size_t bt_size = rec_backtrace(bt_data, JL_MAX_BT_SIZE, 1);

    return RawBacktrace{
        bt_data,
        bt_size
    };
}

void _record_allocated_value(jl_value_t *val) {
    auto type = (jl_datatype_t*)jl_typeof(val);
    register_type_string(type);

    // TODO: get stack, push into vector
    auto stack = get_stack();

    record_alloc(g_alloc_profile, stack, (size_t)type);

    // TODO: more idiomatic way to destruct this
    free(stack.data);
}

void _report_gc_started() {
    // ...
}

// TODO: figure out how to pass all of these in as a struct
void _report_gc_finished(uint64_t pause, uint64_t freed, uint64_t allocd) {
    // TODO: figure out how to put in commas
    jl_printf(
        JL_STDERR,
        "GC: pause %fms. collected %fMB. %lld allocs total\n",
        pause/1e6, freed/1e6, allocd
    );

    if (g_alloc_profile_out != nullptr) {
        alloc_profile_serialize(g_alloc_profile_out, g_alloc_profile);
        ios_flush(g_alloc_profile_out);
    }
}
