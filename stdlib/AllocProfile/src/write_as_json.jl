# serialize an allocation profile into reasonably-compact
# JSON by interning code locations and types.

using JSON3

struct SerializationState
    location_ids::Dict{Base.StackFrame,Int}
    type_ids::Dict{Type,Int}

    function SerializationState()
        return new(
            Dict{Base.StackFrame,Int}(),
            Dict{Type,Int}()
        )
    end
end

function get_location_id(st::SerializationState, frame::Base.StackFrame)::Int
    if haskey(st.location_ids, frame)
        return st.location_ids[frame]
    end
    new_id = length(st.location_ids)
    st.location_ids[frame] = new_id
    return new_id
end

function transform_stack(st::SerializationState, stack::Base.StackTrace)
    return [get_location_id(st, frame) for frame in stack]
end

function get_type_id(st::SerializationState, type::Type)
    if haskey(st.type_ids, type)
        return st.type_ids[type]
    end
    new_id = length(st.type_ids)
    st.type_ids[type] = new_id
    return new_id
end

const SerializedAlloc = @NamedTuple begin
    stack::Vector{Int}
    size::Int
    type_id::Int
end

function write_as_json_help(profile::AllocProfile.AllocResults)
    st = SerializationState()

    allocs = Vector{SerializedAlloc}()
    for alloc in profile.allocs
        push!(allocs, (
            stack=transform_stack(st, alloc.stacktrace),
            size=alloc.bytes_allocated,
            type_id=get_type_id(st, alloc.type)
        ))
    end
    return (
        allocs=allocs,
        frees=[],
        locations=[(loc=string(frame), id=id) for (frame, id) in st.location_ids],
        types=[(name=string(type), id=id) for (type, id) in st.type_ids]
    )
end

function write_as_json(profile::AllocProfile.AllocResults)
    val = write_as_json_help(profile)
    println("writing $val")
    return JSON3.write()
end
