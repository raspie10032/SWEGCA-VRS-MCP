# Runs after swegca_core is built (CMakeLists.txt, "Core isolation").
# Fails closed when the core archive could reach code another layer
# supplies:
# - it names a swegca::vrs symbol (defined, undefined or weak);
# - it holds a weak reference, which links without a definition and so
#   lets another layer supply one later, whatever its name;
# - it refers to a swegca symbol it does not define;
# - its whole contents do not link into a program with only the compiler's
#   own runtime libraries, so a strong reference under any other name (an
#   extern "C" hook, another namespace) must be satisfied by them.
# The symbol listing must name at least one swegca definition: a lister
# that prints nothing (a stand-in tool, bitcode it cannot read) is refused.
# On failure the archive is removed, so the next build links it again and
# repeats this check. What each compilation opened is checked by
# SwegcaCoreCompile.cmake.
#
# Inputs: CORE_ARCHIVE, CORE_COMPILER_KIND (gnu or msvc), CORE_SYMBOL_TOOL,
# CORE_COMPILER, CORE_WORK_DIR, CORE_APPLE (0 or 1), CORE_TOOLCHAIN_FLAGS
# ("|"-joined, may be empty), CORE_PATH_FILE (configure's PATH).
cmake_minimum_required(VERSION 3.20)

foreach(input IN ITEMS CORE_ARCHIVE CORE_COMPILER_KIND CORE_SYMBOL_TOOL CORE_COMPILER
        CORE_WORK_DIR CORE_APPLE CORE_PATH_FILE)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "core isolation check: ${input} is missing")
    endif()
endforeach()

function(core_isolation_fail reason)
    file(REMOVE "${CORE_ARCHIVE}")
    message(FATAL_ERROR "core isolation check: ${reason}\n"
        "The SWEGCA core must not depend on the VRS; ${CORE_ARCHIVE} was removed.")
endfunction()

# The link runs the compiler, so it runs in the clean environment too.
if(NOT CORE_COMPILER_KIND STREQUAL "msvc")
    include("${CMAKE_CURRENT_LIST_DIR}/SwegcaCoreEnvironment.cmake")
    swegca_core_clean_environment(core_isolation_fail)
endif()

