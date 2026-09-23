cmake_minimum_required(VERSION 3.20)
project(SWEGCACore LANGUAGES CXX)

# The core contains only the three-state SWEGCA verifier and its value types.
# The VRS modules below use it, but are not part of the verifier.
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

# Compile every currently implemented C++ VRS module together. This is a
# candidate library, not a complete resident VRS: the four-stage read path,
# Main publication and the writer still need their product composition.
# Keep this list explicit so a new translation unit cannot silently bypass
# the build when the source layout changes.
add_library(swegca_vrs_candidate STATIC
    cpp/swegca_vrs/arbiter_rules.cpp
    cpp/swegca_vrs/authority.cpp
    cpp/swegca_vrs/cognition.cpp
    cpp/swegca_vrs/cognitive_state.cpp
    cpp/swegca_vrs/evidence_accumulator.cpp
    cpp/swegca_vrs/evidence_gate.cpp
    cpp/swegca_vrs/evidence_stages.cpp
    cpp/swegca_vrs/experience.cpp
    cpp/swegca_vrs/gate_rules.cpp
    cpp/swegca_vrs/journal_file_io.cpp
    cpp/swegca_vrs/journal_format.cpp
    cpp/swegca_vrs/journal_store.cpp
    cpp/swegca_vrs/identity_types.cpp
    cpp/swegca_vrs/main_owner.cpp
    cpp/swegca_vrs/native_tensor.cpp
    cpp/swegca_vrs/proposal.cpp
    cpp/swegca_vrs/proposal_arbiter.cpp
    cpp/swegca_vrs/role_registry.cpp
    cpp/swegca_vrs/scalar_codec.cpp
    cpp/swegca_vrs/vrs_strength_compile.cpp
    cpp/swegca_vrs/resource_budget.cpp
)
target_link_libraries(swegca_vrs_candidate PUBLIC swegca_core)
set_target_properties(swegca_vrs_candidate PROPERTIES CXX_EXTENSIONS OFF)

file(GLOB architecture_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/swegca_architecture/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/swegca_vrs/*.cpp")
get_target_property(core_sources swegca_core SOURCES)
get_target_property(vrs_sources swegca_vrs_candidate SOURCES)
list(APPEND core_sources ${vrs_sources})
list(TRANSFORM core_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(SORT core_sources)
list(SORT architecture_sources)
if(NOT core_sources STREQUAL architecture_sources)
    message(FATAL_ERROR "Every SWEGCA core and VRS source must be assigned to a CMake target")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(swegca_evidence_core_test cpp/tests/evidence_core_test.cpp)
    target_link_libraries(swegca_evidence_core_test PRIVATE swegca_core)
    add_test(NAME swegca_evidence_core COMMAND swegca_evidence_core_test)

    add_executable(swegca_evidence_core_bench cpp/tests/evidence_core_bench.cpp)
    target_link_libraries(swegca_evidence_core_bench PRIVATE swegca_core)
endif()
