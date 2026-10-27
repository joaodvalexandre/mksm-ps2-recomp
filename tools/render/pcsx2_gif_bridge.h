// GOW-Port: own transport of GIF bytes for the PCSX2 Transfer interface.
// It does not decode GIF nor replace the renderer. The consumer runs before
// returning: it cannot keep the pointer to the aligned copy.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace gow_pcsx2_gs
{
    enum class Path : unsigned { Path1=1, Path2=2, Path3=3 };
    struct Sink {
        void *opaque=nullptr;
        // Amount in 16-byte quadwords, like GSState::Transfer / GSgifTransfer2/3.
        bool (*transfer)(void *,Path,const uint8_t *,uint32_t)=nullptr;
    };

    class GifBridge {
        struct Fragment { std::array<uint8_t,16> bytes{}; size_t size=0; };
        std::array<Fragment,3> fragments{};
        Sink sink;
        bool failed=false;
        static constexpr size_t capacity=4096;
        static bool validPath(Path path) { const auto n=unsigned(path); return n>=1 && n<=3; }
        bool emit(Path path,const uint8_t *data,size_t bytes) {
            alignas(16) std::array<uint8_t,capacity> aligned;
            std::memcpy(aligned.data(),data,bytes);
            if(!sink.transfer(sink.opaque,path,aligned.data(),uint32_t(bytes/16))) {
                // The destination may have consumed a prefix. Do not retry nor
                // continue as if renderer and sender kept the same state.
                failed=true; return false;
            }
            return true;
        }
    public:
        explicit GifBridge(Sink target):sink(target) {}
        GifBridge(const GifBridge&)=delete;
        GifBridge &operator=(const GifBridge&)=delete;
        bool submit(Path path,const uint8_t *data,size_t size) {
            if(!validPath(path) || (!data && size) || !sink.transfer || failed) return false;
            auto &pending=fragments[unsigned(path)-1];
            if(pending.size) {
                const auto take=std::min(16-pending.size,size);
                if(take) std::memcpy(pending.bytes.data()+pending.size,data,take);
                pending.size+=take;
                if(take) data+=take;
                size-=take;
                if(pending.size<16) return true;
                pending.size=0;
                if(!emit(path,pending.bytes.data(),16)) return false;
            }
            while(size>=16) {
                const auto bytes=std::min(size-size%16,capacity);
                if(!emit(path,data,bytes)) return false;
                data+=bytes; size-=bytes;
            }
            if(size) {
                std::memcpy(pending.bytes.data(),data,size);
                pending.size=size;
            }
            return true;
        }
        size_t pendingBytes(Path path) const {
            return validPath(path)?fragments[unsigned(path)-1].size:0;
        }
        // Only check when closing the full stream; VSync does not end a GIF packet.
        bool complete() const {
            if(failed || !sink.transfer) return false;
            for(const auto &p:fragments) if(p.size) return false;
            return true;
        }
        // The owner must also reset the real GS before discarding fragments.
        void discardAfterGsReset() { fragments={}; failed=false; }
    };
}
