# GS checks using the already patched runtime, with no game data or OpenGL.
# Include from the root CMakeLists.txt of PS2Recomp after defining ps2_runtime.
# MKSM-Port: the GS replay tools used the CPU rasterizer (gs_cpu_backend.h), which Waffle565's runtime
# replaces with the native renderer; they were retired with the migration (see docs/ARCHITECTURE.md).
add_executable(mksm_gs_finish_async_test "${CMAKE_CURRENT_LIST_DIR}/../../tests/gs_finish_async_test.cpp")
target_link_libraries(mksm_gs_finish_async_test PRIVATE ps2_runtime)
