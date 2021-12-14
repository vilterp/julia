module AllocProfile

using Base.StackTraces: StackTrace, StackFrame, lookup
using Base: InterpreterIP, _reformat_bt

# matches RawAlloc on the C side
# struct RawAlloc
#     type_tag::Csize_t
#     bt::Vector{Ptr{Cvoid}}
#     bt2::Vector{Union{Base.InterpreterIP,Core.Compiler.InterpreterIP}}
#     bytes_allocated::Csize_t
# end

# matches RawAllocResults on the C side
struct RawAllocProfile
    allocs::Vector{Core.SimpleVector} # (bt1, bt2, type_tag, bytes_allocated)

    # sampling parameter
    skip_every::Csize_t
    # sampling state
    alloc_counter::Csize_t
    last_recorded_alloc::Csize_t
    
    # TODO: get frees (garbage profiling) working again
    # frees_by_type::Dict{Type,UInt}
    # type_address_by_value_address::Dict{}

    function RawAllocResults(skip_every::Int)
        return new(
            Vector{RawAlloc}(),
            skip_every,
            alloc_counter,
            last_recorded_alloc
        )
    end
end

# pass this in, push to it
const g_profile = Ref{RawAllocResults}()

function start(skip_every::Int=0)
    g_profile[] = RawAllocResults(skip_every)
    ccall(
        :jl_start_alloc_profile,
        Cvoid,
        (Cint, Ref{RawAllocResults}),
        skip_every,
        g_profile
    )
end

function stop()
    ccall(:jl_stop_alloc_profile, Cvoid, ())
    decoded_results = decode(raw_results)
    return decoded_results
end

# decoded results

struct Alloc
    type::Type
    stacktrace::StackTrace
    bytes_allocated::Int
end

struct AllocResults
    allocs::Vector{Alloc}
    frees::Dict{Type,UInt}
end

function Base.show(io::IO, ::AllocResults)
    print(io, "AllocResults")
end

const BacktraceEntry = Union{Ptr{Cvoid}, InterpreterIP}
const BacktraceCache = Dict{BacktraceEntry,Vector{StackFrame}}

# loading anything below this seems to segfault
# TODO: use filter out special types before we get here
TYPE_PTR_THRESHOLD = 0x0000000100000000

function load_type(ptr::Ptr{Type})::Type
    if UInt(ptr) < TYPE_PTR_THRESHOLD
        return Missing
    end
    return unsafe_pointer_to_objref(ptr)
end

function decode_backtrace(bt_data::Ptr, bt_size)
    bt, bt2 = ccall(
        :jl_decode_backtrace,
        Core.SimpleVector,
        (Ptr{Csize_t}, Csize_t),
        bt_data,
        bt_size,
    )
    return bt, bt2
end

function decode_alloc(cache::BacktraceCache, raw_alloc::RawAlloc)::Alloc
    bt, bt2 = decode_backtrace(raw_alloc.backtrace.data, raw_alloc.backtrace.size)
    Alloc(
        load_type(raw_alloc.type),
        stacktrace_memoized(cache, _reformat_bt(bt, bt2)),
        UInt(raw_alloc.size)
    )
end

function decode(raw_results::RawAllocResults)::AllocResults
    cache = BacktraceCache()
    allocs = [
        decode_alloc(cache, unsafe_load(raw_results.allocs, i))
        for i in 1:raw_results.num_allocs
    ]

    frees = Dict{Type,UInt}()
    for i in 1:raw_results.num_frees
        free = unsafe_load(raw_results.frees, i)
        type = load_type(free.type)
        frees[type] = free.count
    end
    
    return AllocResults(
        allocs,
        frees
    )
end

function stacktrace_memoized(
    cache::BacktraceCache,
    trace::Vector{BacktraceEntry},
    c_funcs::Bool=true
)::StackTrace
    stack = StackTrace()
    for ip in trace
        frames = get(cache, ip) do
            res = lookup(ip)
            cache[ip] = res
            return res
        end
        for frame in frames
            # Skip frames that come from C calls.
            if c_funcs || !frame.from_c
                push!(stack, frame)
            end
        end
    end
    return stack
end

end
