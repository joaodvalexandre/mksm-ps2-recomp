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
