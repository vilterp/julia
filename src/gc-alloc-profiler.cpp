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
    unordered_map<string, StackTrieNode*> children;
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

vector<StackFrame> get_julia_frames(jl_bt_element_t *bt_entry) {
    vector<StackFrame> ret;

    size_t ip = jl_bt_entry_header(bt_entry);
    jl_value_t *code = jl_bt_entry_jlvalue(bt_entry, 0);
    if (jl_is_method_instance(code)) {
        // When interpreting a method instance, need to unwrap to find the code info
        code = ((jl_method_instance_t*)code)->uninferred;
    }
    if (jl_is_code_info(code)) {
        jl_code_info_t *src = (jl_code_info_t*)code;
        // See also the debug info handling in codegen.cpp.
        // NB: debuginfoloc is 1-based!
        intptr_t debuginfoloc = ((int32_t*)jl_array_data(src->codelocs))[ip];
        while (debuginfoloc != 0) {
            jl_line_info_node_t *locinfo = (jl_line_info_node_t*)
                jl_array_ptr_ref(src->linetable, debuginfoloc - 1);
            assert(jl_typeis(locinfo, jl_lineinfonode_type));
            const char *func_name = "Unknown";
            jl_value_t *method = locinfo->method;
            if (jl_is_method_instance(method))
                method = ((jl_method_instance_t*)method)->def.value;
            if (jl_is_method(method))
                method = (jl_value_t*)((jl_method_t*)method)->name;
            if (jl_is_symbol(method))
                func_name = jl_symbol_name((jl_sym_t*)method);

            ret.push_back(StackFrame{
                func_name,
                jl_symbol_name(locinfo->file),
                locinfo->line,
            });
            
            debuginfoloc = locinfo->inlined_at;
        }
    }
    else {
        // If we're using this function something bad has already happened;
        // be a bit defensive to avoid crashing while reporting the crash.
        jl_safe_printf("No code info - unknown interpreter state!\n");
    }
    return ret;
}

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

string entry_to_string(jl_bt_element_t *entry) {
    if (jl_bt_is_native(entry)) {
        auto frame = get_native_frame(entry[0].uintptr);
        return "<native>"; // XXX
    } else {
        auto frames = get_julia_frames(entry);
        return "<julia>"; // XXX
    }
}

// TODO: pass size as well
void trie_insert(StackTrieNode *node, vector<jl_bt_element_t*> path, size_t idx, size_t type_address) {
    if (idx == path.size()) {
        auto allocs = node->allocs_by_type_address.find(type_address);
        if (allocs == node->allocs_by_type_address.end()) {
            node->allocs_by_type_address[type_address] = 1;
        } else {
            node->allocs_by_type_address[type_address]++;
        }
        return;
    }
    
    auto entry = path[idx];
    string child_str = entry_to_string(entry);
    auto child = node->children.find(child_str);
    StackTrieNode *child_node;
    if (child == node->children.end()) {
        child_node = new StackTrieNode();
        node->children[child_str] = child_node;
    } else {
        child_node = child->second;
    }
    trie_insert(child_node, path, idx+1, type_address);
}

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

    trie_insert(&profile->root, stack_vec, 0, type_address);
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
