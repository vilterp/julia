module AllocProfile

using Base.StackTraces: StackTrace, StackFrame, lookup
using Base: InterpreterIP, CodeInfo, _reformat_bt

# matches RawAllocResults on the C side
struct RawAllocProfile
    alloc_types::Vector{Csize_t}
    alloc_sizes::Vector{Csize_t}
    alloc_bts::Vector{Vector{Ptr{Cvoid}}}
    alloc_bt2s::Vector{Vector{Union{Base.CodeInfo,Module,Base.InterpreterIP,Core.Compiler.InterpreterIP}}}

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
            Vector{Csize_t}(),
            Vector{Csize_t}(),
            Vector{Vector{Ptr{Cvoid}}}(),
            Vector{Vector{Union{Base.InterpreterIP,Core.Compiler.InterpreterIP}}}(),
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
    bt2_length::Int
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

function decode_alloc(
    cache::BacktraceCache,
    bt::Vector{Ptr{Cvoid}},
    bt2::Vector{Union{Base.InterpreterIP,Core.Compiler.InterpreterIP}}
)::Alloc
    Alloc(
        # load_type(raw_alloc.type),
        Int, # TODO: get type
        stacktrace_memoized(cache, _reformat_bt(bt, bt2)),
        UInt(raw_alloc.size)
    )
end

function _reformat_bt_custom(bt::Array{Ptr{Cvoid},1}, bt2::Vector{Union{CodeInfo,Module,Base.InterpreterIP,Core.Compiler.InterpreterIP}})
    ret = Vector{Union{InterpreterIP,Ptr{Cvoid}}}()
    i, j = 1, 1
    while i <= length(bt)
        ip = bt[i]::Ptr{Cvoid}
        if UInt(ip) != (-1 % UInt) # See also jl_bt_is_native
            # native frame
            push!(ret, ip)
            i += 1
            continue
        end
        # Extended backtrace entry
        entry_metadata = reinterpret(UInt, bt[i+1])::UInt
        njlvalues =  entry_metadata & 0x7
        nuintvals = (entry_metadata >> 3) & 0x7
        tag       = (entry_metadata >> 6) & 0xf
        header    =  entry_metadata >> 10
        if tag == 1 # JL_BT_INTERP_FRAME_TAG
            code = bt2[j]::Union{CodeInfo,Core.MethodInstance,Nothing}
            mod = njlvalues == 2 ? bt2[j+1]::Union{Module,Nothing} : nothing
            push!(ret, InterpreterIP(code, header, mod))
        else
            # Tags we don't know about are an error
            throw(ArgumentError("Unexpected extended backtrace entry tag $tag at bt[$i]"))
        end
        # See jl_bt_entry_size
        j += Int(njlvalues)
        i += 2 + Int(njlvalues + nuintvals)
    end
    ret
end

function decode(raw_results::RawAllocProfile)::AllocResults
    cache = BacktraceCache()
    allocs = Vector{Alloc}()

    @assert length(raw_results.alloc_bts) == length(raw_results.alloc_bt2s)
    bt2_length = 0

    for i in 1:length(raw_results.alloc_bts)
        bt = raw_results.alloc_bts[i]
        bt2 = raw_results.alloc_bt2s[i]
        back_trace = _reformat_bt_custom(bt, bt2)
        stack_trace = stacktrace_memoized(cache, back_trace)
        size = 5 # TODO: grab this
        push!(allocs, Alloc(
            Int,
            stack_trace,
            size
        ))

        bt2_length += length(bt2)
    end

    frees = Dict{Type, UInt}()
    
    return AllocResults(
        allocs,
        frees,
        bt2_length
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
