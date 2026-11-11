# The ahead-of-time translator of the game's VU1 programs.
#
#   vu1gen        the generator (C++20, standard library only)
#   vu1n_tests    its unit tests, and those of the runtime part (runtime/)
#   vu1difftest   upstream's VU1 interpreter against the translation on captured launches; only inside a PS2Recomp
#                 build (cmake/ps2recomp.cmake), where ps2_runtime exists
#
# This file makes the folder a project of its own with the targets that need nothing but a C++ compiler; the
# targets themselves are in cmake/targets.cmake. PROGRESS.md has the commands.
cmake_minimum_required(VERSION 3.21)

project(mksm_vu1gen LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/targets.cmake")
