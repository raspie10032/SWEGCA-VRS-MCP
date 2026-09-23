cmake_minimum_required(VERSION 3.20)
project(SWEGCACore LANGUAGES CXX)

# This target is deliberately limited to the three-state SWEGCA verifier.
# VRS journal, experience, gate, arbiter, writer and memory stages are separate.
add_library(swegca_core STATIC
    cpp/swegca_architecture/sha256.cpp
    cpp/swegca_architecture/strong_types.cpp
    cpp/swegca_architecture/evidence_rules.cpp
)
target_include_directories(swegca_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cpp")
target_compile_features(swegca_core PUBLIC cxx_std_20)
set_target_properties(swegca_core PROPERTIES CXX_EXTENSIONS OFF)

# The kernel is inline in its header, so callers must use the same floating
# point mode as the library. In particular, contraction and fast math change
# evidence thresholds close to the decision boundary.
if(MSVC)
    target_compile_options(swegca_core PUBLIC /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(swegca_core PUBLIC -ffp-contract=off -fno-fast-math)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(swegca_evidence_core_test cpp/tests/evidence_core_test.cpp)
    target_link_libraries(swegca_evidence_core_test PRIVATE swegca_core)
    add_test(NAME swegca_evidence_core COMMAND swegca_evidence_core_test)

    add_executable(swegca_evidence_core_bench cpp/tests/evidence_core_bench.cpp)
    target_link_libraries(swegca_evidence_core_bench PRIVATE swegca_core)
endif()
