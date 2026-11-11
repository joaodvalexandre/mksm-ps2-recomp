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
    "${VU1GEN_DIR}/src/vu_emit.cpp"
    "${VU1GEN_DIR}/src/vu_flow.cpp"
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
    "${VU1GEN_DIR}/tests/math_tests.cpp"
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
    # The translated programs. With VU1GEN_ELF (the game's SLES_535.24) vu1gen translates the images of
    # config/vu1_images.txt into <build>/generated-vu1 (never into the repository: the sources quote the game's
    # microcode) and they are compiled here, /fp:precise: vu1n_programs with the fast arithmetic of runtime/vu1n.h,
    # vu1n_programs_ref with the reference one (VU1N_MATH_REFERENCE), linked into vu1difftest_ref. Without it an empty
    # table stands in and vu1difftest runs the oracle side only.
    set(VU1GEN_ELF "" CACHE FILEPATH "The game ELF (SLES_535.24) to translate the VU1 images of; empty: no translation")
    if(VU1GEN_ELF)
        set(VU1GEN_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated-vu1")
        set(VU1GEN_IMAGES_FILE "${VU1GEN_REPO_DIR}/config/vu1_images.txt")
        file(STRINGS "${VU1GEN_IMAGES_FILE}" VU1GEN_IMAGE_LINES REGEX "^image ")
        set(VU1GEN_GENERATED_SOURCES "${VU1GEN_GENERATED_DIR}/vu1n_table.cpp")
        foreach(image_line IN LISTS VU1GEN_IMAGE_LINES)
            string(REGEX REPLACE "^image ([a-z][a-z0-9_]*).*" "\\1" image_name "${image_line}")
            list(APPEND VU1GEN_GENERATED_SOURCES "${VU1GEN_GENERATED_DIR}/vu1n_${image_name}.cpp")
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VU1GEN_IMAGES_FILE}")
        add_custom_command(
            OUTPUT ${VU1GEN_GENERATED_SOURCES}
            COMMAND vu1gen --elf "${VU1GEN_ELF}" --images "${VU1GEN_IMAGES_FILE}" --out "${VU1GEN_GENERATED_DIR}"
                    --report "${VU1GEN_GENERATED_DIR}/report.txt"
            DEPENDS vu1gen "${VU1GEN_ELF}" "${VU1GEN_IMAGES_FILE}"
            COMMENT "vu1gen: translating the VU1 images"
            VERBATIM)
        foreach(programs_target vu1n_programs vu1n_programs_ref)
            add_library(${programs_target} STATIC ${VU1GEN_GENERATED_SOURCES})
            target_include_directories(${programs_target} PRIVATE $<TARGET_PROPERTY:ps2_runtime,INTERFACE_INCLUDE_DIRECTORIES>)
            if(MSVC)
                target_compile_options(${programs_target} PRIVATE /fp:precise /bigobj)
            endif()
            target_link_libraries(${programs_target} PUBLIC vu1n_runtime)
            vu1gen_target_defaults(${programs_target})
        endforeach()
        target_compile_definitions(vu1n_programs_ref PRIVATE VU1N_MATH_REFERENCE)
    else()
        add_library(vu1n_programs STATIC
            "${VU1GEN_DIR}/difftest/no_programs.cpp"
        )
        target_link_libraries(vu1n_programs PUBLIC vu1n_runtime)
        vu1gen_target_defaults(vu1n_programs)
    endif()

    set(VU1GEN_DIFFTESTS vu1difftest)
    if(TARGET vu1n_programs_ref)
        list(APPEND VU1GEN_DIFFTESTS vu1difftest_ref)
    endif()
    foreach(difftest IN LISTS VU1GEN_DIFFTESTS)
        add_executable(${difftest}
            "${VU1GEN_DIR}/difftest/captures.cpp"
            "${VU1GEN_DIR}/difftest/checks.cpp"
            "${VU1GEN_DIR}/difftest/launch.cpp"
            "${VU1GEN_DIR}/difftest/oracle.cpp"
            "${VU1GEN_DIR}/difftest/vu1difftest.cpp"
        )
        if(difftest STREQUAL "vu1difftest_ref")
            target_link_libraries(${difftest} PRIVATE vu1gen_core vu1n_programs_ref vu1n_runtime ps2_runtime)
        else()
            target_link_libraries(${difftest} PRIVATE vu1gen_core vu1n_programs vu1n_runtime ps2_runtime)
        endif()
        vu1gen_target_defaults(${difftest})
        if(MSVC)
            # As upstream's ps2x_tests: the interpreter's objects are large.
            target_link_options(${difftest} PRIVATE "/STACK:8388608")
        endif()
        if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
            ps2x_stage_ffmpeg_runtime_dlls(${difftest})
        endif()
    endforeach()
endif()
