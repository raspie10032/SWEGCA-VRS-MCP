cmake_minimum_required(VERSION 3.20)
project(SWEGCACore LANGUAGES CXX)

# The core contains only the three-state SWEGCA verifier and its value types.
# The VRS modules below use it, but are not part of the verifier.
# The core is compiled from copies of its files under an include root that
# holds nothing else (see "Core isolation" below), so the core build itself
# cannot reach a VRS file; its users include it from cpp/.
set(swegca_core_sources
    cpp/swegca_architecture/sha256.cpp
    cpp/swegca_architecture/strong_types.cpp
    cpp/swegca_architecture/evidence_rules.cpp
)
set(core_isolation_dir "${CMAKE_CURRENT_BINARY_DIR}/swegca_core_isolation")
set(core_isolation_root "${core_isolation_dir}/include")
set(swegca_core_isolated_sources)
foreach(core_source IN LISTS swegca_core_sources)
    get_filename_component(core_source_name "${core_source}" NAME)
    list(APPEND swegca_core_isolated_sources
        "${core_isolation_root}/swegca_architecture/${core_source_name}")
endforeach()
add_library(swegca_core STATIC ${swegca_core_isolated_sources})
target_include_directories(swegca_core
    PRIVATE "${core_isolation_root}"
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cpp")
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
    cpp/swegca_vrs/cognition_runner.cpp
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
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/swegca_architecture/*.cpp")
file(GLOB vrs_directory_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/swegca_vrs/*.cpp")
# swegca_core compiles the isolated copies of these sources (above).
set(core_sources ${swegca_core_sources})
get_target_property(vrs_sources swegca_vrs_candidate SOURCES)
list(TRANSFORM core_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM vrs_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(SORT core_sources)
list(SORT vrs_sources)
list(SORT architecture_sources)
list(SORT vrs_directory_sources)
if(NOT core_sources STREQUAL architecture_sources)
    message(FATAL_ERROR "Every cpp/swegca_architecture source must be in swegca_core, and only those")
endif()
if(NOT vrs_sources STREQUAL vrs_directory_sources)
    message(FATAL_ERROR "Every cpp/swegca_vrs source must be in swegca_vrs_candidate, and only those")
endif()

# The core is the verifier alone: nothing under cpp/swegca_architecture may
# include a VRS header. Both directories stay flat so the globs above see
# every source, and a change to a core file reruns this check.
foreach(layer_directory IN ITEMS swegca_architecture swegca_vrs)
    file(GLOB layer_entries LIST_DIRECTORIES true
        "${CMAKE_CURRENT_SOURCE_DIR}/cpp/${layer_directory}/*")
    foreach(layer_entry IN LISTS layer_entries)
        if(IS_DIRECTORY "${layer_entry}")
            message(FATAL_ERROR "${layer_entry}: cpp/${layer_directory} must stay flat")
        endif()
    endforeach()
endforeach()
file(GLOB architecture_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/swegca_architecture/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${architecture_files})
foreach(architecture_file IN LISTS architecture_files)
    file(STRINGS "${architecture_file}" vrs_includes
        REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]swegca_vrs/")
    if(vrs_includes)
        message(FATAL_ERROR "${architecture_file} includes a VRS header; the core must not depend on the VRS")
    endif()
endforeach()

# Core isolation. The core must not depend on the VRS in any form. Three
# checks, each failing closed:
# 1. Configure: every core file is a regular .hpp or .cpp file (no symlink),
#    has no carriage return, and uses only two preprocessor directives:
#    `#pragma once`, and `#include` of another core header by its
#    "swegca_architecture/<name>.hpp" path or of a standard header by
#    <name>. A spliced line (a backslash before the line end, spaces
#    allowed), a directive after a block comment and the `%:` digraph stop
#    the configure too. A line holding `[`, `]` or `;` is refused because a
#    CMake list could merge several lines into it. The text `vrs`, the
#    tokens that reach a file or a directive outside `#include` (inline
#    assembly, `_Pragma`, `__has_include`, `__has_embed`) and module
#    `import`/`module` lines are refused anywhere in a core file.
# 2. Build: swegca_core compiles the copies made here, with this root as its
#    only project include path (the compiler adds its own), so the core
#    build cannot open cpp/ or cpp/swegca_vrs. Each core header is also
#    compiled on its own. Every one of these compilations runs through
#    SwegcaCoreCompile.cmake, which reads the dependency list of that very
#    compilation (its own flags, definitions, configuration and
#    environment) and refuses a file outside this directory and the
#    compiler's own include directories; the object is then removed, so
#    the next build compiles and checks it again.
# 3. After swegca_core is built, SwegcaCoreIsolationCheck.cmake refuses any
#    swegca::vrs symbol in the archive, any weak reference, any reference to
#    a swegca symbol the archive does not define, and an archive that does
#    not link whole with the compiler's runtime alone (a reference under
#    any other name). On failure it removes the archive, so the next build
#    repeats the check.
# cpp/ is the include root of the core's users. A file there named like a
# standard header (no extension, e.g. cpp/cstdint) would replace it in every
# translation unit that includes a core header, so none is allowed.
file(GLOB include_root_entries LIST_DIRECTORIES false CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*")
foreach(include_root_entry IN LISTS include_root_entries)
    get_filename_component(include_root_name "${include_root_entry}" NAME)
    if(NOT include_root_name MATCHES "[.]")
        message(FATAL_ERROR "${include_root_entry}: a file without an extension in cpp/ could stand in for a standard header")
    endif()
endforeach()
string(ASCII 13 carriage_return)
string(ASCII 92 backslash)
set(core_isolation_files)
set(core_header_checks)
foreach(architecture_file IN LISTS architecture_files)
    get_filename_component(architecture_name "${architecture_file}" NAME)
    if(IS_SYMLINK "${architecture_file}")
        message(FATAL_ERROR "${architecture_file}: a core file must not be a symbolic link")
    endif()
    if(NOT architecture_name MATCHES "^[A-Za-z0-9_]+[.](hpp|cpp)$")
        message(FATAL_ERROR "${architecture_file}: cpp/swegca_architecture holds only <name>.hpp and <name>.cpp files")
    endif()
    file(READ "${architecture_file}" architecture_content)
    string(FIND "${architecture_content}" "${carriage_return}" carriage_return_at)
    if(NOT carriage_return_at EQUAL -1)
        message(FATAL_ERROR "${architecture_file}: a core file must use LF line ends only")
    endif()
    string(TOLOWER "${architecture_content}" architecture_lower)
    string(FIND "${architecture_lower}" "vrs" vrs_at)
    if(NOT vrs_at EQUAL -1)
        message(FATAL_ERROR "${architecture_file}: the text `vrs` is not allowed in a core file")
    endif()
    if(architecture_content MATCHES
            "(^|[^A-Za-z0-9_])(asm|_asm|__asm|__asm__|_Pragma|__pragma|__has_include|__has_embed)([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR "${architecture_file}: `${CMAKE_MATCH_2}` is not allowed in a core file")
    endif()
    if(architecture_content MATCHES "(^|\n|[;{}])[ \t]*(export[ \t]+)?(import|module)([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR "${architecture_file}: a module `${CMAKE_MATCH_3}` line is not allowed in a core file")
    endif()
    file(STRINGS "${architecture_file}" directive_lines
        REGEX "^[ \t]*(#|%:)|[*]/[ \t]*(#|%:)|[${backslash}][ \t]*$")
    foreach(directive_line IN LISTS directive_lines)
        if(directive_line MATCHES "[][;]" OR NOT directive_line MATCHES
                "^[ \t]*#[ \t]*(pragma[ \t]+once|include[ \t]*(\"swegca_architecture/[A-Za-z0-9_]+[.]hpp\"|<[A-Za-z0-9_]+>))[ \t]*(//.*)?$")
            message(FATAL_ERROR "${architecture_file}: `${directive_line}` is not an allowed core directive; the core may include only core and standard headers")
        endif()
    endforeach()
    set(isolated_file "${core_isolation_root}/swegca_architecture/${architecture_name}")
    configure_file("${architecture_file}" "${isolated_file}" COPYONLY)
    list(APPEND core_isolation_files "${isolated_file}")
    if(architecture_name MATCHES "[.]hpp$")
        set(header_check "${core_isolation_dir}/header_checks/${architecture_name}.cpp")
        file(CONFIGURE OUTPUT "${header_check}"
            CONTENT "#include \"swegca_architecture/${architecture_name}\"\n")
        list(APPEND core_header_checks "${header_check}")
    endif()
endforeach()
# Remove copies whose core file is gone, and nothing else, so an unchanged
# core is not rebuilt at every configure.
file(GLOB isolation_entries LIST_DIRECTORIES true
    "${core_isolation_root}/swegca_architecture/*"
    "${core_isolation_dir}/header_checks/*")
foreach(isolation_entry IN LISTS isolation_entries)
    if(NOT isolation_entry IN_LIST core_isolation_files AND
       NOT isolation_entry IN_LIST core_header_checks)
        file(REMOVE_RECURSE "${isolation_entry}")
    endif()
endforeach()
file(GLOB isolation_top LIST_DIRECTORIES true "${core_isolation_dir}/*" "${core_isolation_root}/*")
foreach(isolation_entry IN LISTS isolation_top)
    if(NOT isolation_entry STREQUAL "${core_isolation_root}" AND
       NOT isolation_entry STREQUAL "${core_isolation_dir}/header_checks" AND
       NOT isolation_entry STREQUAL "${core_isolation_root}/swegca_architecture")
        file(REMOVE_RECURSE "${isolation_entry}")
    endif()
endforeach()

add_library(swegca_core_isolation OBJECT ${core_header_checks})
target_include_directories(swegca_core_isolation PRIVATE "${core_isolation_root}")
target_compile_features(swegca_core_isolation PRIVATE cxx_std_20)
set_target_properties(swegca_core_isolation PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(swegca_core_isolation PRIVATE /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(swegca_core_isolation PRIVATE -ffp-contract=off -fno-fast-math)
endif()
add_dependencies(swegca_core swegca_core_isolation)

# The compiler's own include directories are allowed, but not one inside
# the source or build tree, nor one holding either: an environment include
# path such as CPLUS_INCLUDE_PATH=cpp/swegca_vrs, or =/tmp for a tree under
# /tmp, would turn VRS files into "compiler" headers. Of the isolation
# directory only the exact files made here are allowed, so a file added to
# it after configure (e.g. forced in with -include) is refused.
# For a GNU-style compiler the directories are asked of the compiler itself,
# run in the clean environment with no project flag (SwegcaCoreCompilerRoots
# says why CMake's implicit list is not used); the launcher and the link
# check run it in that environment too.
if(NOT MSVC AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    # CMake passes -Xclang options to a GNU-style Clang targeting the MSVC
    # ABI, and the launcher refuses -Xclang (SwegcaCoreCompile.cmake).
    message(FATAL_ERROR "The core isolation check does not support ${CMAKE_CXX_COMPILER} (GNU-style driver for the MSVC ABI); use clang-cl or MSVC")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "QCC")
    # QCC writes its dependency listing through -Wp,-MD, which the launcher
    # refuses (it could pass any preprocessor option unseen).
    message(FATAL_ERROR "The core isolation check does not support QCC (${CMAKE_CXX_COMPILER})")
endif()
if(CMAKE_APPLE_ARCH_SYSROOTS)
    # One Apple SDK per architecture is passed with -Xarch_, which the
    # launcher refuses, and one -isysroot cannot stand for them in the checks.
    message(FATAL_ERROR "The core isolation check does not support per-architecture Apple SDKs (CMAKE_APPLE_ARCH_SYSROOTS); build one architecture at a time")
endif()

# The toolchain arguments CMake gives every compilation (COMPILE) or link
# (LINK) of the core: the extra arguments of the compiler command
# (CMAKE_CXX_COMPILER_ARG1), CMake's sysroot, target and external toolchain,
# each spelled as this compiler's CMake module spells it and left out when
# the module defines no spelling (CMake ignores it then too), and on Apple
# the SDK, the deployment target and, for the link, the architectures.
function(swegca_core_toolchain_flags out which)
    set(flags)
    if(CMAKE_CXX_COMPILER_ARG1)
        separate_arguments(arg1 NATIVE_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
        list(APPEND flags ${arg1})
    endif()
    set(sysroot "${CMAKE_SYSROOT_${which}}")
    if(NOT sysroot)
        set(sysroot "${CMAKE_SYSROOT}")
    endif()
    foreach(setting IN ITEMS SYSROOT TARGET EXTERNAL_TOOLCHAIN)
        if(setting STREQUAL "SYSROOT")
            set(value "${sysroot}")
        else()
            set(value "${CMAKE_CXX_COMPILER_${setting}}")
        endif()
        set(option "${CMAKE_CXX_COMPILE_OPTIONS_${setting}}")
        if(value AND option)
            if(option MATCHES "^(.*[^ ]) +$")
                # "-target ": the value is a separate argument.
                list(APPEND flags "${CMAKE_MATCH_1}" "${value}")
            else()
                list(APPEND flags "${option}${value}")
            endif()
        endif()
    endforeach()
    if(APPLE AND CMAKE_CXX_SYSROOT_FLAG)
        # With no SDK named, CMake still passes the one `xcrun
        # --show-sdk-path` printed (Darwin-Initialize.cmake) to a Clang that
        # has no default SDK (CMAKE_CXX_COMPILER_APPLE_SYSROOT_REQUIRED).
        set(osx_sysroot "${CMAKE_OSX_SYSROOT}")
        if(NOT osx_sysroot AND CMAKE_CXX_COMPILER_APPLE_SYSROOT_REQUIRED)
            set(osx_sysroot "${_CMAKE_OSX_SYSROOT_PATH}")
        endif()
        if(osx_sysroot)
            list(APPEND flags "${CMAKE_CXX_SYSROOT_FLAG}" "${osx_sysroot}")
        endif()
    endif()
    if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG)
        # The deployment target, which the clean environment's scripts no
        # longer take from MACOSX_DEPLOYMENT_TARGET.
        list(APPEND flags "${CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG}${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(APPLE AND which STREQUAL "LINK")
        # The archive holds CMAKE_OSX_ARCHITECTURES; a program for another
        # architecture would skip it and link nothing. (Not in the include
        # probe: -E refuses more than one -arch.)
        foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND flags -arch "${arch}")
        endforeach()
    endif()
    foreach(flag IN LISTS flags)
        if(flag MATCHES "[|]")
            message(FATAL_ERROR "The core isolation check cannot pass the toolchain argument `${flag}`, which holds `|`")
        endif()
    endforeach()
    string(REPLACE ";" "|" flags "${flags}")
    set(${out} "${flags}" PARENT_SCOPE)
endfunction()

# The PATH present at the first configure, when CMake discovered the
# toolchain (some of whose paths may instead have been given to it), is
# recorded with that toolchain (and cached, like the compiler); for a
# GNU-style compiler the probe, the launcher and the link check run the
# compiler with that PATH, so where the compiler searches PATH for its
# assembler and linker it sees what configure saw, whatever PATH the build
# is started with.
# (MSVC is not run in a clean environment: it gets the build's PATH, INCLUDE
# and LIB, and its include roots are CMake's measured ones.) It is cached
# once: a configure the build reruns by itself runs in the build's
# environment and must not replace it. A new PATH needs a fresh build tree,
# as a new compiler does. An empty PATH is refused: the compiler would then
# look for its parts on the system's default search path, which configure
# did not use. The file lies in the build tree, which already names the
# commands the build runs, so it needs no check of its own.
if(NOT DEFINED CACHE{SWEGCA_CORE_TOOLCHAIN_PATH})
    if("$ENV{PATH}" STREQUAL "")
        message(FATAL_ERROR "The core isolation check needs the PATH present at configure; configure with PATH set")
    endif()
    set(SWEGCA_CORE_TOOLCHAIN_PATH "$ENV{PATH}" CACHE INTERNAL
        "PATH present when the first configure discovered the toolchain (core isolation)")
endif()
set(core_path_file "${CMAKE_CURRENT_BINARY_DIR}/swegca_core_path.txt")
file(WRITE "${core_path_file}" "${SWEGCA_CORE_TOOLCHAIN_PATH}")

if(MSVC)
    set(core_compiler_roots ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
else()
    # Only the flags that choose the toolchain's own headers reach the
    # probe: a -stdlib= choice and the toolchain arguments CMake passes to
    # every compilation.
    separate_arguments(core_cxx_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
    set(core_probe_flags)
    foreach(core_cxx_flag IN LISTS core_cxx_flags)
        if(core_cxx_flag MATCHES "^-stdlib=")
            if(core_cxx_flag MATCHES "[|]")
                message(FATAL_ERROR "The core isolation check cannot pass `${core_cxx_flag}`, which holds `|`")
            endif()
            list(APPEND core_probe_flags "${core_cxx_flag}")
        endif()
    endforeach()
    string(REPLACE ";" "|" core_probe_flags "${core_probe_flags}")
    swegca_core_toolchain_flags(core_compile_toolchain_flags COMPILE)
    if(core_compile_toolchain_flags)
        if(core_probe_flags)
            string(APPEND core_probe_flags "|")
        endif()
        string(APPEND core_probe_flags "${core_compile_toolchain_flags}")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/swegca_core_probe.cpp" "")
    execute_process(COMMAND "${CMAKE_COMMAND}"
            "-DCORE_PATH_FILE=${core_path_file}"
            "-DCORE_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCORE_PROBE_FLAGS=${core_probe_flags}"
            "-DCORE_PROBE_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/swegca_core_probe.cpp"
            "-DCORE_ROOTS_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/swegca_core_roots.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SwegcaCoreCompilerRoots.cmake"
        RESULT_VARIABLE core_probe_result)
    if(NOT core_probe_result EQUAL 0)
        message(FATAL_ERROR "The core isolation check could not ask ${CMAKE_CXX_COMPILER} for its include directories")
    endif()
    file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/swegca_core_roots.txt" core_compiler_roots)
endif()
# A toolchain's standard include directories (e.g. Android's), which CMake
# adds to every compilation, are the toolchain's own headers too.
list(APPEND core_compiler_roots ${CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES})
set(core_allowed_roots)
foreach(implicit_directory IN LISTS core_compiler_roots)
    file(REAL_PATH "${implicit_directory}" implicit_real)
    foreach(project_directory IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
        file(REAL_PATH "${project_directory}" project_real)
        string(FIND "${implicit_real}/" "${project_real}/" project_at)
        string(FIND "${project_real}/" "${implicit_real}/" holder_at)
        if(project_at EQUAL 0 OR holder_at EQUAL 0)
            message(FATAL_ERROR "${implicit_directory}: a compiler include directory inside or above ${project_directory} would bypass the core isolation")
        endif()
    endforeach()
    list(APPEND core_allowed_roots "${implicit_directory}")
endforeach()

# Only generators that run a compiler launcher can check each compilation.
if(NOT CMAKE_GENERATOR MATCHES "Makefiles|Ninja")
    message(FATAL_ERROR "The core isolation check needs a Makefile or Ninja generator, not ${CMAKE_GENERATOR}")
endif()
if(MSVC)
    set(core_compiler_kind msvc)
    get_filename_component(core_linker_directory "${CMAKE_LINKER}" DIRECTORY)
    find_program(core_symbol_tool dumpbin HINTS "${core_linker_directory}")
else()
    set(core_compiler_kind gnu)
    set(core_symbol_tool "${CMAKE_NM}")
endif()
if(NOT core_symbol_tool)
    message(FATAL_ERROR "No symbol lister (nm or dumpbin) was found; the core isolation check cannot run")
endif()
string(REPLACE ";" "|" core_allowed_roots_arg "${core_allowed_roots}")
string(REPLACE ";" "|" core_allowed_files_arg "${core_isolation_files};${core_header_checks}")
# The launcher replaces any CMAKE_CXX_COMPILER_LAUNCHER (e.g. ccache) for
# these two targets: a launcher runs the compilation it is given, so only
# this one may stand between the build and the compiler. Unity builds and
# precompiled headers would compile a generated file outside the root.
set_target_properties(swegca_core swegca_core_isolation PROPERTIES
    CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-DCORE_COMPILER_KIND=${core_compiler_kind};-DCORE_ALLOWED_ROOTS=${core_allowed_roots_arg};-DCORE_ALLOWED_FILES=${core_allowed_files_arg};-DCORE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cpp/swegca_architecture;-DCORE_PATH_FILE=${core_path_file};-P;${CMAKE_CURRENT_SOURCE_DIR}/cmake/SwegcaCoreCompile.cmake;--"
    UNITY_BUILD OFF
    DISABLE_PRECOMPILE_HEADERS ON)
# The link check compiles its program with the core's compile flags of the
# configuration being built (e.g. -fprofile-arcs needs its runtime); the
# check refuses any of them that changes what the link may resolve.
set(core_link_flags "${CMAKE_CXX_FLAGS}")
set(core_link_configs Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES})
list(REMOVE_DUPLICATES core_link_configs)
foreach(core_link_config IN LISTS core_link_configs)
    string(TOUPPER "${core_link_config}" core_link_config_upper)
    string(APPEND core_link_flags
        "$<$<CONFIG:${core_link_config}>: ${CMAKE_CXX_FLAGS_${core_link_config_upper}}>")
endforeach()
if(MSVC)
    set(core_link_toolchain_flags)
else()
    swegca_core_toolchain_flags(core_link_toolchain_flags LINK)
endif()
add_custom_command(TARGET swegca_core POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DCORE_ARCHIVE=$<TARGET_FILE:swegca_core>"
        "-DCORE_COMPILER_KIND=${core_compiler_kind}"
        "-DCORE_SYMBOL_TOOL=${core_symbol_tool}"
        "-DCORE_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCORE_LINK_FLAGS=${core_link_flags}"
        "-DCORE_TOOLCHAIN_FLAGS=${core_link_toolchain_flags}"
        "-DCORE_WORK_DIR=${core_isolation_dir}"
        "-DCORE_PATH_FILE=${core_path_file}"
        "-DCORE_APPLE=$<BOOL:${APPLE}>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SwegcaCoreIsolationCheck.cmake"
    VERBATIM)

include(CTest)
if(BUILD_TESTING)
    add_executable(swegca_evidence_core_test cpp/tests/evidence_core_test.cpp)
    target_link_libraries(swegca_evidence_core_test PRIVATE swegca_core)
    add_test(NAME swegca_evidence_core COMMAND swegca_evidence_core_test)

    add_executable(swegca_evidence_core_bench cpp/tests/evidence_core_bench.cpp)
    target_link_libraries(swegca_evidence_core_bench PRIVATE swegca_core)
endif()
