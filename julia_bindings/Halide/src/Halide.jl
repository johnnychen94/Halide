module Halide

  using CxxWrap
  # TODO: wrap this with jll package provided by BinaryBuilder to avoid hardcoded path
  artifacts_root = joinpath(@__DIR__, "../../../build/julia_bindings/src")
  @wrapmodule(joinpath(artifacts_root, "libhalide"), :define_func)

  
  function __init__()
    @initcxx
  end

end # module
