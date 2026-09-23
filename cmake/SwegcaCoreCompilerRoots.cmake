# The core's allowed include roots (CMakeLists.txt, "Core isolation"): the
# directories a GNU-style compiler searches for <...> when it runs in the
# clean environment with none of the project's flags but those choosing the
# toolchain's own headers (-stdlib=, and the toolchain arguments CMake
# passes to every compilation: the compiler command's own, sysroot, target
# and external toolchain). CMake's CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES is not
# used: it is measured with the configure environment and the project's
# flags, so CPATH, COMPILER_PATH, CCC_OVERRIDE_OPTIONS or an -isystem in
# CXXFLAGS would add a directory of their choosing to it. The probe runs in
# the C locale, so the compiler prints the list's untranslated markers.
#
# Inputs: CORE_COMPILER, CORE_PROBE_FLAGS ("|"-joined, may be empty),
# CORE_PROBE_SOURCE (an empty file), CORE_ROOTS_OUTPUT (written: one
# directory per line).
cmake_minimum_required(VERSION 3.20)

foreach(input IN ITEMS CORE_COMPILER CORE_PROBE_SOURCE CORE_ROOTS_OUTPUT)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "core include probe: ${input} is missing")
    endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/SwegcaCoreEnvironment.cmake")
swegca_core_clean_environment()
set(ENV{LC_ALL} C)
unset(ENV{LANGUAGE})

string(REPLACE "|" ";" probe_flags "${CORE_PROBE_FLAGS}")
execute_process(COMMAND "${CORE_COMPILER}" ${probe_flags} -E -v -x c++ "${CORE_PROBE_SOURCE}"
    OUTPUT_QUIET ERROR_VARIABLE log RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "core include probe: ${CORE_COMPILER} -E -v failed:\n${log}")
endif()

# GCC and Clang print the search list between these two lines, one
# directory per line after a space (Apple marks framework directories).
string(REPLACE ";" "\n" log "${log}")
string(REPLACE "\n" ";" lines "${log}")
set(roots)
set(in_list FALSE)
set(seen_end FALSE)
foreach(line IN LISTS lines)
    if(line MATCHES "^#include [<\"][.][.][.][>\"] search starts here:$")
        set(in_list TRUE)
    elseif(line STREQUAL "End of search list.")
        set(in_list FALSE)
        set(seen_end TRUE)
    elseif(in_list AND line MATCHES "^ (.+)$")
        string(REGEX REPLACE " [(]framework directory[)]$" "" directory "${CMAKE_MATCH_1}")
        if(directory MATCHES "[|]")
            message(FATAL_ERROR "core include probe: an include directory holds `|`: ${directory}")
        endif()
        file(REAL_PATH "${directory}" real_directory)
        list(APPEND roots "${real_directory}")
    endif()
endforeach()
if(NOT seen_end OR NOT roots)
    message(FATAL_ERROR "core include probe: no include search list in the output of ${CORE_COMPILER} -E -v:\n${log}")
endif()
list(REMOVE_DUPLICATES roots)
list(JOIN roots "\n" text)
file(WRITE "${CORE_ROOTS_OUTPUT}" "${text}\n")
