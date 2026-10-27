// GOW-Port: ABI experimental de renderizado; el runtime conserva IRQ/FINISH.
#pragma once
#include <stdint.h>
#ifdef GOW_PCSX2_MODULE_BUILD
#define GOW_GS_API __declspec(dllexport)
#else
#define GOW_GS_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { GOW_GS_ABI = 1, GOW_GS_PRIV_BYTES = 8192 };
enum GowGsStatus {
    GOW_GS_OK=0, GOW_GS_BAD_ARGUMENT=1, GOW_GS_BAD_HANDLE=2,
    GOW_GS_WRONG_THREAD=3, GOW_GS_BUSY=4, GOW_GS_FAILED=5,
    GOW_GS_NO_FRAME=6, GOW_GS_BUFFER_SMALL=7
};
typedef struct GowGsCreateInfo {
    uint32_t size, abi;
    const char* resources; // UTF-8; nullptr uses the configured development path.
    uint32_t width, height;
    uint32_t hidden_window; // 0 Surfaceless, 1 ventana propia oculta.
    uint32_t reserved;
} GowGsCreateInfo;
typedef struct GowGsFrameInfo {
    uint32_t size, width, height, row_bytes;
    uint64_t sequence, required_bytes;
} GowGsFrameInfo;
// GOW-Port: optional extension; it does not change the structs or the earlier ABI1 calls.
typedef struct GowGsVideoInfo {
    uint32_t size, video_mode, really_interlaced, interlace_mode;
    uint32_t pcrtc_anti_blur, reserved;
    uint64_t sequence, smode1, syncv, smode2;
} GowGsVideoInfo;
// GOW-Port: prefixes of calls accepted in this instance/epoch, not VRAM positions.
// the transport's complete() only checks 16-byte fragments, not the end of the GIF tag.
enum GowGsStreamFlags {
    GOW_GS_STREAM_HAS_VSYNC=1, GOW_GS_STREAM_OUTPUT_AVAILABLE=2,
    GOW_GS_STREAM_INPUT_QWORD_ALIGNED=4, GOW_GS_STREAM_VSYNC_QWORD_ALIGNED=8
};
typedef struct GowGsStreamInfo {
    uint32_t size, flags;
    uint64_t epoch, input_events, input_gif_bytes;
    uint64_t last_vsync_events, last_vsync_gif_bytes, last_vsync_sequence;
    uint32_t last_vsync_field, reserved;
} GowGsStreamInfo;

enum GowGsSnapshotAspect {
    GOW_GS_ASPECT_STRETCH=0, GOW_GS_ASPECT_AUTO=1,
    GOW_GS_ASPECT_4_3=2, GOW_GS_ASPECT_16_9=3
};
GOW_GS_API uint32_t gow_gs_abi(void);
GOW_GS_API uint32_t gow_gs_create(const GowGsCreateInfo* info, uint64_t* handle);
GOW_GS_API uint32_t gow_gs_destroy(uint64_t handle);
GOW_GS_API uint32_t gow_gs_submit(uint64_t handle, uint32_t path, const uint8_t* bytes, uint32_t size);
GOW_GS_API uint32_t gow_gs_write_register(uint64_t handle, uint32_t reg, uint64_t value);
GOW_GS_API uint32_t gow_gs_reset(uint64_t handle);
// GOW-Port: field uses the PCSX2 GSvsync convention (CSR.FIELD ? 0 : 1).
GOW_GS_API uint32_t gow_gs_vsync(uint64_t handle, uint32_t field, const uint8_t* privileged, uint32_t size);
GOW_GS_API uint32_t gow_gs_read_output(uint64_t handle, uint8_t* rgba, uint64_t capacity, GowGsFrameInfo* info);
// The mode and options come from the original GS; video_mode uses GSVideoMode of the pinned source.
GOW_GS_API uint32_t gow_gs_read_video_info(uint64_t handle, GowGsVideoInfo* info);
// No consume entrada ni fuerza render/readback. La identidad incluye handle + epoch.
// input_events counts submit (even empty), correct write_register and VSync; reset opens an epoch.
// last_vsync_* describes the boundary of the VSync call, without crediting new RGB/new composition.
// OUTPUT_AVAILABLE only observes GetCurrent; it can keep a texture after a reset or later GIF data.
// Optional export: older DLLs may not offer it. The earlier structs and calls do not change.
GOW_GS_API uint32_t gow_gs_read_stream_info(uint64_t handle, GowGsStreamInfo* info);
// nullptr/0 computes and stores the original snapshot; a later read uses that capture without recomputing.
// Window 0/0 keeps the internal resolution; maximum 2048 per dimension. Original normalizes alpha and rescales.
// The cache is invalidated on VSync/reset/close. Older DLLs may not export these two functions.
GOW_GS_API uint32_t gow_gs_read_snapshot(uint64_t handle, uint32_t aspect, uint32_t window_width,
    uint32_t window_height, uint8_t* rgba, uint64_t capacity, GowGsFrameInfo* info);
#ifdef __cplusplus
}
static_assert(sizeof(GowGsCreateInfo)==32 && sizeof(GowGsFrameInfo)==32, "ABI Windows x64");
static_assert(sizeof(GowGsVideoInfo)==56, "Optional Windows x64 ABI extension");
static_assert(sizeof(GowGsStreamInfo)==64, "Frontera optativa de ABI Windows x64");
#endif
