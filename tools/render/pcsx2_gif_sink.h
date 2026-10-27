// GOW-Port: enlace opcional con GSState de PCSX2 2954ebe6965669219f1bbd256e958fbd8f83a85d.
// Build inside the isolated GS module, with its includes and libraries. The
// real GS must be open; this file creates no device or window.
#pragma once
#include "pcsx2_gif_bridge.h"
#include "GS/Renderers/Common/GSRenderer.h"

namespace gow_pcsx2_gs
{
    inline Sink pcsx2TransferSink() {
        return {nullptr,[](void *,Path path,const uint8_t *bytes,uint32_t qwc)->bool {
            if(!g_gs_renderer) return false;
            // PATH1 uses the continuous entry of GSgifTransfer (index 3). The
            // legacy index 0 cuts at EOP and discards nloop if the span ends:
            // it would break both fragments and several packets in the same batch.
            // Indices 1/2 keep their own cursors for PATH2/3.
            switch(path) {
            case Path::Path1: g_gs_renderer->Transfer<3>(bytes,qwc); break;
            case Path::Path2: g_gs_renderer->Transfer<1>(bytes,qwc); break;
            case Path::Path3: g_gs_renderer->Transfer<2>(bytes,qwc); break;
            default: return false;
            }
            return true;
        }};
    }
}
