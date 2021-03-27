#ifndef HALIDE_JULIA_BINDINGS_JLFUNC_H
#define HALIDE_JULIA_BINDINGS_JLFUNC_H

#include "JlHalide.h"

namespace Halide {
namespace JuliaBindings {

JLCXX_MODULE define_func(jlcxx::Module& m);

}  // namespace JuliaBindings
}  // namespace Halide

#endif  // HALIDE_JULIA_BINDINGS_JLFUNC_H
