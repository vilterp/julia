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

    string total;
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

// == utility functions ==

// https://stackoverflow.com/questions/874134/find-out-if-string-ends-with-another-string-in-c
bool ends_with(string const &full_string, string const &ending) {
    if (full_string.length() >= ending.length()) {
        return (0 == full_string.compare(full_string.length() - ending.length(), ending.length(), ending));
    } else {
        return false;
    }
}

bool starts_with(string const &full_string, string const &beginning) {
    if (full_string.length() >= beginning.length()) {
        return (0 == full_string.compare(0, beginning.length(), beginning));
    } else {
        return false;
    }
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

// === stack stuff ===

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

vector<StackFrame> get_native_frames(uintptr_t ip) JL_NOTSAFEPOINT {
    vector<StackFrame> out_frames;

    // This function is not allowed to reference any TLS variables since
    // it can be called from an unmanaged thread on OSX.
    // it means calling getFunctionInfo with noInline = 1
    jl_frame_t *frames = NULL;
    int n = jl_getFunctionInfo(&frames, ip, 0, 0);
    int i;

    for (i = 0; i < n; i++) {
        jl_frame_t frame = frames[i];
        if (!frame.func_name) {
            // TODO: record these somewhere
            // jl_safe_printf("unknown function (ip: %p)\n", (void*)ip);
        }
        else {
            out_frames.push_back(StackFrame{
                frame.func_name,
                frame.file_name,
                frame.line,
            });

            free(frame.func_name);
            free(frame.file_name);
        }
    }
    free(frames);

    return out_frames;
}

vector<StackFrame> get_stack() {
    // TODO: don't allocate this every time
    jl_bt_element_t *bt_data = (jl_bt_element_t*) malloc(JL_MAX_BT_SIZE);

    // TODO: tune the number of frames that are skipped
    size_t bt_size = rec_backtrace(bt_data, JL_MAX_BT_SIZE, 1);

    vector<StackFrame> out;

    for (int i = 0; i < bt_size; i += jl_bt_entry_size(bt_data + i)) {
        jl_bt_element_t *entry = bt_data + i;
        auto is_native = jl_bt_is_native(entry);;

        // TODO: cache frames by bt_element as string?
        auto frames = is_native
            ? get_native_frames(entry[0].uintptr)
            : get_julia_frames(entry);
        
        for (auto frame : frames) {
            auto frame_label = frame.func_name;
            auto is_julia = ends_with(frame.file_name, ".jl") || frame.file_name == "top-level scope";
            auto actual_is_native = !is_julia;
            auto is_stdlib = is_julia && starts_with(frame.file_name, "./");

            if (actual_is_native || is_stdlib) {
                continue;
            }

            out.push_back(frame);
        }
    }

    free(bt_data);

    return out;
}

string stack_frame_to_string(StackFrame frame) {
    if (frame.total != "") {
        return frame.total;
    }

    ios_t str;
    ios_mem(&str, 1024);

    ios_printf(
        &str, "%s at %s:%d",
        frame.func_name.c_str(), frame.file_name.c_str(), frame.line_no
    );

    string type_str = string((const char*)str.buf, str.size);
    frame.total = type_str;
    ios_close(&str);

    return frame.total;
}

// === trie stuff ===

// TODO: pass size as well
void trie_insert(StackTrieNode *node, vector<StackFrame> path, size_t idx, size_t type_address) {
    if (idx == path.size()) {
        auto allocs = node->allocs_by_type_address.find(type_address);
        if (allocs == node->allocs_by_type_address.end()) {
            node->allocs_by_type_address[type_address] = 1;
        } else {
            node->allocs_by_type_address[type_address]++;
        }
        return;
    }
    
    auto frame = path[idx];
    string child_str = stack_frame_to_string(frame);
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

void print_indent(ios_t *out, int level) {
    for (int i=0; i < level; i++) {
        ios_printf(out, "  ");
    }
}

void trie_serialize(ios_t *out, AllocProfile *profile, StackTrieNode *node, int level) {
    for (auto child : node->children) {
        print_indent(out, level);
        ios_printf(out, "%s: [", child.first.c_str());
        auto first = true;
        for (auto alloc_count : child.second->allocs_by_type_address) {
            if (first) {
                first = false;
            } else {
                ios_printf(out, ", ");
            }
            auto type_str = profile->type_name_by_address[alloc_count.first];
            ios_printf(out, "%s: %d", type_str.c_str(), alloc_count.second);
        }
        ios_printf(out, "]\n");
        // TODO: print the types as well
        trie_serialize(out, profile, child.second, level+1);
    }
}

void alloc_profile_serialize(ios_t *out, AllocProfile *profile) {
    trie_serialize(out, profile, &profile->root, 0);
}

// == global variables manipulated by callbacks ==

AllocProfile *g_alloc_profile;
ios_t *g_alloc_profile_out;

// == exported interface ==

JL_DLLEXPORT void jl_start_alloc_profile(ios_t *stream) {
    g_alloc_profile_out = stream;
    g_alloc_profile = new AllocProfile{};
}

JL_DLLEXPORT void jl_stop_alloc_profile() {
    alloc_profile_serialize(g_alloc_profile_out, g_alloc_profile);
    ios_flush(g_alloc_profile_out);
    
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

void _record_allocated_value(jl_value_t *val) {
    auto type = (jl_datatype_t*)jl_typeof(val);
    register_type_string(type);

    // TODO: get stack, push into vector
    auto stack = get_stack();

    trie_insert(&g_alloc_profile->root, stack, 0, (size_t) type);
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
}
