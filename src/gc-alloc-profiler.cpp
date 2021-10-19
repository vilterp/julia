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

struct CallGraphNode {
    bool is_native;
    unordered_map<jl_value_t*, size_t> calls_out; // value: # calls to that edge
    unordered_map<size_t, size_t> allocs_by_type_address; // allocations from this node
};

struct AllocProfile {
    unordered_map<jl_value_t*, CallGraphNode*> nodes;
    unordered_map<size_t, string> type_name_by_address;
};

struct RawBacktrace {
    jl_bt_element_t *data;
    size_t size;
};

// == utility functions ==

void print_str_escape_csv(ios_t *stream, const string &s) {
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

void print_str_escape_dot(ios_t *stream, const string &s) {
    ios_printf(stream, "\"");
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        switch (*c) {
        case '"': ios_printf(stream, "\\\""); break;
        case '\\': ios_printf(stream, "\\\\"); break;
        case '\b': ios_printf(stream, "\\b"); break;
        case '\f': ios_printf(stream, "\\f"); break;
        case '\n': ios_printf(stream, "\\n"); break;
        case '\r': ios_printf(stream, "\\r"); break;
        case '\t': ios_printf(stream, "\\t"); break;
        default:
            if ('\x00' <= *c && *c <= '\x1f') {
                ios_printf(stream, "\\u%04x", (int)*c);
            } else {
                ios_printf(stream, "%c", *c);
            }
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

// === stack trace stuff ===

vector<jl_value_t*> get_julia_methods(jl_bt_element_t *bt_entry) {
    vector<jl_value_t*> out;

    size_t ip = jl_bt_entry_header(bt_entry);
    jl_value_t *code = jl_bt_entry_jlvalue(bt_entry, 0);
    if (jl_is_method_instance(code)) {
        // When interpreting a method instance, need to unwrap to find the code info
        code = ((jl_method_instance_t*)code)->uninferred;
    }
    if (!jl_is_code_info(code)) {
        jl_printf(JL_STDERR, "not codeinfo\n");
        return out;
    }
    jl_value_t *method = nullptr;
    jl_code_info_t *src = (jl_code_info_t*)code;
    // See also the debug info handling in codegen.cpp.
    // NB: debuginfoloc is 1-based!
    intptr_t debuginfoloc = ((int32_t*)jl_array_data(src->codelocs))[ip];
    while (debuginfoloc != 0) {
        jl_line_info_node_t *locinfo = (jl_line_info_node_t*)
            jl_array_ptr_ref(src->linetable, debuginfoloc - 1);
        assert(jl_typeis(locinfo, jl_lineinfonode_type));
        method = locinfo->method;
        if (jl_is_method_instance(method))
            method = ((jl_method_instance_t*)method)->def.value;
        if (jl_is_method(method))
            method = (jl_value_t*)((jl_method_t*)method)->name;
        
        out.push_back(method);

        debuginfoloc = locinfo->inlined_at;
    }
    return out;
}

// copy pasted from stackwalk.c (I think)
vector<StackFrame> get_julia_frames(jl_bt_element_t *bt_entry) {
    vector<StackFrame> ret;

    size_t ip = jl_bt_entry_header(bt_entry);
    jl_value_t *code = jl_bt_entry_jlvalue(bt_entry, 0);
    if (jl_is_method_instance(code)) {
        // When interpreting a method instance, need to unwrap to find the code info
        code = ((jl_method_instance_t*)code)->uninferred;
    }
    if (!jl_is_code_info(code)) {
        char buffer[50];
        sprintf(buffer, "%p", (void*)code);
        ret.push_back(StackFrame{
            buffer,
            "unknown",
            0
        });
        return ret;
    }
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
    return ret;
}

vector<StackFrame> get_native_frame(uintptr_t ip) JL_NOTSAFEPOINT {
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
                frame.line
            });

            free(frame.func_name);
            free(frame.file_name);
        }
    }
    free(frames);

    return out_frames;
}

string entry_to_string(jl_value_t *method) {
    char buf[100];
    sprintf(buf, "unknown %p", method);

    char *func_name = buf;

    if (jl_is_method_instance(method))
        method = ((jl_method_instance_t*)method)->def.value;
    if (jl_is_method(method))
        method = (jl_value_t*)((jl_method_t*)method)->name;
    if (jl_is_symbol(method))
        func_name = jl_symbol_name((jl_sym_t*)method);
    return func_name;
}

// === call graph manipulation ===

CallGraphNode *get_or_insert_node(
    AllocProfile *profile, jl_value_t *frame_label, bool is_native
) {
    auto node = profile->nodes.find(frame_label);
    if (node != profile->nodes.end()) {
        return node->second;
    }
    auto new_node = new CallGraphNode{is_native};
    profile->nodes[frame_label] = new_node;
    return new_node;
}

