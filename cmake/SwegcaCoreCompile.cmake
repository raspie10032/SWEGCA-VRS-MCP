# Compiler launcher of the core targets (CMakeLists.txt, "Core isolation").
# Runs the compilation it is given unchanged except for a dependency listing,
# then reads what that compilation opened: the same flags, definitions,
# configuration and environment as the object. (A RULE_LAUNCH_COMPILE
# wrapper, set only by CMake code, runs before this one and is outside what
# this check sees.) A file that is neither one of CORE_ALLOWED_FILES (the
# isolated core copies and header probes) nor under one of
# CORE_ALLOWED_ROOTS (the compiler's own include directories) fails the
# compilation and removes its object, so the next build compiles and checks
# it again.
#
# Inputs: CORE_COMPILER_KIND (gnu or msvc), CORE_ALLOWED_FILES and
# CORE_ALLOWED_ROOTS (each joined with "|"), CORE_SOURCE_DIR (the core
# directory the copies come from); the compiler command follows "--".
cmake_minimum_required(VERSION 3.20)

foreach(input IN ITEMS CORE_COMPILER_KIND CORE_ALLOWED_FILES CORE_SOURCE_DIR)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "core isolation check: ${input} is missing")
    endif()
endforeach()

# The command: every argument after "--". An argument holding ";" cannot be
# kept whole in a CMake list, so it is refused rather than split.
set(command)
set(in_command FALSE)
math(EXPR last_argument "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last_argument})
    set(argument "${CMAKE_ARGV${index}}")
    if(in_command)
        string(FIND "${argument}" ";" semicolon_at)
        if(NOT semicolon_at EQUAL -1)
            message(FATAL_ERROR "core isolation check: a compiler argument holds `;`: ${argument}")
        endif()
        list(APPEND command "${argument}")
    elseif(argument STREQUAL "--")
        set(in_command TRUE)
    endif()
endforeach()
if(NOT command)
    message(FATAL_ERROR "core isolation check: no compiler command after --")
endif()
# Arguments that would make the listing incomplete or hide part of the
# command: dependency modes that leave out system headers or skip missing
# files, options passed straight to the preprocessor, and response files,
# whose contents this launcher does not read.
# Also options that bring in arguments or compiler parts this launcher does
# not see: configuration and specs files, another directory for the
# compiler's own programs, a wrapper around them, arguments passed straight
# to Clang's front end or to one architecture's compilation (-Xarch_), and
# plugins. Also precompiled headers and modules, whose own inputs need not
# appear in the listing, and the long GNU spellings of the dependency modes.
# (CMake itself passes -Xclang to a GNU-style Clang targeting the MSVC ABI,
# and -Xarch_ with per-architecture Apple SDKs; CMakeLists.txt refuses both
# for the core.)
foreach(argument IN LISTS command)
    if(argument MATCHES "^-(M|MM|MMD|MG)$" OR argument MATCHES "^-Wp," OR
       argument STREQUAL "-Xpreprocessor" OR argument MATCHES "^@" OR
       argument MATCHES "^--?config" OR argument MATCHES "^--?specs" OR
       argument MATCHES "^-B" OR argument MATCHES "^--?wrapper" OR
       argument MATCHES "^-Xclang" OR argument MATCHES "^-Xarch_" OR
       argument MATCHES "^-f(pass-)?plugin" OR
       argument MATCHES "^--(write-)?(user-)?dependencies" OR
       argument MATCHES "^--print-missing-file-dependencies" OR
       argument MATCHES "^-include-pch" OR argument MATCHES "^-f(implicit-|prebuilt-)?module")
        message(FATAL_ERROR "core isolation check: `${argument}` is not allowed when compiling the core")
    endif()
endforeach()

# The object and the dependency listing the command already writes.
set(object)
set(dependency_file)
list(LENGTH command argument_count)
math(EXPR last_index "${argument_count} - 1")
foreach(index RANGE ${last_index})
    list(GET command ${index} argument)
    math(EXPR next_index "${index} + 1")
    if(CORE_COMPILER_KIND STREQUAL "msvc")
        if(argument MATCHES "^[-/]Fo(.+)$")
            set(object "${CMAKE_MATCH_1}")
        endif()
    elseif(argument STREQUAL "-o" AND next_index LESS argument_count)
        list(GET command ${next_index} object)
    elseif(argument STREQUAL "-MF" AND next_index LESS argument_count)
        list(GET command ${next_index} dependency_file)
    elseif(argument MATCHES "^-MF(.+)$")
        set(dependency_file "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT object)
    message(FATAL_ERROR "core isolation check: the compiler command names no object")
endif()

function(core_isolation_fail reason)
    file(REMOVE "${object}")
    message(FATAL_ERROR "core isolation check: ${reason}\n"
        "The SWEGCA core must not depend on the VRS; ${object} was removed.")
endfunction()

# The compiler runs in the clean environment the include roots were asked in.
if(NOT CORE_COMPILER_KIND STREQUAL "msvc")
    include("${CMAKE_CURRENT_LIST_DIR}/SwegcaCoreEnvironment.cmake")
    swegca_core_clean_environment(core_isolation_fail)
endif()

if(CORE_COMPILER_KIND STREQUAL "msvc")
    # Not exercised on the development host (no MSVC there).
    set(dependency_file "${object}.swegca_dependencies.json")
    list(APPEND command /sourceDependencies "${dependency_file}")
elseif(NOT dependency_file)
    set(dependency_file "${object}.swegca.d")
    list(APPEND command -MD -MF "${dependency_file}")
else()
    # A rule that names -MF without a mode still gets a listing. The mode
    # added here does not override one before it: GCC and Clang both prefer
    # -MMD to -MD wherever it stands. System headers stay listed because
    # every spelling of -MMD (and -Wp, -Xpreprocessor, -Xclang, -Xarch_,
    # which could pass it on) is refused above.
    list(APPEND command -MD)
endif()
file(REMOVE "${dependency_file}")

execute_process(COMMAND ${command} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    file(REMOVE "${object}")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        cmake_language(EXIT 1)
    endif()
    message(FATAL_ERROR "core isolation check: the compilation failed")
endif()

if(NOT EXISTS "${dependency_file}")
    core_isolation_fail("the compilation wrote no dependency listing ${dependency_file}")
endif()

set(real_roots)
string(REPLACE "|" ";" allowed_roots "${CORE_ALLOWED_ROOTS}")
foreach(root IN LISTS allowed_roots)
    if(EXISTS "${root}")
        file(REAL_PATH "${root}" real_root)
        list(APPEND real_roots "${real_root}/")
    endif()
endforeach()
set(real_files)
string(REPLACE "|" ";" allowed_files "${CORE_ALLOWED_FILES}")
foreach(allowed_file IN LISTS allowed_files)
    if(EXISTS "${allowed_file}")
        file(REAL_PATH "${allowed_file}" real_file)
        list(APPEND real_files "${real_file}")
    endif()
endforeach()
# A directory holding isolated copies holds nothing else: a precompiled
# header put beside a copy (`sha256.hpp.gch`, a file or a directory) would be
# used in the copy's place, and not every compiler lists it. The directories
# are the ones the compiler searches, taken before links are resolved: a
# copy made a link to its core file must not move the check to the core
# directory.
set(copy_directories)
foreach(allowed_file IN LISTS allowed_files)
    get_filename_component(copy_directory "${allowed_file}" DIRECTORY)
    list(APPEND copy_directories "${copy_directory}")
endforeach()
list(REMOVE_DUPLICATES copy_directories)
foreach(copy_directory IN LISTS copy_directories)
    file(GLOB copy_entries LIST_DIRECTORIES true "${copy_directory}/*" "${copy_directory}/.*")
    foreach(copy_entry IN LISTS copy_entries)
        get_filename_component(entry_name "${copy_entry}" NAME)
        if(entry_name STREQUAL "." OR entry_name STREQUAL "..")
            continue()
        endif()
        file(REAL_PATH "${copy_entry}" real_entry)
        if(NOT real_entry IN_LIST real_files)
            core_isolation_fail("${copy_entry} was added beside the isolated core files; reconfigure")
        endif()
    endforeach()
endforeach()

# Relative paths in the listing are relative to the compiler's working
# directory, which is this launcher's.
function(core_isolation_check_file dependency)
    file(REAL_PATH "${dependency}" real_dependency BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    if(real_dependency IN_LIST real_files)
        # An isolated copy must still equal its core file, and a header probe
        # its one include line: the build tree may have been edited since
        # configure checked the core.
        get_filename_component(dependency_name "${real_dependency}" NAME)
        get_filename_component(dependency_directory "${real_dependency}" DIRECTORY)
        get_filename_component(probe_directory "${dependency_directory}" NAME)
        if(probe_directory STREQUAL "header_checks")
            string(REGEX REPLACE "[.]cpp$" "" probed "${dependency_name}")
            file(READ "${real_dependency}" probe_text)
            if(NOT probe_text STREQUAL "#include \"swegca_architecture/${probed}\"\n")
                core_isolation_fail("${real_dependency} was changed after configure")
            endif()
        else()
            file(SHA256 "${real_dependency}" copy_digest)
            file(SHA256 "${CORE_SOURCE_DIR}/${dependency_name}" source_digest)
            if(NOT copy_digest STREQUAL source_digest)
                core_isolation_fail("${real_dependency} differs from ${CORE_SOURCE_DIR}/${dependency_name}; reconfigure")
            endif()
        endif()
        return()
    endif()
    foreach(root IN LISTS real_roots)
        string(FIND "${real_dependency}" "${root}" at)
        if(at EQUAL 0)
            return()
        endif()
    endforeach()
    core_isolation_fail("the compilation of ${object} opens ${dependency}, which is neither an isolated core file nor under the compiler's include directories (${CORE_ALLOWED_ROOTS}). These are what the compiler reports with no project flag but -stdlib= and CMake's toolchain arguments (the compiler command's own, sysroot, target, external toolchain, Apple SDK), plus the toolchain's standard include directories; reconfigure after changing the compiler")
endfunction()

file(READ "${dependency_file}" listing)
if(CORE_COMPILER_KIND STREQUAL "msvc")
    string(JSON source ERROR_VARIABLE json_error GET "${listing}" Data Source)
    string(JSON include_count ERROR_VARIABLE json_error LENGTH "${listing}" Data Includes)
    if(json_error)
        core_isolation_fail("cannot read ${dependency_file}: ${json_error}")
    endif()
    core_isolation_check_file("${source}")
    if(include_count GREATER 0)
        math(EXPR last "${include_count} - 1")
        foreach(index RANGE ${last})
            string(JSON dependency GET "${listing}" Data Includes ${index})
            core_isolation_check_file("${dependency}")
        endforeach()
    endif()
else()
    # Make syntax: targets, then ":" followed by white space (a drive letter
    # colon is followed by a slash), then the files. A line ends in a
    # backslash when it continues; "$$" is one "$"; a backslash escapes a
    # space or "#".
    string(ASCII 92 backslash)
    string(REPLACE "${backslash}\n" " " listing "${listing}")
    string(REPLACE "$$" "$" listing "${listing}")
    string(REGEX MATCH ":[ \t\n]" separator "${listing}")
    string(FIND "${listing}" "${separator}" separator_at)
    if(NOT separator OR separator_at EQUAL -1)
        core_isolation_fail("cannot read ${dependency_file}")
    endif()
    math(EXPR files_at "${separator_at} + 1")
    string(SUBSTRING "${listing}" ${files_at} -1 files)
    # With continuations joined, the first rule is one line. A header listed
    # as its own phony target after it (-MP) repeats a file already listed.
    string(FIND "${files}" "\n" rule_end)
    if(NOT rule_end EQUAL -1)
        string(SUBSTRING "${files}" 0 ${rule_end} files)
    endif()
    separate_arguments(dependency_list UNIX_COMMAND "${files}")
    if(NOT dependency_list)
        core_isolation_fail("${dependency_file} lists no file")
    endif()
    foreach(dependency IN LISTS dependency_list)
        core_isolation_check_file("${dependency}")
    endforeach()
endif()
