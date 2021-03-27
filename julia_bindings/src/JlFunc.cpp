#include "JlFunc.h"

#include <string>
#include <vector>

namespace Halide {
namespace JuliaBindings {


JLCXX_MODULE define_func(jlcxx::Module& m) {
    m.add_type<Func>("Func")
        .constructor<const std::string&>()
        .constructor<>()
        .method("name", &Func::name);
}

}  // namespace JuliaBindings
}  // namespace Halide
