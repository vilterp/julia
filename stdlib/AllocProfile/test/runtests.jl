# TODO: register AllocProfile in the stdlib
using Pkg; Pkg.activate("stdlib/AllocProfile")

using Test

using AllocProfile

@testset "alloc profiler doesn't segfault" begin
    AllocProfile.start()

    # test the allocations during compilation
    using Base64

    AllocProfile.stop()
    raw_results = AllocProfile.g_profile[]
    results = AllocProfile.decode(raw_results)

    @test length(results.allocs) > 0
    first_alloc = results.allocs[1]
    @test first_alloc.bytes_allocated > 0
    @test length(first_alloc.stacktrace) > 0
    @test length(string(first_alloc.type)) > 0

    json = AllocProfile.write_as_json(results)
    println(json)
    @test length(json) > 0
end
