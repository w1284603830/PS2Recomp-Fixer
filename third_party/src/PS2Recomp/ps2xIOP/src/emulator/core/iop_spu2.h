#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2x::iop
{
    // [DoD] Audio trace hooks, set by the runtime when the test launcher's audio trace is on (DOD_AUDIO_TRACE), null
    // otherwise: every output frame's stems (core 0 L/R, core 1 sound data input L/R, core 1 voices L/R) and SPU2
    // events such as an AutoDMA underrun (text). Called on the IOP thread.
    extern void (*g_dodSpu2StemsSink)(const int16_t *stems6);
    extern void (*g_dodSpu2EventSink)(const char *text);

    // [DoD] DOD_ADMA_WATCH=1 (diagnostics): per IOP RAM halfword, the SPU2 sample at which it was last written (+1,
    // 0 = never); null when off. IopMemory's write paths update it, IopSpu2::pullAutoDma reports AutoDMA data that was
    // not rewritten since the SPU2 last pulled it (stale) or that was written only just before the pull.
    extern uint32_t *g_dodAdmaLastWrite;
    extern uint32_t g_dodAdmaNow;
    // Writer of each halfword: the storing instruction's pc, or (HLE memcpy) the caller's ra | 1 with the source
    // address in g_dodAdmaLastSrc; 2 = another block write (SIF, loader, DMA).
    extern uint32_t *g_dodAdmaLastPc;
    extern uint32_t *g_dodAdmaLastSrc;
    extern uint32_t g_dodIopStorePc;
    extern bool g_dodAdmaCopyActive;
    extern uint32_t g_dodAdmaCopyDelta; // source - destination of the HLE memcpy in progress
    // A CPU load by an instruction in DOD_ADMA_WATCH_PC (lo:hi, hex; default CRI_ADXI's software mixer at module base
    // 0xBA400 + 0x3C00..0x4D30) of data that was not rewritten since that halfword was last read (stale).
    extern uint32_t g_dodAdmaLoadPcLo;
    extern uint32_t g_dodAdmaLoadPcHi;
    void dodAdmaNoteLoad(uint32_t pc, uint32_t address, uint32_t size);

    // IOP thread switches where the thread that ran was still ready (its timeslice ended, it was preempted), kept by
    // the emulator while DOD_ADMA_WATCH is on; printed with the next right-channel REPEAT.
    struct DodSchedEvent
    {
        uint32_t sample;
        int fromId, fromPrio;
        uint32_t fromPc;
        int toId, toPrio;
        uint32_t toPc;
    };
    extern DodSchedEvent g_dodSchedRing[256];
    extern uint32_t g_dodSchedCount;
    extern void (*g_dodDescribeIopThreads)();

    // [DoD] DOD_IO_CENSUS=<file>: IOP hardware registers (0x1F80xxxx) and SPU2 registers (0x1F900000 + offset) the IOP
    // reads or writes, written to <file>.iop.csv every 20 s and at exit (see dodio in ps2xRuntime's ps2_memory.cpp).
    void dodIopIoNote(uint32_t address, bool write, uint32_t value);
}

namespace ps2x::iop::detail
{
    // SPU2 sound processor: two cores of 24 ADPCM voices each, 2 MB of sound RAM, the
    // register file at 0x1F900000 and DMA channels 4/7. Produces 48 kHz stereo output.
    // Noise is not emulated yet (DoD does not use it).
    // (ran-j/PS2Recomp#308; [DoD] AutoDMA input, AVOL/BVOL routing and the ENDX/NAX fixes noted below; reverb, wet
    // mixing and Gaussian interpolation as PCSX2's SPU2 Reverb.cpp / ReverbResample.cpp / Mixer.cpp)
    class IopSpu2
    {
    public:
        static constexpr uint32_t RamHalfwords = 1u << 20u;
        static constexpr uint32_t RegisterBytes = 0x800u;
        static constexpr uint32_t SampleRate = 48000u;
        static constexpr uint32_t IopCyclesPerSample = 768u; // 36.864 MHz / 48 kHz

        IopSpu2();

        void reset();

        // `offset` is the byte offset from 0x1F900000.
        [[nodiscard]] uint16_t read16(uint32_t offset) const;
        [[nodiscard]] uint16_t read16Raw(uint32_t offset) const; // [DoD] without DOD_IO_CENSUS
        void write16(uint32_t offset, uint16_t value);

        void dmaWrite(int core, const uint8_t *source, uint32_t bytes);
        void dmaRead(int core, uint8_t *destination, uint32_t bytes);

        // Mixes up to `frames` stereo samples into `out` (interleaved L/R) and returns how many it produced: it stops
        // after the sample in which an AutoDMA block ended, so the caller can raise the DMA interrupt (and the driver
        // queue the next block) before the SPU2 needs more input.
        uint32_t generate(int16_t *out, uint32_t frames);

        // [DoD] AutoDMA (LIBSD sceSdBlockTrans): a DMA 4/7 write started while the core's ADMAS bit is set is not
        // copied at once. The SPU2 pulls it into the core's input area (0x2000/0x2400 left, 0x2200/0x2600 right,
        // 0x200 samples each) one half (0x100 samples, 1 KB of the 512-byte L/R blocks) at a time, one half ahead of
        // what it plays (PCSX2's AutoDMAReadBuffer), and the DMA ends when its last bytes were pulled. The input area
        // is always mixed, through BVOL and MMIX bits 0x80/0x40 (PCSX2's ReadInput).
        [[nodiscard]] bool autoDmaEnabled(int core) const noexcept;
        void autoDmaStart(int core, const uint8_t *source, uint32_t bytes, uint32_t iopAddress = 0u);
        void autoDmaStop(int core);
        [[nodiscard]] bool takeAutoDmaDone(int core) noexcept;

