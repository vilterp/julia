# using Pkg; Pkg.activate("stdlib/AllocProfile")
using AllocProfile

@testset "alloc profiler doesn't segfault" begin
    AllocProfile.start()

    # test the allocations during compilation
    using Base64

    raw_results = AllocProfile.stop()
    
    # TODO: assert something about the results
end
