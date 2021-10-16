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

    vector<Alloc> allocs;
};

// Insert a record into the trie indicating that we allocated the
// given type at the given stack.
//
// TODO: move to method on StackTrieNode
// I don't know how to C++
void record_alloc(AllocProfile *profile, vector<StackFrame> stack, size_t type_address) {
    profile->allocs.push_back(Alloc{
        type_address,
        stack
    });
}

void alloc_profile_serialize(ios_t *out, AllocProfile *profile) {
    for (auto alloc : profile->allocs) {
        ios_printf(
            out, "type: %s\n",
            profile->type_name_by_address[alloc.type_address].c_str()
        );
        for (auto frame : alloc.stack) {
            ios_printf(
                out, "  %s at %s:%d\n",
                frame.func_name.c_str(), frame.file_name.c_str(), frame.line_no
            );
        }
    }
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

vector<StackFrame> get_stack() {
    // TODO: don't allocate this every time
    jl_bt_element_t *bt_data = (jl_bt_element_t*) malloc(JL_MAX_BT_SIZE);

    // TODO: tune the number of frames that are skipped
    size_t bt_size = rec_backtrace(bt_data, JL_MAX_BT_SIZE, 1);

    vector<StackFrame> stack;

    for (int i = 0; i < bt_size; i += jl_bt_entry_size(bt_data + i)) {
        jl_bt_element_t *bt_entry = bt_data + i;
        if (jl_bt_is_native(bt_entry)) {
            continue;
        }

        // adapted from jl_print_bt_entry_codeloc
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

                stack.push_back(StackFrame{
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
    }

    free(bt_data);

    return stack;
}

void _record_allocated_value(jl_value_t *val) {
    auto type = (jl_datatype_t*)jl_typeof(val);
    register_type_string(type);

    // TODO: get stack, push into vector
    auto stack = get_stack();

    record_alloc(g_alloc_profile, stack, (size_t)type);
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

    alloc_profile_serialize(g_alloc_profile_out, g_alloc_profile);
}
