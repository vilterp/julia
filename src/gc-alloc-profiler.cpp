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

    string total; // cache of the above fields concatenated
};

struct RawBacktrace {
    jl_bt_element_t *data;
    size_t size;

    RawBacktrace(
        jl_bt_element_t *_data,
        size_t _size
    ) : data(_data), size(_size) {}

    ~RawBacktrace() {
        if (data != nullptr) {
            free(data);
        }
    }
    // Move constructor (X a = X{...})
    RawBacktrace(RawBacktrace&& rhs) :
        data(rhs.data), size(rhs.size)
    {
        rhs.data = nullptr;
        rhs.size = 0;
    }
    private:
    // Disallow copy and copy-assignment
    RawBacktrace(const RawBacktrace&) = delete; // X b(a);
    RawBacktrace& operator=(const RawBacktrace& other) = delete; // b = a;
};

struct Alloc {
    size_t type_address;
    RawBacktrace backtrace;
    size_t size;
};

struct AllocProfile {
    AllocProfile(int _skip_every) {
        reset(_skip_every);
    }

    int skip_every;

    vector<Alloc> allocs;
    unordered_map<size_t, string> type_name_by_address;
    unordered_map<size_t, size_t> type_address_by_value_address;
    unordered_map<size_t, size_t> frees_by_type_address;

    size_t alloc_counter;
    size_t last_recorded_alloc;

    void reset(int _skip_every) {
        skip_every = _skip_every;
        alloc_counter = 0;
        last_recorded_alloc = 0;
    }

    private:
    AllocProfile(const AllocProfile&) = delete; // X b(a);
    AllocProfile& operator=(const AllocProfile& other) = delete; // b = a;
};

// == global variables manipulated by callbacks ==

AllocProfile g_alloc_profile(0);
int g_alloc_profile_enabled = false;


// == utility functions ==

// https://stackoverflow.com/a/33799784/751061
// TODO: dedup with heap snapshot, or rebase off of that branch
void print_str_escape_json(ios_t *stream, const string &s) {
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

struct StringTable {
    typedef unordered_map<string, size_t> MapType;

    MapType map;
    vector<string> strings;

    StringTable() {}
    StringTable(std::initializer_list<string> strs) : strings(strs) {
        for (const auto& str : strs) {
            map.insert({str, map.size()});
        }
    }

    size_t find_or_create_string_id(string key) {
        auto val = map.find(key);
        if (val == map.end()) {
            val = map.insert(val, {key, map.size()});
            strings.push_back(key);
        }
        return val->second;
    }

    void print_json_array(ios_t *stream, string key, bool newlines) {
        ios_printf(stream, "[");
        bool first = true;
        size_t id = 0;
        for (const auto &str : strings) {
            if (first) {
                first = false;
            } else {
                ios_printf(stream, newlines ? ",\n" : ",");
            }
            ios_printf(stream, "{\"id\":%zu", id);
            id++;
            ios_printf(stream, ",\"%s\":", key.c_str());
            print_str_escape_json(stream, str);
            ios_printf(stream, "}");
        }
        ios_printf(stream, "]");
    }
};

string frame_as_string(jl_bt_element_t *entry, size_t entry_size) {
    auto size_in_bytes = entry_size * sizeof(jl_bt_element_t);
    char *buf = (char*)malloc(size_in_bytes);
    for (int i=0; i < size_in_bytes; i++) {
        buf[i] = ((char*)entry)[i];
    }
    return string(buf, size_in_bytes);
}

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

RawBacktrace get_raw_backtrace() {
    static jl_bt_element_t bt_data[JL_MAX_BT_SIZE];

    // TODO: tune the number of frames that are skipped
    size_t bt_size = rec_backtrace(bt_data, JL_MAX_BT_SIZE, 1);

    return RawBacktrace{
        bt_data,
        bt_size
    };
}

// == exported interface ==

JL_DLLEXPORT void jl_start_alloc_profile(int skip_every) {
    jl_printf(JL_STDERR, "g_alloc_profile se:%d allocs:%d \n", g_alloc_profile.skip_every, g_alloc_profile.allocs.size());
    g_alloc_profile_enabled = true;
    g_alloc_profile.reset(skip_every);
}

extern "C" {  // Needed since the function doesn't take any arguments.

JL_DLLEXPORT struct AllocResults jl_stop_and_write_alloc_profile() {
    g_alloc_profile_enabled = false;

    return AllocResults{g_alloc_profile.allocs.size(),
                    g_alloc_profile.allocs.data()};
}

}

// == callbacks called into by the outside ==

void register_type_string(jl_datatype_t *type) {
    auto id = g_alloc_profile.type_name_by_address.find((size_t)type);
    if (id != g_alloc_profile.type_name_by_address.end()) {
        return;
    }

    string type_str = _type_as_string(type);
    g_alloc_profile.type_name_by_address[(size_t)type] = type_str;
}

void _record_allocated_value(jl_value_t *val, size_t size) {
    auto& profile = g_alloc_profile;
    profile.alloc_counter++;
    auto diff = profile.alloc_counter - profile.last_recorded_alloc;
    if (diff < profile.skip_every) {
        return;
    }
    profile.last_recorded_alloc = profile.alloc_counter;

    auto type = (jl_datatype_t*)jl_typeof(val);
    register_type_string(type);

    profile.type_address_by_value_address[(size_t)val] = (size_t)type;

    // TODO: get stack, push into vector
    profile.allocs.emplace_back(Alloc{
        (size_t) type,
        get_raw_backtrace(),
        size
    });
}

void _record_freed_value(jl_taggedvalue_t *tagged_val) {
    jl_value_t *val = jl_valueof(tagged_val);

    auto value_address = (size_t)val;
    auto type_address = g_alloc_profile.type_address_by_value_address.find(value_address);
    if (type_address == g_alloc_profile.type_address_by_value_address.end()) {
        return; // TODO: warn
    }
    auto frees = g_alloc_profile.frees_by_type_address.find(type_address->second);

    if (frees == g_alloc_profile.frees_by_type_address.end()) {
        g_alloc_profile.frees_by_type_address[type_address->second] = 1;
    } else {
        g_alloc_profile.frees_by_type_address[type_address->second] = frees->second + 1;
    }
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
