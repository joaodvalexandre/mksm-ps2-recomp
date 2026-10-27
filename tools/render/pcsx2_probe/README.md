# Isolated GS module for procedural checks

This project builds **original** PCSX2 sources and a limited Host layer.
It neither contains nor requires the game, BIOS, ELF or dumps for these checks.
The DLL can also receive the runtime stream through the optional mirror described in the former
`docs/PCSX2_GS.md` (removed from the tree; see the git history); the usual renderer is kept.
Windows x64, MSVC, Windows SDK, CMake/Ninja and an AVX2 CPU.

Pinned inputs:

- PCSX2 `2954ebe6965669219f1bbd256e958fbd8f83a85d`, clean tree; `common/`,
  `pcsx2/`, `3rdparty/` and `bin/resources/shaders/` available.
- Microsoft DirectX-Headers `64c650b0c6642ab0f3cdf674e38401e02ccd2460`.
- libpng/zlib installed for MSVC. CI uses vcpkg
  `e456309491fd875487bf02b8a0dca76c1b1b00cc`, `libpng:x64-windows-static-md`.
  A dynamic installation needs its DLLs next to the executables.

The DLL fixes the **Maximum** blending precision by default, without modifying the
PCSX2 sources. `-DGOW_PCSX2_BLENDING=Basic` keeps the configuration of the earlier
checks; any other value is rejected at configure time. Use separate builds and
record the hash of each DLL. When the GS is created, `[gow-pcsx2-module] blending=Maximum`
or `Basic` is logged; the ABI is still 1.
Maximum can cost more: the mirror and its readbacks remain diagnostics, and no
port performance is attributed to them.

From a Developer PowerShell, with Python 3 on PATH, choose your own directories
for the sources, excerpts and build. Do not reuse a collaborator's checkout:

```powershell
python tools/render/pcsx2_probe/prepare.py --pcsx2-source C:/work/pcsx2 --output C:/work/gs-excerpts
cmake -S tools/render/pcsx2_probe -B C:/work/gs-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPCSX2_SOURCE=C:/work/pcsx2 -DPROBE_GENERATED=C:/work/gs-excerpts -DDIRECTX_HEADERS=C:/work/DirectX-Headers -DCMAKE_TOOLCHAIN_FILE=C:/work/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build C:/work/gs-build --parallel 3
ctest --test-dir C:/work/gs-build --output-on-failure
```

The preparation checks the revision/tree, keeps the GPL-3.0+ notices and generates
only excerpts of configuration, utilities, CPU detection and dump registers.
It rejects foreign non-empty destinations. It does not change the downloaded sources. The release
of repeated memory adds two local fixes marked GOW-Port: closing the
handle and joining the placeholders before freeing the whole range. The test
verifies aliases, handles and MEM_FREE after eight cycles; it does not cover reservation failures.

CTest runs the parser and memory checks **without a device**. The GPU checks are also built;
run them explicitly, one after the other, on a free PC:

If you have PS2Recomp headers with the port's chain applied,
`-DGOW_RUNTIME_HEADERS=C:/work/PS2Recomp/ps2xRuntime/include` adds the
client/queue/DLL contract for VSync and CSR, without a GPU. Windows CI enables this sixth check.

Optional GPU checks:

```powershell
& C:/work/gs-build/gs_dx11_probe.exe
& C:/work/gs-build/gs_gif_draw_probe.exe
& C:/work/gs-build/gs_gif_draw_probe.exe --frame
& C:/work/gs-build/gs_gif_draw_probe.exe --window
& C:/work/gs-build/gs_module_gpu_probe.exe C:/work/gs-build/gow_pcsx2_gs_module.dll
python tools/render/probe_fog_reference.py --module C:/work/gs-build/gow_pcsx2_gs_module.dll --resources C:/work/pcsx2/bin/resources
python tools/render/probe_pipeline_reference.py --module C:/work/gs-build/gow_pcsx2_gs_module.dll --resources C:/work/pcsx2/bin/resources
```

The FOG check through the ABI compares 28 RGB halves, with FGE off and
FOG 0/1/77/128/201/255, plus FOG `(F+17)%255` on the second sprite. It checks
the color and uniformity of the 32×32 pixels of each half. `--left-only` reproduces
the 14 historical checks; those only checked the left half.
Two sprites with different fogs are sent in the same batch: a uniform PCSX2
FOG=255 clear can skip the shader, so that clear is no use to
decide its rounding. The data is synthetic, with no game files. PCRTC alpha
is 255; the runtime regression checks framebuffer alpha 128 separately.
It does not validate variable interpolation, a scene or performance.

On the original DX11 hardware 16/28 was observed with Maximum: the right half
of the FGE cases takes the FOG of the first sprite. The original software matches
in 28/28 and the repetition of a synthetic Freeze/GIF/VSync burst matches
its fresh run in every pixel. The two-sprite technique keeps
the validity of the 14 expected left ones, but does not prevent every hardware
optimization of the batch. Keep the negative result and compare other paths before
attributing a game discrepancy to the runtime.

