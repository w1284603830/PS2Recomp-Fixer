// [DoD] IOP SIO2 + DualShock 2, following PCSX2's pcsx2/SIO/Sio2.cpp and SIO/Pad/PadDualshock2.cpp (GPL-3.0+).

#include "iop_sio2.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ps2x::iop
{
    void (*g_dodPadRumbleOutput)(int port, uint8_t large, uint8_t small) = nullptr;
    bool (*g_dodPadStateProvider)(int port, uint8_t *data32) = nullptr;
    // Private runner bridge: only normal input polls consume retained presses.
    bool (*g_dodPadPollStateProvider)(int port, uint8_t *data32) = nullptr;
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kCmdStatDisconnected = 0x1D100u;
        constexpr uint32_t kCmdStatNoDevicesMissing = 0x1000u;
        constexpr uint32_t kCmdStatPort1Missing = 0x1D000u;
        constexpr uint32_t kCmdStatPort2Missing = 0x2D000u;
        constexpr uint32_t kCmdStatOnePortOpen = 0x100u;
        constexpr uint32_t kCmdStatTwoPortsOpen = 0x200u;

        constexpr uint8_t kModeDigital = 0x41u;
        constexpr uint8_t kModeAnalog = 0x73u;
        constexpr uint8_t kModeDualShock2 = 0x79u;
        constexpr uint8_t kModeConfig = 0xF3u;

        inline bool pressed(uint32_t buttons, unsigned bit)
        {
            return (buttons & (1u << bit)) == 0u;
        }
    }

    void IopSio2::reset()
    {
        if (g_dodPadRumbleOutput) g_dodPadRumbleOutput(0, 0, 0);
        *this = IopSio2{};
    }

    void IopSio2::softReset()
    {
        m_queueRead = false;
        m_queuePosition = 0;
        m_commandLength = 0;
        m_dmaBlockSize = 0;
        m_queueComplete = false;
        m_fifoIn.clear();
        m_cmdStat = 0;
    }

    uint32_t IopSio2::read32(uint32_t phys)
    {
        const uint32_t off = phys - 0x1F808200u;
        if (off < 0x40u)
            return m_cmdQueue[off / 4u];
        if (off < 0x60u)
            return m_portCtrl[(off - 0x40u) / 4u];
        switch (off)
        {
        case 0x64u:
        {
            uint8_t value = 0xFFu;
            if (!m_fifoOut.empty())
            {
                value = m_fifoOut.front();
                m_fifoOut.pop_front();
            }
            return value;
        }
        case 0x68u:
            return m_ctrl;
        case 0x6Cu:
            return m_cmdStat;
        case 0x70u:
            return m_portStat;
        case 0x74u:
            return m_fifoStat;
        case 0x80u:
            return m_iStat;
        default:
            return 0u;
        }
    }

    void IopSio2::write32(uint32_t phys, uint32_t value)
    {
        const uint32_t off = phys - 0x1F808200u;
        if (off < 0x40u)
        {
            m_cmdQueue[off / 4u] = value;
            if (off == 0u)
                softReset();
            return;
        }
        if (off < 0x60u)
        {
            m_portCtrl[(off - 0x40u) / 4u] = value;
            return;
        }
        switch (off)
        {
        case 0x60u:
            pushIn(static_cast<uint8_t>(value));
            break;
        case 0x68u:
            m_ctrl = value;
            if (value & 1u)
            {
                if (!m_iStat)
                    m_irqPending = true;
                m_iStat |= 1u;
            }
            break;
        case 0x80u:
            m_iStat &= ~value;
            break;
        default:
            break;
        }
    }

    void IopSio2::dmaIn(const uint8_t *data, size_t bytes, uint32_t blockBytes)
    {
        m_dmaBlockSize = blockBytes;
        for (size_t i = 0; i < bytes; ++i)
            pushIn(data[i]);
    }

    void IopSio2::dmaOut(uint8_t *data, size_t bytes)
    {
        for (size_t i = 0; i < bytes; ++i)
        {
            uint8_t value = 0xFFu;
            if (!m_fifoOut.empty())
            {
                value = m_fifoOut.front();
                m_fifoOut.pop_front();
            }
            data[i] = value;
        }
    }

    bool IopSio2::takeInterrupt()
    {
        const bool pending = m_irqPending;
        m_irqPending = false;
        return pending;
    }

    void IopSio2::pushIn(uint8_t value)
    {
        if (!m_queueRead)
        {
            if (m_queuePosition >= 16u)
                return;
            const uint32_t cmd = m_cmdQueue[m_queuePosition];
            m_port = static_cast<int>(cmd & 1u);
            m_commandLength = (cmd >> 8) & 0x3FFu;
            m_queueRead = true;
            if (m_commandLength == 0u)
                m_queueComplete = true;
            m_fifoIn.clear();
        }
        if (m_queueComplete)
            return;

        m_fifoIn.push_back(value);
        if ((m_fifoIn.size() == m_commandLength && m_dmaBlockSize == 0u) ||
            (m_dmaBlockSize != 0u && m_fifoIn.size() == m_dmaBlockSize))
        {
            m_queueRead = false;
            ++m_queuePosition;
            runCommand();
            if (m_dmaBlockSize > 0u)
            {
                const size_t rem = m_fifoOut.size() % m_dmaBlockSize;
                if (rem != 0u)
                    m_fifoOut.insert(m_fifoOut.end(), m_dmaBlockSize - rem, 0x00u);
            }
        }
    }

    void IopSio2::runCommand()
    {
        static uint32_t s_logged = 0u; // [DoD] diagnostics: the first SIO2 commands
        const size_t outBefore = m_fifoOut.size();
        std::string inText;
        if (s_logged < 40u)
        {
            for (uint8_t b : m_fifoIn)
            {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%02X ", b);
                inText += hex;
            }
        }
        struct LogOnExit
        {
            IopSio2 &self;
            size_t outBefore;
            const std::string &inText;
            ~LogOnExit()
            {
                if (s_logged >= 40u)
                    return;
                ++s_logged;
                std::string outText;
                for (size_t i = outBefore; i < self.m_fifoOut.size() && i < outBefore + 24u; ++i)
                {
                    char hex[4];
                    std::snprintf(hex, sizeof(hex), "%02X ", self.m_fifoOut[i]);
                    outText += hex;
                }
                std::fprintf(stderr, "[sio2] port %d in: %s-> out: %s stat=%05X\n", self.m_port, inText.c_str(), outText.c_str(),
                             self.m_cmdStat);
            }
        } logOnExit{*this, outBefore, inText};

        const uint8_t mode = m_fifoIn.front();
        m_fifoIn.pop_front();

        auto markPortOpened = [this]()
        {
            if (m_cmdStat & kCmdStatOnePortOpen)
            {
                m_cmdStat &= ~kCmdStatOnePortOpen;
                m_cmdStat |= kCmdStatTwoPortsOpen;
            }
            else
            {
                m_cmdStat |= kCmdStatOnePortOpen;
            }
            m_cmdStat |= kCmdStatNoDevicesMissing;
        };
        auto answerNothing = [this]()
        {
            m_fifoOut.push_back(0xFFu);
            while (!m_fifoIn.empty())
            {
                m_fifoIn.pop_front();
                m_fifoOut.push_back(0xFFu);
            }
        };

        switch (mode)
        {
        case 0x01: // pad
        {
            markPortOpened();
            Pad &pad = m_pads[m_port & 1];
            const bool normalPoll = !pad.inConfig &&
                                    (pad.mode == kModeDigital || pad.mode == kModeAnalog || pad.mode == kModeDualShock2) &&
                                    m_commandLength >= 5u && m_fifoIn.size() >= 4u && m_fifoIn.front() == 0x42u;
            const auto provider = normalPoll && g_dodPadPollStateProvider ? g_dodPadPollStateProvider : g_dodPadStateProvider;
            const bool present = m_port == 0 && provider && provider(m_port, pad.state);
            if (!present)
            {
                if (g_dodPadRumbleOutput) g_dodPadRumbleOutput(m_port, 0, 0);
                m_cmdStat |= (m_port == 0) ? kCmdStatPort1Missing : kCmdStatPort2Missing;
                answerNothing();
                break;
            }
            const size_t replyStart = m_fifoOut.size();
            m_fifoOut.push_back(0xFFu);
            pad.received = 1u;
            pad.command = 0u;
            pad.largeMotor = pad.smallMotor = 0;
            while (!m_fifoIn.empty())
            {
                const uint8_t commandByte = m_fifoIn.front();
                m_fifoIn.pop_front();
                m_fifoOut.push_back(padByte(pad, commandByte));
            }
            if (g_dodPadRumbleOutput && (normalPoll || pad.command == 0x4D || pad.command == 0x44))
                g_dodPadRumbleOutput(m_port, normalPoll ? pad.largeMotor : 0, normalPoll ? pad.smallMotor : 0);
            if (normalPoll)
            {
                // Trace the actual wire reply, after the pending press has
                // been sampled, rather than merely tracing a provider read.
                static const bool tracePad = std::getenv("DOD_PAD_TRACE") != nullptr;
                static uint64_t pollSequence = 0u;
                static unsigned traceCount = 0u;
                static int lastButtons = -1, lastMode = -1;
                ++pollSequence;
                const uint16_t buttons = static_cast<uint16_t>(m_fifoOut[replyStart + 3u] |
                                                               (m_fifoOut[replyStart + 4u] << 8));
                const int replyMode = m_fifoOut[replyStart + 1u];
                if (tracePad && traceCount < 1024u && (lastButtons != buttons || lastMode != replyMode))
                {
                    const bool hasPressure = replyMode == kModeDualShock2 && m_commandLength >= 21u &&
                                             m_fifoOut.size() - replyStart >= 21u;
                    std::fprintf(stderr, "[pad] sio2 poll=%llu port=%d mode=%02X buttons=%04X length=%u pressure_circle=%d pressure_cross=%d\n",
                                 static_cast<unsigned long long>(pollSequence), m_port, static_cast<unsigned>(replyMode),
                                 static_cast<unsigned>(buttons), m_commandLength,
                                 hasPressure ? m_fifoOut[replyStart + 14u] : -1,
                                 hasPressure ? m_fifoOut[replyStart + 15u] : -1);
                    lastButtons = buttons;
                    lastMode = replyMode;
                    ++traceCount;
                }
            }
            break;
        }
        case 0x21: // multitap: none
            markPortOpened();
            m_cmdStat |= kCmdStatPort1Missing;
            answerNothing();
            break;
        default: // memory card (0x81), infrared (0x61), unknown: nothing attached yet
            m_cmdStat = kCmdStatDisconnected;
            answerNothing();
            break;
        }
    }

    uint8_t IopSio2::padByte(Pad &pad, uint8_t commandByte)
    {
        const uint32_t n = pad.received++;
        if (n == 0u)
            return 0x00u;
        if (n == 1u)
        {
            pad.command = commandByte;
            return pad.inConfig ? kModeConfig : pad.mode;
        }
        if (n == 2u)
            return 0x5Au;

        const uint32_t buttons = (static_cast<uint32_t>(pad.state[2]) << 8) | pad.state[3];
        switch (pad.command)
        {
        case 0x40: // mystery
            return n == 5u ? 0x02u : (n == 8u ? 0x5Au : 0x00u);
        case 0x41: // button query
            if (pad.mode == kModeDualShock2 || pad.mode == kModeAnalog)
            {
                if (n == 3u || n == 4u)
                    return 0xFFu;
                if (n == 5u)
                    return 0x03u;
                if (n == 8u)
                    return 0x5Au;
            }
            return 0x00u;
        case 0x42: // poll
            if (!pad.inConfig && n >= 3u && n <= 8u && n < m_commandLength)
            {
                if (pad.motorMap[n - 3u] == 0u) pad.smallMotor = commandByte & 1u;
                if (pad.motorMap[n - 3u] == 1u) pad.largeMotor = commandByte;
            }
            switch (n)
            {
            case 3: return static_cast<uint8_t>(buttons >> 8);
            case 4: return static_cast<uint8_t>(buttons);
            case 5: return pad.state[4]; // right stick X
            case 6: return pad.state[5]; // right stick Y
            case 7: return pad.state[6]; // left stick X
            case 8: return pad.state[7]; // left stick Y
            case 9: return pressed(buttons, 13) ? 0xFFu : 0u;  // right
            case 10: return pressed(buttons, 15) ? 0xFFu : 0u; // left
            case 11: return pressed(buttons, 12) ? 0xFFu : 0u; // up
            case 12: return pressed(buttons, 14) ? 0xFFu : 0u; // down
            case 13: return pressed(buttons, 4) ? 0xFFu : 0u;  // triangle
            case 14: return pressed(buttons, 5) ? 0xFFu : 0u;  // circle
            case 15: return pressed(buttons, 6) ? 0xFFu : 0u;  // cross
            case 16: return pressed(buttons, 7) ? 0xFFu : 0u;  // square
            case 17: return pressed(buttons, 2) ? 0xFFu : 0u;  // L1
            case 18: return pressed(buttons, 3) ? 0xFFu : 0u;  // R1
            case 19: return pressed(buttons, 0) ? 0xFFu : 0u;  // L2
            case 20: return pressed(buttons, 1) ? 0xFFu : 0u;  // R2
            default: return 0x00u;
            }
        case 0x43: // enter / leave config mode
            if (n == 3u)
                pad.inConfig = commandByte != 0u;
            return 0x00u;
        case 0x44: // mode switch
            if (n == 3u)
            {
                pad.analogLight = commandByte != 0u;
                pad.mode = pad.analogLight ? kModeAnalog : kModeDigital;
            }
            else if (n == 4u)
            {
                pad.analogLocked = commandByte == 0x03u;
            }
            return 0x00u;
        case 0x45: // status info
            switch (n)
            {
            case 3: return 0x03u; // standard controller
            case 4: return 0x02u;
            case 5: return pad.analogLight ? 1u : 0u;
            case 6: return 0x02u;
            case 7: return 0x01u;
            default: return 0x00u;
            }
        case 0x46: // constant 1
            switch (n)
            {
            case 3:
                pad.commandStage = commandByte != 0u;
                return 0x00u;
            case 5: return 0x01u;
            case 6: return pad.commandStage ? 0x01u : 0x02u;
            case 7: return pad.commandStage ? 0x01u : 0x00u;
            case 8: return pad.commandStage ? 0x14u : 0x0Au;
            default: return 0x00u;
            }
        case 0x47: // constant 2
            return n == 5u ? 0x02u : (n == 7u ? 0x01u : 0x00u);
        case 0x4C: // constant 3
            if (n == 3u)
            {
                pad.commandStage = commandByte != 0u;
                return 0x00u;
            }
            return n == 6u ? (pad.commandStage ? 0x07u : 0x04u) : 0x00u;
        case 0x4D: // Six poll-payload actuator slots: 0=small, 1=large, FF=unused.
            if (n >= 3u && n <= 8u && n < m_commandLength)
            {
                const uint8_t old = pad.motorMap[n - 3u];
                pad.motorMap[n - 3u] = commandByte;
                return old;
            }
            return 0xFFu;
        case 0x4F: // response bytes (analog / pressure)
            if (n == 3u)
                pad.responseBytes = commandByte;
            else if (n == 4u)
                pad.responseBytes |= static_cast<uint32_t>(commandByte) << 8;
            else if (n == 5u)
            {
                pad.responseBytes |= static_cast<uint32_t>(commandByte) << 16;
                if (pad.responseBytes == 0x3Fu)
                {
                    pad.analogLight = true;
                    pad.mode = kModeAnalog;
                }
                else if (pad.responseBytes == 0x3FFFFu)
                {
                    pad.analogLight = true;
                    pad.mode = kModeDualShock2;
                }
                else
                {
                    pad.analogLight = false;
                    pad.mode = kModeDigital;
                }
            }
            else if (n == 8u)
                return 0x5Au;
            return 0x00u;
        default:
            return 0x00u;
        }
    }
}
