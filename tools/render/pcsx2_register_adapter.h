// GOW-Port: stub writes without inventing a PATH nor losing GIF cursors.
// Legacy index 0 is reserved for these complete A+D packets; the continuous
// bridge uses 3/1/2. Keep Q: a direct write is not a game tag.
#pragma once
#include <array>
#include <cstdint>
namespace gow_pcsx2_gs {
template<class Base> class RegisterAdapter : public Base {
public:
    void writeDirect(uint8_t reg,uint64_t value) {
        alignas(16) const std::array<uint64_t,4> packet{
            1|(uint64_t(1)<<15)|(uint64_t(1)<<60),0xe,value,reg};
        const float q=this->m_q;
        this->template Transfer<0>(reinterpret_cast<const uint8_t*>(packet.data()),2);
        this->m_q=q;
    }
};
}
