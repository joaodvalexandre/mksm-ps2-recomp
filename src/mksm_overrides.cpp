// Game override module for Mortal Kombat: Shaolin Monks (PAL, SLES-53524), compiled into ps2EntryRunner next to the
// generated code (scripts\build.ps1 copies src\*.cpp into PS2Recomp\ps2xRuntime\src\runner).
//
// The runtime applies it when the booted ELF is named SLES_535.24 and its entry point is 0x0011C038. Everything
// specific to this game (address bindings, replacements, diagnostics) goes here; the runtime itself stays generic.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace
{
    // MKSM_ISO=<path>: the disc image for the sector reads the game does outside the files extracted into game\
    // (the runtime serves sceCdRead from the registered files first, then from this image).
    void configureDiscImage()
    {
        const char *iso = std::getenv("MKSM_ISO");
        if (!iso || !*iso)
            return;
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = std::filesystem::path(iso);
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[mksm] disc image: %s\n", iso);
    }

    void applyMksmOverrides(PS2Runtime &runtime)
    {
        (void)runtime;
        configureDiscImage();
        std::fprintf(stderr, "[mksm] overrides applied: Mortal Kombat: Shaolin Monks (SLES-53524)\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("Mortal Kombat: Shaolin Monks (SLES-53524)", "SLES_535.24", 0x0011C038u, 0u, applyMksmOverrides)