if(CORE_COMPILER_KIND STREQUAL "msvc")
    # Not exercised on the development host (no MSVC there).
    execute_process(COMMAND "${CORE_SYMBOL_TOOL}" /nologo /SYMBOLS "${CORE_ARCHIVE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE errors)
    set(vrs_symbol_pattern "swegca::vrs|@vrs@swegca@")
else()
    execute_process(COMMAND "${CORE_SYMBOL_TOOL}" -C "${CORE_ARCHIVE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE errors)
    set(vrs_symbol_pattern "swegca::vrs")
endif()
if(NOT result EQUAL 0)
    core_isolation_fail("listing the symbols of the core archive failed:\n${errors}")
endif()
string(REGEX MATCH "[^\n]*(${vrs_symbol_pattern})[^\n]*" vrs_symbol "${symbols}")
if(vrs_symbol)
    core_isolation_fail("the core archive names a VRS symbol: ${vrs_symbol}")
endif()

# References and definitions by raw (mangled) name: those use only
# [A-Za-z0-9_.$?@], so each is one CMake list element.
if(CORE_COMPILER_KIND STREQUAL "msvc")
    # Not exercised on the development host (no MSVC there). A dumpbin line:
    # "<index> <value> <section> <type> <class> | <name>", section UNDEF
    # for a reference; a weak external is class "WeakExternal".
    set(raw_symbols "${symbols}")
    set(weak_pattern "^[^|]*WeakExternal[ \t]*[|][ \t]*([^ \t(]+)")
    set(undefined_pattern "^[^|]* UNDEF [^|]*External[ \t]*[|][ \t]*([^ \t(]*@swegca@[^ \t(]*)")
    set(defined_pattern "^[^|]* SECT[0-9A-Fa-f]+ [^|]*External[ \t]*[|][ \t]*([^ \t(]*@swegca@[^ \t(]*)")
else()
    execute_process(COMMAND "${CORE_SYMBOL_TOOL}" "${CORE_ARCHIVE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE raw_symbols ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        core_isolation_fail("listing the symbols of the core archive failed:\n${errors}")
    endif()
    # nm: "[value] <type> <name>"; U is a reference, w and v weak ones.
    set(weak_pattern "^[ \t0-9A-Fa-f]*[wv] ([^ \t]+)$")
    set(undefined_pattern "^[ \t0-9A-Fa-f]*U ([^ \t]*6swegca[^ \t]*)$")
    set(defined_pattern "^[ \t0-9A-Fa-f]*[A-TV-Za-uxyz] ([^ \t]*6swegca[^ \t]*)$")
endif()
string(REPLACE ";" "," raw_symbols "${raw_symbols}")
string(REPLACE "\n" ";" symbol_lines "${raw_symbols}")
set(defined_symbols)
set(undefined_symbols)
foreach(symbol_line IN LISTS symbol_lines)
    if(symbol_line MATCHES "${weak_pattern}")
        core_isolation_fail("the core archive holds a weak reference to ${CMAKE_MATCH_1}")
    elseif(symbol_line MATCHES "${undefined_pattern}")
        list(APPEND undefined_symbols "${CMAKE_MATCH_1}")
    elseif(symbol_line MATCHES "${defined_pattern}")
        list(APPEND defined_symbols "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT defined_symbols)
    core_isolation_fail("the symbol listing names no swegca definition; ${CORE_SYMBOL_TOOL} cannot read the core archive")
endif()
foreach(undefined_symbol IN LISTS undefined_symbols)
    if(NOT undefined_symbol IN_LIST defined_symbols)
        core_isolation_fail("the core archive refers to ${undefined_symbol}, which it does not define")
    endif()
endforeach()

# Every member linked into one program with the compiler's own runtime only.
# The core's compile flags come along (a coverage or profiling flag brings
# its runtime); one that changes what the link resolves (a linker option, a
# library or its path, a specs file, another linker, an undefined-symbol or
# script option, a response file) is refused rather than passed.
# CMake's own toolchain settings for the link (CORE_TOOLCHAIN_FLAGS: the
# compiler's extra arguments, sysroot, target, external toolchain, Apple
# architectures) come first and are trusted, as for every link CMake runs.
separate_arguments(link_flags NATIVE_COMMAND "${CORE_LINK_FLAGS}")
foreach(link_flag IN LISTS link_flags)
    if(link_flag MATCHES "^(-Wl,|-Xlinker$|-l|-L|-B|-specs|--specs|-fuse-ld|-nostdlib|-nodefaultlibs|-nostartfiles|-shared$|-r$|-u$|-T|@|-z$|--sysroot|-isysroot|/link$|-link$|/DEFAULTLIB|/NODEFAULTLIB)")
        core_isolation_fail("the core compile flag `${link_flag}` could change what the link check resolves")
    endif()
endforeach()
string(REPLACE "|" ";" toolchain_flags "${CORE_TOOLCHAIN_FLAGS}")
set(link_main "${CORE_WORK_DIR}/link_check_main.cpp")
file(WRITE "${link_main}" "int main() { return 0; }\n")
if(CORE_COMPILER_KIND STREQUAL "msvc")
    # Not exercised on the development host (no MSVC there).
    set(link_command "${CORE_COMPILER}" /nologo "${link_main}"
        "/Fe${CORE_WORK_DIR}/link_check.exe" "/Fo${CORE_WORK_DIR}/link_check_main.obj"
        /link "/WHOLEARCHIVE:${CORE_ARCHIVE}")
elseif(CORE_APPLE)
    set(link_command "${CORE_COMPILER}" ${toolchain_flags} ${link_flags} "${link_main}"
        "-Wl,-force_load,${CORE_ARCHIVE}" -o "${CORE_WORK_DIR}/link_check")
else()
    set(link_command "${CORE_COMPILER}" ${toolchain_flags} ${link_flags} "${link_main}" -Wl,--whole-archive
        "${CORE_ARCHIVE}" -Wl,--no-whole-archive -o "${CORE_WORK_DIR}/link_check")
endif()
execute_process(COMMAND ${link_command} WORKING_DIRECTORY "${CORE_WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    core_isolation_fail("the core archive does not link with the compiler's runtime alone:\n${output}")
endif()
