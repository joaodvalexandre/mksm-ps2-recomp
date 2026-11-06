# Optimization of the quick profile, by target. scripts/build.ps1 includes this file after the top-level project()
# (CMAKE_PROJECT_INCLUDE) and leaves /O out of CMAKE_<LANG>_FLAGS_RELWITHDEBINFO.
#
# The game's generated code (ps2EntryRunner: thousands of functions in 425 unity batches) is compiled with /O1 /Ob1,
# which keeps a full rebuild short. Everything else, the runtime it links above all (software GS rasterizer, VU1 and
# IOP interpreters), is compiled with /O2 /Ob2: a host profile of the quick build put 80 to 90 per cent of the game
# thread in the rasterizer's per-pixel code, which /Ob1 barely inlined.
include_guard(GLOBAL)

set(_mksm_quick "$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:RelWithDebInfo>>")
set(_mksm_runner "$<STREQUAL:$<TARGET_PROPERTY:NAME>,ps2EntryRunner>")
add_compile_options(
    "$<$<AND:${_mksm_quick},${_mksm_runner}>:/O1;/Ob1>"
    "$<$<AND:${_mksm_quick},$<NOT:${_mksm_runner}>>:/O2;/Ob2>")