void incr_or_add_alloc(CallGraphNode *node, size_t type_address) {
    auto count = node->allocs_by_type_address.find(type_address);
    if (count == node->allocs_by_type_address.end()) {
        node->allocs_by_type_address[type_address] = 1;
    } else {
        node->allocs_by_type_address[type_address]++;
    }
}

void add_call_edge(CallGraphNode *from_node, jl_value_t *to_node) {
    auto calls_out = from_node->calls_out.find(to_node);
    if (calls_out == from_node->calls_out.end()) {
        from_node->calls_out[to_node] = 1;
    } else {
        from_node->calls_out[to_node]++;
    }
}

// Insert a record into the trie indicating that we allocated the
// given type at the given stack.
//
// TODO: move to method on StackTrieNode
// I don't know how to C++
void record_alloc(AllocProfile *profile, RawBacktrace stack, size_t type_address) {
    jl_value_t *prev_frame_label = nullptr;
    int i = 0;
    while (i < stack.size) {
        jl_bt_element_t *entry = stack.data + i;
        auto entry_size = jl_bt_entry_size(entry);
        i += entry_size;

        auto is_native = jl_bt_is_native(entry);
        if (is_native) {
            continue;
        }

        auto methods = get_julia_methods(entry);

        for (auto method : methods) {
            auto frame_label = method;
            auto cur_node = get_or_insert_node(profile, frame_label, is_native);

            if (prev_frame_label == nullptr) {
                incr_or_add_alloc(cur_node, type_address);
            } else {
                add_call_edge(cur_node, prev_frame_label);
            }
            prev_frame_label = frame_label;
        }
    }
}

void print_indent(ios_t *out, int level) {
    for (int i=0; i < level; i++) {
        ios_printf(out, "  ");
    }
}

void alloc_profile_serialize(ios_t *out, AllocProfile *profile) {
    jl_printf(JL_STDERR, "serialize start\n");

    // unordered_map<string, vector<string>> raw_frames_by_formatted_frame;
    // for (auto node : profile->nodes) {
    //     string formatted_frame = entry_to_string((jl_bt_element_t*) node.first.data());

    //     auto raw_frame = node.first;
    //     auto entries = raw_frames_by_formatted_frame.find(formatted_frame);
    //     if (entries == raw_frames_by_formatted_frame.end()) {
    //         raw_frames_by_formatted_frame[formatted_frame] = {raw_frame};
    //     } else {
    //         raw_frames_by_formatted_frame[formatted_frame].push_back(raw_frame);
    //     }
    // }
    // for (auto entry : raw_frames_by_formatted_frame) {
    //     if (entry.second.size() > 1) {
    //         jl_printf(JL_STDERR, "dup frames for %s: %d\n", entry.first.c_str(), entry.second.size());
    //     }
    // }

    ios_printf(out, "digraph {\n");
    for (auto node : profile->nodes) {
        string entry_str = entry_to_string(node.first);

        auto color = node.second->is_native ? "darksalmon" : "thistle";
        ios_printf(out, "  ");
        print_str_escape_dot(out, entry_str);
        ios_printf(out, " [shape=box, fillcolor=%s, style=filled];\n", color);
    }
    for (auto type : profile->type_name_by_address) {
        ios_printf(out, "  ");
        print_str_escape_dot(out, type.second.c_str());
        ios_printf(out, " [fillcolor=darkseagreen1, shape=box, style=filled];\n");
    }
    for (auto node : profile->nodes) {
        string entry_str = entry_to_string(node.first);

        for (auto out_edge : node.second->calls_out) {
            string out_edge_str = entry_to_string(out_edge.first);
            // ios_printf(
            //     out, "%s,%s,%d\n",
            //     node.first.c_str(), out_edge.first.c_str(), out_edge.second
            // );
            ios_printf(out, "  ");
            print_str_escape_dot(out, entry_str);
            ios_printf(out, " -> ");
            print_str_escape_dot(out, out_edge_str);
            ios_printf(out, " [label=%d];\n", out_edge.second);
        }
        for (auto alloc_count : node.second->allocs_by_type_address) {
            auto type_name = profile->type_name_by_address[alloc_count.first];
            // ios_printf(out, "%s,", node.first.c_str());
            // print_str_escape_csv(out, type_name.c_str());
            // ios_printf(out, "%d\n", alloc_count.second);

            ios_printf(out, "  ");
            print_str_escape_dot(out, entry_str);
            ios_printf(out, " -> ");
            print_str_escape_dot(out, type_name.c_str());
            ios_printf(out, " [label=%d];\n", alloc_count.second);
        }
    }
    ios_printf(out, "}");
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
    // free(stack.data);
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
