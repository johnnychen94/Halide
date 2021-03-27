set(SOURCES
    JlHalide.cpp
    JlFunc.cpp
    )

# CxxWrap requires this to be built as shared library
add_library(Halide_Julia SHARED ${SOURCES})
add_library(Halide::Julia ALIAS Halide_Julia)
set_target_properties(Halide_Julia
                      PROPERTIES
                      LIBRARY_OUTPUT_NAME halide
                      EXPORT_NAME Julia)

target_include_directories(Halide_Julia PRIVATE ${jlcxx_SOURCE_DIR}/include)
target_link_libraries(Halide_Julia PRIVATE Halide::Halide)
target_link_libraries(Halide_Julia PUBLIC cxxwrap_julia)

install(TARGETS
    Halide_Julia
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION lib
)
