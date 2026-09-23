# Clean environment for the core's compiler (CMakeLists.txt, "Core
# isolation"), included by the scripts that run it: the include-root probe,
# the compiler launcher and the isolation check's link. Environment
# variables change what a GNU-style compiler does without a flag on the
# command line: CPATH and CPLUS_INCLUDE_PATH add include directories,
# COMPILER_PATH and GCC_EXEC_PREFIX move where it finds its own parts,
# CCC_OVERRIDE_OPTIONS rewrites Clang's arguments (e.g. -MD into -MMD, which
# leaves system headers out of the dependency listing), LD_LIBRARY_PATH and
# LD_PRELOAD choose the shared libraries the compiler, cc1plus and nm load,
# SDKROOT and DEVELOPER_DIR choose Apple's SDK and toolchain,
# MACOSX_DEPLOYMENT_TARGET its target version, HOME (on some Clang builds) a
# user configuration file, and TMPDIR where GCC leaves the assembly and
# link files it hands from one of its parts to the next. So only the
# variables named here reach it, PATH and the temporary directory with the
# values set here; every other one is removed from this script's process
# before it runs the compiler.
#
# For a GNU-style compiler (MSVC is not run through this file: it gets the
# build's PATH, INCLUDE and LIB):
#
# Trusted: the compiler command CMake was given (CXX, including a wrapper
# named as the compiler and CMAKE_CXX_COMPILER_ARG1); CMake's own toolchain
# settings (sysroot, target, external toolchain, Apple sysroot and
# deployment target); the PATH present when the first configure discovered
# the toolchain, which CMakeLists.txt caches (CORE_PATH_FILE) and which
# replaces PATH here, so where the compiler searches PATH for its assembler
# and linker it sees what configure saw; the build tree, which holds
# that file and the commands the build runs, and where TMPDIR, TMP and TEMP
# are pointed here (swegca_core_tmp beside CORE_PATH_FILE); and the dynamic
# loader's variables (LD_PRELOAD, LD_LIBRARY_PATH, LD_AUDIT, DYLD_*, and on
# Windows PATH, where the loader looks for DLLs) of the process that starts
# the build. Those loader variables are read when each program is loaded:
# make, ninja and `cmake --build` are already loaded under them, and so is
# every `cmake -P` or `env -i` the build could start before this script
# runs. No check inside the build can run ahead of them, so whoever starts
# the build must trust them; they are still removed or replaced here, so
# the compiler, cc1plus and nm are not loaded under them.
# Passed through from the build's environment, as none changes what is
# compiled or what the compiler opens: LANG, LANGUAGE and LC_* (the
# language of its messages; the include probe sets the C locale),
# SOURCE_DATE_EPOCH (the value of __DATE__ and __TIME__), TZ, and on Windows
# SYSTEMROOT, WINDIR, COMSPEC and PATHEXT, without which programs there do
# not start.
# Not trusted: every other variable of the build's environment, PATH and
# TMPDIR included; each changes what the compiler does only once the
# compiler reads it (CPATH, COMPILER_PATH, PATH, ...) and is removed or
# replaced before it runs. A compiler that needs its own library path or SDK
# must get it from the trusted settings, e.g. a wrapper script named as
# CXX, not from the environment.

# Names compared in upper case: Windows names are case-insensitive (`Path`).
set(swegca_core_kept_variables
    "^(PATH|TMPDIR|TMP|TEMP|LANG|LANGUAGE|LC_[A-Z_]+|TZ|SOURCE_DATE_EPOCH|SYSTEMROOT|WINDIR|COMSPEC|PATHEXT)$")

# The names in the environment, one per line of `cmake -E environment`.
# Before the text becomes a CMake list, the characters a list treats
# specially (`[` and `]` group, `\` escapes `;`) are replaced, so a value
# holding one cannot hide the variables after it; a value's line break or
# `;` only adds pieces whose "names" are removed too, which is safe. A name
# holding one of those characters is changed by the replacement and so is
# not removed; no compiler reads such a name.
function(swegca_core_environment_names out)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
        OUTPUT_VARIABLE environment RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "core isolation check: cannot list the environment")
    endif()
    string(ASCII 92 backslash)
    string(REPLACE "${backslash}" "_" environment "${environment}")
    string(REPLACE "[" "_" environment "${environment}")
    string(REPLACE "]" "_" environment "${environment}")
    string(REPLACE ";" "\n" environment "${environment}")
    string(REPLACE "\n" ";" lines "${environment}")
    set(names)
    foreach(line IN LISTS lines)
        if(line MATCHES "^([^=]+)=")
            list(APPEND names "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# The optional argument names a function to call with the reason when the
# environment cannot be cleaned (e.g. one that removes the output first);
# without it the script stops with that reason.
function(swegca_core_clean_environment)
    swegca_core_environment_names(names)
    foreach(name IN LISTS names)
        string(TOUPPER "${name}" upper)
        if(NOT upper MATCHES "${swegca_core_kept_variables}")
            unset(ENV{${name}})
        endif()
    endforeach()
    # Listed again: a name the removal missed stops the compilation. A piece
    # of a value that only looks like a name (`PATH=a;X=b`) is not defined
    # and is skipped.
    swegca_core_environment_names(names)
    foreach(name IN LISTS names)
        string(TOUPPER "${name}" upper)
        if(NOT upper MATCHES "${swegca_core_kept_variables}" AND DEFINED ENV{${name}})
            set(reason "the environment variable ${name} could not be removed")
            if(ARGC GREATER 0)
                cmake_language(CALL "${ARGV0}" "${reason}")
            endif()
            message(FATAL_ERROR "core isolation check: ${reason}")
        endif()
    endforeach()
    # PATH: configure's, recorded by CMakeLists.txt, not the build's. (An
    # empty value would unset PATH, leaving the system's default search.)
    set(configured_path)
    if(DEFINED CORE_PATH_FILE AND EXISTS "${CORE_PATH_FILE}")
        file(READ "${CORE_PATH_FILE}" configured_path)
    endif()
    if("${configured_path}" STREQUAL "")
        set(reason "the PATH recorded at configure (CORE_PATH_FILE) is missing or empty; configure a fresh build tree with PATH set")
        if(ARGC GREATER 0)
            cmake_language(CALL "${ARGV0}" "${reason}")
        endif()
        message(FATAL_ERROR "core isolation check: ${reason}")
    endif()
    set(ENV{PATH} "${configured_path}")
    # Temporary files: in the build tree, not where the build's TMPDIR says.
    # GCC names each file uniquely, so parallel compilations can share it.
    get_filename_component(build_dir "${CORE_PATH_FILE}" DIRECTORY)
    set(temporary_dir "${build_dir}/swegca_core_tmp")
    file(MAKE_DIRECTORY "${temporary_dir}")
    set(ENV{TMPDIR} "${temporary_dir}")
    set(ENV{TMP} "${temporary_dir}")
    set(ENV{TEMP} "${temporary_dir}")
endfunction()
