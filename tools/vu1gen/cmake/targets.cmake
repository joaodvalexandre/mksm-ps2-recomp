# The targets of tools/vu1gen. Included by ../CMakeLists.txt (the folder as a project of its own) and by
# ps2recomp.cmake (inside a PS2Recomp build), so every path is absolute and the outputs go to <build>/vu1gen.
get_filename_component(VU1GEN_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VU1GEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/vu1gen")

function(vu1gen_target_defaults target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ARCHIVE_OUTPUT_DIRECTORY "${VU1GEN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${VU1GEN_OUTPUT_DIR}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        # Standard C file functions without the CRT's deprecation warnings.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# The generator's parts, as a library: the unit tests and vu1difftest use them too.
add_library(vu1gen_core STATIC
    "${VU1GEN_DIR}/src/images.cpp"
    "${VU1GEN_DIR}/src/listing.cpp"
    "${VU1GEN_DIR}/src/vu_decode.cpp"
)
target_include_directories(vu1gen_core PUBLIC "${VU1GEN_DIR}/src")
vu1gen_target_defaults(vu1gen_core)

add_executable(vu1gen
    "${VU1GEN_DIR}/src/main.cpp"
)
target_link_libraries(vu1gen PRIVATE vu1gen_core)
vu1gen_target_defaults(vu1gen)

# The part of the translated programs' runtime that is not generated (the XGKICK packet walk, the launch wrapper with
# the MXCSR guard). It refers to vu1nPrograms(), which the generated table defines.
add_library(vu1n_runtime STATIC
    "${VU1GEN_DIR}/runtime/vu1n_runtime.cpp"
)
target_include_directories(vu1n_runtime PUBLIC "${VU1GEN_DIR}/runtime")
vu1gen_target_defaults(vu1n_runtime)
if(MSVC)
    # Never /fp:fast next to the translated arithmetic, whatever the profile of the build says.
    target_compile_options(vu1n_runtime PRIVATE /fp:precise)
endif()

get_filename_component(VU1GEN_REPO_DIR "${VU1GEN_DIR}/../.." ABSOLUTE)
add_executable(vu1n_tests
    "${VU1GEN_DIR}/tests/test_main.cpp"
    "${VU1GEN_DIR}/tests/decode_tests.cpp"
    "${VU1GEN_DIR}/tests/images_tests.cpp"
    "${VU1GEN_DIR}/tests/runtime_tests.cpp"
)
target_compile_definitions(vu1n_tests PRIVATE "VU1GEN_IMAGES_FILE=\"${VU1GEN_REPO_DIR}/config/vu1_images.txt\"")
target_link_libraries(vu1n_tests PRIVATE vu1gen_core vu1n_runtime)
vu1gen_target_defaults(vu1n_tests)

# Inside a PS2Recomp build: the differential test. ps2_runtime holds upstream's VU1 interpreter, compiled with the
# flags of the build tree's profile; it is the oracle.
if(TARGET ps2_runtime)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        # ps2xRuntime/cmake/ReleaseMode.cmake compiles the runtime /fp:fast /arch:AVX2 in this configuration: the
        # compiler may then fuse and reorder the interpreter's arithmetic.
        message(WARNING "vu1difftest: in the Release configuration upstream's VU1 interpreter is compiled /fp:fast. "
                        "It is the reference of the translation only as the quick profile compiles it (RelWithDebInfo, /fp:precise).")
    endif()
    # The translated programs. vu1gen does not emit them yet, so an empty table stands in; the emission stage
    # replaces this source by the generated ones (outside the repository, compiled /fp:precise).
    add_library(vu1n_programs STATIC
        "${VU1GEN_DIR}/difftest/no_programs.cpp"
    )
    target_link_libraries(vu1n_programs PUBLIC vu1n_runtime)
    vu1gen_target_defaults(vu1n_programs)

    add_executable(vu1difftest
        "${VU1GEN_DIR}/difftest/captures.cpp"
        "${VU1GEN_DIR}/difftest/checks.cpp"
        "${VU1GEN_DIR}/difftest/launch.cpp"
        "${VU1GEN_DIR}/difftest/oracle.cpp"
        "${VU1GEN_DIR}/difftest/vu1difftest.cpp"
    )
    target_link_libraries(vu1difftest PRIVATE vu1gen_core vu1n_programs vu1n_runtime ps2_runtime)
    vu1gen_target_defaults(vu1difftest)
    if(MSVC)
        # As upstream's ps2x_tests: the interpreter's objects are large.
        target_link_options(vu1difftest PRIVATE "/STACK:8388608")
    endif()
    if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        ps2x_stage_ffmpeg_runtime_dlls(vu1difftest)
    endif()
endif()
