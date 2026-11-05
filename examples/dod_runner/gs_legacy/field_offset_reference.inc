// Historical reference only. Not compiled into the shared transport.
    // [DoD] DoD renders its interlaced fields (SMODE2 FFMD, 224-line fields at 60 fps) with the 3D half a line apart on
    // alternate fields (XYOFFSET OFY x.5 on one buffer, x.0 on the other) for a TV that puts odd fields one scanline
    // lower; its 2D is drawn at the same place on both. PCSX2 shows the fields progressive (SYNCV left as the port sets
    // it), so the 3D shook by a line from field to field. This takes the half line out of the copy of the GIF stream
    // that PCSX2's GS gets (the game's memory and state are untouched): every field lines up. A tag walker per GIF
    // path, because tags continue across transfers. DOD_GS_FIELD_OFFSET=keep sends the stream as it is.
    struct GifFieldOffsetFix
    {
        uint32_t left = 0u;
        uint32_t nreg = 1u;
        uint32_t idx = 0u;
        uint32_t flg = 0u;
        uint8_t regs[16]{};
    };
    GifFieldOffsetFix g_fieldFix[4];

    bool fieldOffsetFixOn()
    {
        static const bool on = []() { const char *v = std::getenv("DOD_GS_FIELD_OFFSET"); return !(v && std::strcmp(v, "keep") == 0); }();
        return on;
    }

    void fixFieldOffsets(uint32_t path, uint8_t *data, uint32_t bytes)
    {
        GifFieldOffsetFix &st = g_fieldFix[path & 3u];
        for (uint32_t pos = 0u; pos + 16u <= bytes; pos += 16u)
        {
            uint64_t lo = 0u;
            uint64_t hi = 0u;
            std::memcpy(&lo, data + pos, 8u);
            std::memcpy(&hi, data + pos + 8u, 8u);
            if (st.left == 0u)
            {
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                st.flg = static_cast<uint32_t>((lo >> 58u) & 3u);
                st.nreg = static_cast<uint32_t>((lo >> 60u) & 0xFu);
                if (st.nreg == 0u)
                    st.nreg = 16u;
                for (uint32_t i = 0u; i < 16u; ++i)
                    st.regs[i] = static_cast<uint8_t>((hi >> (4u * i)) & 0xFu);
                st.idx = 0u;
                st.left = st.flg == 0u ? nloop * st.nreg : st.flg == 1u ? (nloop * st.nreg + 1u) / 2u : nloop;
                continue;
            }
            if (st.flg == 0u)
            {
                if (st.regs[st.idx % st.nreg] == 0xEu) // A+D
                {
                    const uint32_t addr = static_cast<uint32_t>(hi & 0xFFu);
                    if ((addr == 0x18u || addr == 0x19u) && ((lo >> 32u) & 0xFu) == 8u) // XYOFFSET_1/2, OFY x.5
                    {
                        // DOD_GS_FIELD_LOG=<n>: the first n changes (path, offset, the qword before the change)
                        static const int logLeft = []() { const char *v = std::getenv("DOD_GS_FIELD_LOG"); return v ? std::atoi(v) : 0; }();
                        static int logged = 0;
                        if (logged < logLeft)
                        {
                            ++logged;
                            std::fprintf(stderr, "[gs-field] path %u pos %u nreg %u idx %u left %u lo %016llX hi %016llX\n", path, pos,
                                         st.nreg, st.idx, st.left, static_cast<unsigned long long>(lo),
                                         static_cast<unsigned long long>(hi));
                        }
                        lo &= ~(0xFull << 32u);
                        std::memcpy(data + pos, &lo, 8u);
                    }
                }
                ++st.idx;
            }
            --st.left;
        }
    }
