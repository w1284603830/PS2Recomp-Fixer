// Frozen public legacy function from main 05807db13681af24443e6514602b028fcd622e9e.
// Local ea949e9 has the identical source tree. Only the method name is changed.
// Differential reference, not a second production implementation or game validation.
const char* GSRendererHW::LegacyParticleEdges(const GSTextureCache::Source* tex, float rtscale)
{
	if (!m_process_texture || !tex)
		return "no-texture";
	if (tex->m_target || tex->m_from_target)
		return "rt-source";
	if (!m_vt.IsLinear())
		return "nearest";
	if (!m_cached_ctx.TEST.ZTE || m_cached_ctx.TEST.ZTST == ZTST_NEVER || m_cached_ctx.TEST.ZTST == ZTST_ALWAYS)
		return "no-ztest";
	const u32 count = m_vertex->next;
	GSVertex* v = &m_vertex->buff[0];
	const bool fst = PRIM->FST;
	for (u32 i = 0; i < count; i++)
	{
		if (v[i].XYZ.Z >= 0xFF00u)
			return "hud-z"; // the HUD layer
		if (!fst && (i & 1) && (v[i].RGBAQ.Q == 0.0f || v[i - 1].RGBAQ.Q != v[i].RGBAQ.Q))
			return "q-varies";
	}
	const int ox = m_context->XYOFFSET.OFX;
	const int oy = m_context->XYOFFSET.OFY;
	const float tw = static_cast<float>(1 << m_cached_ctx.TEX0.TW);
	const float th = static_cast<float>(1 << m_cached_ctx.TEX0.TH);
	// texels: half a texel of the coarsest level the filter reads (level 0 only without mip filtering, MMIN 0 / 1)
	const int levels = m_context->TEX1.MMIN >= 2 ? std::min<int>(m_context->TEX1.MXL, 6) : 0;
	const float inset = 0.5f * static_cast<float>(1 << levels);
	const auto texels = [&](const GSVertex& p, float q, float& u, float& w) {
		u = fst ? p.U / 16.0f : p.ST.S / q * tw;
		w = fst ? p.V / 16.0f : p.ST.T / q * th;
	};

	// One axis of one sprite: positions p0 / p1 (1/16 pixel, offset removed), texture coordinates c0 / c1 (texels)
	// -> positions q0 / q1 on the native pixel grid and their texture coordinates d0 / d1.
	const auto axis = [&](int p0, int p1, float c0, float c1, int& q0, int& q1, float& d0, float& d1) {
		const int n0 = (std::min(p0, p1) + 15) >> 4; // native pixels [n0, n1)
		const int n1 = (std::max(p0, p1) + 15) >> 4;
		q0 = (p0 <= p1 ? n0 : n1) << 4;
		q1 = (p0 <= p1 ? n1 : n0) << 4;
		d0 = c0;
		d1 = c1;
		if (n1 <= n0)
			return; // fills no pixel
		const float slope = (c1 - c0) / static_cast<float>(p1 - p0); // texels per 1/16 pixel
		float a = c0 + slope * static_cast<float>((n0 << 4) - p0); // native first sample
		float b = c0 + slope * static_cast<float>(((n1 - 1) << 4) - p0); // native last sample
		const float lo = std::min(c0, c1) + inset;
		const float hi = std::max(c0, c1) - inset;
		if (lo <= hi) // a texture rectangle wider than one texel: stay half a texel inside it
		{
			a = std::clamp(a, lo, hi);
			b = std::clamp(b, lo, hi);
		}
		const float n = static_cast<float>(n1 - n0);
		const float s0 = a; // at n0: the first internal pixel samples a
		const float s1 = a + (b - a) * n / (n - 1.0f / rtscale); // at n1: the last one (n1 - 1 / scale) samples b
		d0 = p0 <= p1 ? s0 : s1;
		d1 = p0 <= p1 ? s1 : s0;
	};
	const auto to_xy = [](int origin, int q) { return static_cast<u16>(std::clamp(origin + q, 0, 0xFFFF)); };
	const auto to_uv = [](float d) { return static_cast<u16>(std::clamp(static_cast<int>(std::lround(d * 16.0f)), 0, 0x3FFF)); };

	GSVector4i filled = m_r; // the draw rectangle grows to the native pixels the sprites now fill
	for (u32 i = 0; i + 1 < count; i += 2)
	{
		GSVertex& v0 = v[i];
		GSVertex& v1 = v[i + 1];
		const float q = v1.RGBAQ.Q;
		float u0, w0, u1, w1;
		texels(v0, q, u0, w0);
		texels(v1, q, u1, w1);
		int x0, x1, y0, y1;
		axis(v0.XYZ.X - ox, v1.XYZ.X - ox, u0, u1, x0, x1, u0, u1);
		axis(v0.XYZ.Y - oy, v1.XYZ.Y - oy, w0, w1, y0, y1, w0, w1);
		if (x0 != x1 && y0 != y1)
			filled = filled.runion(GSVector4i(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)).sra32<4>());
		v0.XYZ.X = to_xy(ox, x0);
		v1.XYZ.X = to_xy(ox, x1);
		v0.XYZ.Y = to_xy(oy, y0);
		v1.XYZ.Y = to_xy(oy, y1);
		if (fst)
		{
			v0.U = to_uv(u0);
			v1.U = to_uv(u1);
			v0.V = to_uv(w0);
			v1.V = to_uv(w1);
		}
		else
		{
			v0.ST.S = u0 / tw * q;
			v1.ST.S = u1 / tw * q;
			v0.ST.T = w0 / th * q;
			v1.ST.T = w1 / th * q;
		}
	}
	m_r = filled.rintersect(m_context->scissor.in);
	return "applied";
}

