# Adds the targets of tools/vu1gen to a PS2Recomp build without a patch to PS2Recomp. Configure that build with
#
#   -DCMAKE_PROJECT_PS2Runtime_INCLUDE=<repository>/tools/vu1gen/cmake/ps2recomp.cmake
#
# CMake then includes this file as the last step of project(PS2Runtime ...) in ps2xRuntime/CMakeLists.txt. The call
# deferred here runs at the end of that file, when ps2_runtime exists: upstream's VU1 interpreter, built with the
# flags of the profile, is the oracle of vu1difftest. (A deferred call cannot add a subdirectory, so the targets are
# defined by an included file, in the scope of ps2xRuntime.)
include_guard(GLOBAL)

set(MKSM_VU1GEN_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(mksm_vu1gen_add_targets)
    include("${MKSM_VU1GEN_CMAKE_DIR}/targets.cmake")
endfunction()

cmake_language(DEFER CALL mksm_vu1gen_add_targets)