The second check sends 24 cells of 8×8: eight ZTE/ZTST combinations,
eight Z-write probes and eight cases of FOG followed by blending with alpha 64.
It checks RGB and the uniformity of every pixel of each cell. The expected values
were compared against CPU, compute, hardware and DX11 Maximum. The historical Basic DLL
matches in 20/24 and differs by one level in four blending cases,
two of them without FOG; Maximum matches in 24/24. Do not turn differences from
a Basic reference into a runtime fix. PCRTC alpha is 255 and this
check does not verify framebuffer alpha, variable interpolation nor the game.

DX11 uses original SM4/5 shaders and Surfaceless mode. DXC is linked with delayed
loading; those checks need neither dxcompiler.dll nor dxil.dll. This does not
implement DX12 or Vulkan. The device selects hardware: WARP is not introduced
to attribute its result to a GPU.

The Host layer offers default configuration, shader resources, messages
to stderr, procedural registers and real SW code memory when requested.
`--frame` adds three distinct patterns to the original GSvsync and compares each composed
PCRTC output. The host counts the callbacks and the omission of Surfaceless's
own presentation; those counters are not FPS. `--window` creates its own
**hidden** window, checks the DXGI backbuffer before Present and repeats two
device/renderer/window cycles, with three patterns per cycle. It uses an empty
ImGui context for the original EndPresent, with no interface ini/log files.
It does not check the visible image, resizing or device loss.
Reopening, capture, dumps and texture replacements are not implemented;
their operations abort with a diagnostic. The original SW objects
are kept; the GPU checks do not validate that path. The results
and pending integration items were in the former `docs/PCSX2_GS.md` (see the git history).

## Experimental DLL ABI

`module_api.h` defines ABI 1 with fixed-size structures (Windows x64),
numeric handles and error states. It creates a single GS per DLL, receives GIF bytes
borrowed during the call and a copy of the **8 KiB of privileged PS2 registers**,
and allows querying/copying the composed RGBA PCRTC texture.
That layout is not the GSRegisters struct of PS2Recomp: the adapter must build
the register mirror. VSync does not require finishing pending GIF fragments.
The sequence counts VSync, without certifying new content or a visible image.

Creating, sending, VSync, reading and closing require the same thread. A failure while processing
input is retained until close; argument rejections do not consume data.
A query without a buffer returns the required dimensions/bytes; a smaller buffer
receives no pixels. Reading the output performs a readback and has an explicit cost.

`gs_module_contract_probe` loads the DLL and checks exports/ABI/rejections without a
device; it is added to CTest/CI. The optional GPU check uses only GetProcAddress,
without linking the renderer's C++ globals: repeated creation, busy singleton,
closed handle, another thread, fragmented GIF, register size, capacity and
exactly 2,048 RGBA pixels per cycle. The DLL is still **rendering only**:
it does not replace the runtime's FIFO/VRAM, IRQ/FINISH nor does it support loading in the middle of a
game, device loss or every reservation failure path.

`gow_gs_read_stream_info` is another optional ABI1 extension. It returns a
64-byte `GowGsStreamInfo` under the same thread/handle contract, without consuming
input, composing, flushing queues or downloading pixels. An old DLL may not
export it. The identity of the stream is **handle + epoch**; reset advances the epoch and
zeroes the cursors even if the VSync sequence repeats.

`input_events` counts every accepted submit, direct write and VSync, including
an empty submit; `input_gif_bytes` counts only its GIF bytes. The
`last_vsync_*` fields keep the values at the last successful VSync call,
including that event, and its field/sequence. Rejections and queries do not advance them.
This way, later GIF data or registers are not attributed to the previous VSync.

The `INPUT_QWORD_ALIGNED` and `VSYNC_QWORD_ALIGNED` flags only indicate the absence
of 16-byte transport fragments in the current input or at that boundary.
**A GIF tag can still be waiting for payload even if the transport is aligned.**
`HAS_VSYNC` indicates a successful VSync in the epoch; `OUTPUT_AVAILABLE` only
observes GetCurrent and can correspond to a texture kept after a reset.
These cursors are not hashes, do not prove new composition/new RGB nor
alignment with the host's OpenGL presentation. To compare, the same stream and
prefix, the effective CRTC and options, plus the boundary of the composed image
must be established. Game captures and bursts are kept private.

The `probe_snapshot_reference.py --stream-info` probe keeps that metadata in
`result.json` at four points: before sending, after the GIF, after the
VSync and after the 16 snapshots. It requires the new export only if requested;
without the option it stays compatible with old DLLs. A query error
fails explicitly and closes the handle, instead of publishing zeros as data.
The probe requires that snapshots do not advance the cursor. The pattern is synthetic;
the local cursors and their limits are saved next to the result, without declaring
parity with the host or fidelity of a scene.
