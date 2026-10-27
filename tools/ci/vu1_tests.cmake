# MKSM-Port: build a procedural program with the real generator; it includes no game data.
set(MKSM_VU1_TEST_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../tests/vu1_compiled_test.cpp")
add_executable(mksm_vu1_generator "${CMAKE_CURRENT_LIST_DIR}/../vu1/generar_vu1.cpp")
target_link_libraries(mksm_vu1_generator PRIVATE ps2_runtime)
add_executable(mksm_vu1_fixture "${MKSM_VU1_TEST_SOURCE}")
target_link_libraries(mksm_vu1_fixture PRIVATE ps2_runtime)
set(MKSM_VU1_TEST_DIR "${CMAKE_BINARY_DIR}/mksm_vu1_procedural")
set(MKSM_VU1_TEST_PARTS)
foreach(part RANGE 0 12)
    list(APPEND MKSM_VU1_TEST_PARTS "${MKSM_VU1_TEST_DIR}/programa${part}.cpp")
endforeach()
add_custom_command(OUTPUT ${MKSM_VU1_TEST_PARTS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MKSM_VU1_TEST_DIR}"
    COMMAND "$<TARGET_FILE:mksm_vu1_fixture>" --write "${MKSM_VU1_TEST_DIR}/synthetic.bin"
    COMMAND "$<TARGET_FILE:mksm_vu1_generator>" "${MKSM_VU1_TEST_DIR}" "${MKSM_VU1_TEST_DIR}/synthetic.bin"
    DEPENDS mksm_vu1_fixture mksm_vu1_generator
    VERBATIM)
add_executable(mksm_vu1_compiled_test "${MKSM_VU1_TEST_SOURCE}" ${MKSM_VU1_TEST_PARTS})
target_link_libraries(mksm_vu1_compiled_test PRIVATE ps2_runtime)
target_include_directories(mksm_vu1_compiled_test PRIVATE "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/vu")
if(NOT MSVC)
    target_compile_options(mksm_vu1_compiled_test PRIVATE -ffp-contract=off)
endif()

# MKSM-Port: every FMAC (sources, DEST masks, cross) compiled against the interpreter with random
# boundary values. With -march=x86-64-v2 the compiled code uses the 4-lane SSE path.
set(MKSM_VU1_FMAC_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../tests/vu1_fmac_test.cpp")
add_executable(mksm_vu1_fmac_fixture "${MKSM_VU1_FMAC_SOURCE}")
target_link_libraries(mksm_vu1_fmac_fixture PRIVATE ps2_runtime)
set(MKSM_VU1_FMAC_DIR "${CMAKE_BINARY_DIR}/mksm_vu1_fmac")
set(MKSM_VU1_FMAC_PARTS)
foreach(part RANGE 0 12)
    list(APPEND MKSM_VU1_FMAC_PARTS "${MKSM_VU1_FMAC_DIR}/programa${part}.cpp")
endforeach()
add_custom_command(OUTPUT ${MKSM_VU1_FMAC_PARTS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MKSM_VU1_FMAC_DIR}"
    COMMAND "$<TARGET_FILE:mksm_vu1_fmac_fixture>" --write "${MKSM_VU1_FMAC_DIR}/synthetic.bin"
    COMMAND "$<TARGET_FILE:mksm_vu1_generator>" "${MKSM_VU1_FMAC_DIR}" "${MKSM_VU1_FMAC_DIR}/synthetic.bin"
    DEPENDS mksm_vu1_fmac_fixture mksm_vu1_generator
    VERBATIM)
add_executable(mksm_vu1_fmac_test "${MKSM_VU1_FMAC_SOURCE}" ${MKSM_VU1_FMAC_PARTS})
target_link_libraries(mksm_vu1_fmac_test PRIVATE ps2_runtime)
target_include_directories(mksm_vu1_fmac_test PRIVATE "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/vu")
if(NOT MSVC)
    target_compile_options(mksm_vu1_fmac_test PRIVATE -ffp-contract=off)
endif()