        // True once after a voice, DMA or data-port access hit an enabled IRQA.
        [[nodiscard]] bool takeIrq() noexcept;

    private:
        enum class Phase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        struct Envelope
        {
            uint32_t counter = 0u;
            uint32_t increment = 0x8000u;
            int32_t step = 0;
            uint8_t rate = 0u;
            bool decreasing = false;
            bool exponential = false;

            void reset(uint8_t rate, bool decreasing, bool exponential);
            [[nodiscard]] int32_t tick(int32_t level);
        };

        // [DoD] Voice / master volume with slide mode (bit 15), as PCSX2's V_VolumeSlide::Update (was a magnitude
        // envelope that flipped the sign at once for the negative-phase bit).
        struct Volume
        {
            uint16_t reg = 0u;
            int32_t current = 0;
            uint32_t counter = 0u;

            void write(uint16_t value);
            void tick();
        };

        struct Voice
        {
            Volume volL;
            Volume volR;
            uint16_t pitch = 0u;
            uint16_t adsr1 = 0u;
            uint16_t adsr2 = 0u;
            uint32_t ssa = 0u;
            uint32_t lsax = 0u;
            uint32_t nax = 0u;

            Phase phase = Phase::Off;
            int32_t envLevel = 0;
            Envelope env;

            uint32_t counter = 0u;
            bool blockLoaded = false;
            uint8_t blockFlags = 0u;
            int32_t adpcm1 = 0;
            int32_t adpcm2 = 0;
            // [0..2] = last samples of the previous block, [3..30] = current block.
            std::array<int16_t, 31> samples{};
            int32_t lastOutput = 0;
        };

        struct Core
        {
            std::array<Voice, 24> voices;
            uint32_t pmon = 0u;
            uint32_t non = 0u;
            uint32_t vmixl = 0u;
            uint32_t vmixr = 0u;
            uint32_t vmixel = 0u;
            uint32_t vmixer = 0u;
            uint16_t mmix = 0u;
            uint16_t attr = 0u;
            uint32_t irqa = 0u;
            uint32_t tsa = 0u;
            uint32_t endx = 0u;
            uint16_t statx = 0u;
            uint16_t admas = 0u;
            Volume mvolL;
            Volume mvolR;
            int16_t avolL = 0;
            int16_t avolR = 0;
            int16_t bvolL = 0;
            int16_t bvolR = 0;
            int16_t evolL = 0;
            int16_t evolR = 0;

            // [DoD] Reverb (PCSX2 V_Core::DoReverb): the effects area ESA..EEA (EEA: only its high halfword is
            // written, 0x33C), the 22 address offsets from ESA in register order (0x2E4 APF1_SIZE .. 0x338
            // APF2_R_DST, high then low halfword) and the 10 volumes of the master area (+0x14 IIR_VOL .. +0x26
            // IN_COEF_R); ATTR bit 7 (FxEnable) allows the writes to the work area. The 39-tap down/up-sampling
            // buffers hold each sample twice (at pos and pos + 64).
            uint32_t esa = 0u;
            uint32_t eea = 0u;
            std::array<uint32_t, 22> revbAddr{};
            std::array<int16_t, 10> revbVol{};
            std::array<std::array<int16_t, 128>, 2> revbDown{};
            std::array<std::array<int16_t, 128>, 2> revbUp{};
            uint32_t revbPos = 0u;

            struct AutoDmaBlock
            {
                const uint8_t *data = nullptr;
                uint32_t bytes = 0u;
                uint32_t offset = 0u;
                uint32_t iopAddress = 0u; // [DoD] DOD_ADMA_WATCH
                uint32_t startSample = 0u;
            };
            std::deque<AutoDmaBlock> autoDma;
            uint32_t inputPos = 0u; // 0..0x1FF in the input area
            bool autoDmaDone = false;
        };

        void writeVoiceParam(Core &core, Voice &voice, uint32_t reg, uint16_t value);
        [[nodiscard]] uint16_t readVoiceParam(const Voice &voice, uint32_t reg) const;
        void writeVoiceAddress(Voice &voice, uint32_t reg, uint16_t value);
        [[nodiscard]] uint16_t readVoiceAddress(const Voice &voice, uint32_t reg) const;
        void writeCoreRegister(int coreIndex, uint32_t offset, uint16_t value);
        [[nodiscard]] bool readCoreRegister(int coreIndex, uint32_t offset, uint16_t &value) const;

        void keyOn(Core &core, uint32_t mask);
        void keyOff(Core &core, uint32_t mask);
        void loadBlock(Core &core, Voice &voice);
        [[nodiscard]] int32_t sampleVoice(Core &core, int voiceIndex, int32_t previousOutput);
        void tickEnvelope(Voice &voice);
        void checkIrq(uint32_t begin, uint32_t count);
        bool pullAutoDma(int coreIndex, uint32_t half);
        [[nodiscard]] uint32_t reverbIndex(const Core &core, uint32_t offset) const;
        void doReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, RegisterBytes / 2u> m_regs{};
        std::array<Core, 2> m_cores{};
        bool m_irqPending = false;
        int32_t m_stemInputL = 0, m_stemInputR = 0, m_stemVoicesL = 0, m_stemVoicesR = 0; // [DoD] diagnostics
        uint32_t m_sampleCounter = 0u;                                                    // [DoD] diagnostics
    };
}
