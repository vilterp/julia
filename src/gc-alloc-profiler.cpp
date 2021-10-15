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

// Insert a record into the trie indicating that we allocated the
// given type at the given stack.
//
// TODO: move to method on StackTrieNode
// I don't know how to C++
void stack_trie_insert_alloc(StackTrieNode *trie, vector<jl_bt_element_t> stack, size_t type_id) {
    jl_printf(JL_STDERR, "TODO: insert stack\n");
}

void alloc_profile_serialize(ios_t *out, AllocProfile *profile) {
    // walk the trie, serializing nodes and edges as we go
    // maybe directly as graphviz? idk
    ios_printf(out, "TODO: actually write alloc profile\n");
}

// == global variables manipulated by callbacks ==

int g_alloc_profile_enabled = 0;
AllocProfile *g_alloc_profile;

// == utility functions ==

void print_str_escape_csv(ios_t *stream, const std::string &s) {
    ios_printf(stream, "\"");
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        switch (*c) {
        case '"': ios_printf(stream, "\"\""); break;
        default:
            ios_printf(stream, "%c", *c);
        }
    }
    ios_printf(stream, "\"");
}

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

JL_DLLEXPORT void jl_start_alloc_profile() {
    g_alloc_profile_enabled = 1;
    g_alloc_profile = new AllocProfile{};
}

JL_DLLEXPORT void jl_finish_and_write_alloc_profile(ios_t *stream) {
    g_alloc_profile_enabled = 0;
    
    alloc_profile_serialize(stream, g_alloc_profile);

    // TODO: something to free the alloc profile?
    // I don't know how to C++
    g_alloc_profile = nullptr;
}

// == callbacks called into by the outside ==

void _report_gc_started() {
    // TODO: anything?
}

// TODO: figure out how to pass all of these in as a struct
void _report_gc_finished(uint64_t pause, uint64_t freed, uint64_t allocd) {
    // TODO: figure out how to put in commas
    jl_printf(
        JL_STDERR,
        "GC: pause %fms. collected %fMB. %lld allocs total\n",
        pause/1e6, freed/1e6, allocd
    );

    // TODO: anything else?
}

void register_type_string(jl_datatype_t *type) {
    auto id = g_alloc_profile->type_name_by_address.find((size_t)type);
    if (id != g_alloc_profile->type_name_by_address.end()) {
        return;
    }

    string type_str = _type_as_string(type);
    g_alloc_profile->type_name_by_address[(size_t)type] = type_str;
}

vector<jl_bt_element_t> get_stack() {
    // TODO: don't allocate this every time
    jl_bt_element_t *bt_data = (jl_bt_element_t*) malloc(JL_MAX_BT_SIZE);

    // TODO: tune the number of frames that are skipped
    size_t num_frames = rec_backtrace(bt_data, JL_MAX_BT_SIZE, 1);

    jl_printf(JL_STDERR, "==============");

    // TODO: maybe just return the raw buffer
    vector<jl_bt_element_t> stack;
    for (size_t i = 0; i < num_frames; i++) {
        jl_print_bt_entry_codeloc(&bt_data[i]);
        stack.push_back(bt_data[i]);
    }

    return stack;
}

void _record_allocated_value(jl_value_t *val) {
    auto type = (jl_datatype_t*)jl_typeof(val);
    register_type_string(type);

    // TODO: get stack, push into vector
    auto stack = get_stack();

    stack_trie_insert_alloc(&g_alloc_profile->root, stack, (size_t)type);
}

void _record_freed_value(jl_taggedvalue_t *tagged_val) {
    // TODO: anything?
}

