#pragma once

// [DoD] IOP SIO2 (controller / memory card serial interface, 0x1F808200-0x1F808284) with a DualShock 2 on port 1.
// The register model and the pad protocol follow PCSX2 (pcsx2/SIO/Sio2.cpp, SIO/Pad/PadDualshock2.cpp, GPL-3.0+).
// Memory cards and multitaps report "not connected" for now.

#include <cstddef>
#include <cstdint>
#include <deque>

namespace ps2x::iop
{
    // Host pad state in libpad layout: data[2..3] buttons (active low: select,L3,R3,start,up,right,down,left /
    // L2,R2,L1,R1,triangle,circle,cross,square), data[4..7] rx, ry, lx, ly. Returns false when no pad is present.
    extern bool (*g_dodPadStateProvider)(int port, uint8_t *data32);
}

namespace ps2x::iop::detail
{
    class IopSio2
    {
    public:
        void reset();
        bool handles(uint32_t phys) const { return phys >= 0x1F808200u && phys < 0x1F808288u; }
        uint32_t read32(uint32_t phys);
        void write32(uint32_t phys, uint32_t value);

        // IOP DMA 11 (SIO2in) / 12 (SIO2out)
        void dmaIn(const uint8_t *data, size_t bytes, uint32_t blockBytes);
        void dmaOut(uint8_t *data, size_t bytes);

        bool takeInterrupt();

    private:
        struct Pad
        {
            uint8_t mode = 0x41; // digital
            bool inConfig = false;
            bool analogLight = false;
            bool analogLocked = false;
            bool commandStage = false;
            uint32_t responseBytes = 0;
            uint8_t motorMap[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            uint8_t largeMotor = 0, smallMotor = 0;
            uint8_t command = 0;
            uint32_t received = 0;
            uint8_t state[32] = {};
        };

        void softReset();
        void pushIn(uint8_t value);
        void runCommand();
        uint8_t padByte(Pad &pad, uint8_t commandByte);

        uint32_t m_cmdQueue[16] = {};
        uint32_t m_portCtrl[8] = {};
        uint32_t m_ctrl = 0x3BC;
        uint32_t m_cmdStat = 0x1D100;
        uint32_t m_portStat = 0xF;
        uint32_t m_fifoStat = 0;
        uint32_t m_iStat = 0;
        bool m_irqPending = false;

        bool m_queueRead = false;
        size_t m_queuePosition = 0;
        uint32_t m_commandLength = 0;
        uint32_t m_dmaBlockSize = 0;
        bool m_queueComplete = false;
        int m_port = 0;
        std::deque<uint8_t> m_fifoIn;
        std::deque<uint8_t> m_fifoOut;
        Pad m_pads[2];
    };
}
