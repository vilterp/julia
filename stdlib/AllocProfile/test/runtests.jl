# TODO: register AllocProfile in the stdlib
using Pkg; Pkg.activate("stdlib/AllocProfile")

using Test

using AllocProfile

@testset "alloc profiler doesn't segfault" begin
    AllocProfile.start()

    # test the allocations during compilation
    using Base64

    raw_results = AllocProfile.stop()
    results = AllocProfile.decode(raw_results)

    @test length(results.allocs) > 0
end
