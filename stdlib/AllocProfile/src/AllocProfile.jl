module AllocProfile

using Base.StackTraces: StackTrace, StackFrame, lookup
using Base: InterpreterIP, CodeInfo, _reformat_bt

const ExtendedEntryObj = Union{
    Base.CodeInfo,
    Module,
    Base.InterpreterIP,
    Core.Compiler.InterpreterIP
}

# matches RawAllocResults on the C side
struct RawAllocProfile
    alloc_types::Vector{Ptr{Type}}
    alloc_sizes::Vector{Ptr{Csize_t}}
    alloc_bts::Vector{Vector{Ptr{Cvoid}}}
    alloc_bt2s::Vector{Vector{ExtendedEntryObj}}

    # sampling parameter
    skip_every::Csize_t
    # sampling state
    alloc_counter::Csize_t
    last_recorded_alloc::Csize_t
    
    # TODO: get frees (garbage profiling) working again
    # frees_by_type::Dict{Type,UInt}
    # type_address_by_value_address::Dict{}

    function RawAllocProfile(skip_every::Int)
        return new(
            Vector{Type}(),
            Vector{Csize_t}(),
            Vector{Vector{Ptr{Cvoid}}}(),
            Vector{Vector{ExtendedEntryObj}}(),
            skip_every,
            0,
            0
        )
    end
end

# default implementation actually gives a fatal type inference
# error while printing. lol
function Base.show(io::IO, ::RawAllocProfile)
    print(io, "RawAllocProfile")
end

# pass this in, push to it
const g_profile = Ref{RawAllocProfile}()

function start(skip_every::Int=0)
    g_profile[] = RawAllocProfile(skip_every)
    ccall(
        :jl_start_alloc_profile,
        Cvoid,
        (Cint, Ref{RawAllocProfile}),
        skip_every,
        g_profile
    )
end

function stop()
    ccall(:jl_stop_alloc_profile, Cvoid, ())
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

function profile(f::Function, skip_every::Int=0)
    AllocProfile.start(skip_every)
    res = f()
    AllocProfile.stop()
    return (res, decode(g_profile[]))
end

# loading anything below this seems to segfault
# TODO: use filter out special types before we get here
TYPE_PTR_THRESHOLD = 0x0000000100000000

function load_type(ptr::Ptr{Type})::Type
    if UInt(ptr) < TYPE_PTR_THRESHOLD
        return Missing
    end
    return unsafe_pointer_to_objref(ptr)
end

function decode(raw_results::RawAllocProfile)::AllocResults
    cache = BacktraceCache()
    allocs = Vector{Alloc}()

    @assert length(raw_results.alloc_bts) ==
        length(raw_results.alloc_bt2s) ==
        length(raw_results.alloc_types) ==
        length(raw_results.alloc_sizes)

    for i in 1:length(raw_results.alloc_bts)
        bt = raw_results.alloc_bts[i]
        bt2 = raw_results.alloc_bt2s[i]
        type_tag = raw_results.alloc_types[i]
        # size = ccall(:jl_unbox_uint64, UInt64, (Ptr{Csize_t},), raw_results.alloc_sizes[i])
        size = 42 # TODO: sometimes getting corrupt values here

        type = load_type(type_tag)
        stack_trace = stacktrace_memoized(cache, bt)
        
        push!(allocs, Alloc(
            type,
            stack_trace,
            size
        ))
    end

    frees = Dict{Type, UInt}()
    
    return AllocResults(
        allocs,
        frees,
    )
end

function stacktrace_memoized(
    cache::BacktraceCache,
    trace::Vector{Ptr{Nothing}},
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

include("write_as_json.jl")

end
