// GOW-Port: services of the isolated test; they are not a runtime API.
#pragma once
bool gsProbeOpenWindow(unsigned width, unsigned height);
void gsProbeCloseWindow();
bool gsProbeHasWindow();
void gsProbeSetSurfaceSize(unsigned width, unsigned height);
void gsProbeBeginUiFrame();
void gsProbeSkipUiFrame();
void gsProbeEndUiFrame();
extern void (*gsProbeBeforePresent)();
extern unsigned gsProbeGpuPresents;
