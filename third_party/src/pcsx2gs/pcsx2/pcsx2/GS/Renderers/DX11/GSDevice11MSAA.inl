// DoD optional geometry MSAA. Keep sample history across compatible triangle draws.
// Resolve before feedback, sprites, destination-alpha tests, or any outside access
// to the original color/depth. Those PS2-specific operations keep the original path.
void GSDevice11::ResolveMSAAForTexture(GSTexture* texture)
{
	if (!m_msaa_internal && m_msaa_source_rt &&
		(texture == m_msaa_source_rt || texture == m_msaa_source_ds))
		FlushMSAA();
}

void GSDevice11::FlushMSAA()
{
	if (!m_msaa_source_rt || m_msaa_internal) return;
	m_msaa_internal = true;
	GSTexture* rt = m_msaa_source_rt;
	GSTexture* ds = m_msaa_source_ds;
	m_msaa_source_rt = m_msaa_source_ds = nullptr;
	// A logical clear may have been queued since the last geometry draw. Preserve
	// it for the caller after resolving the preceding draw's physical contents.
	const auto rt_state = rt->GetState(), ds_state = ds->GetState();
	rt->SetState(GSTexture::State::Dirty);
	ds->SetState(GSTexture::State::Dirty);
	OMSetRenderTargets(nullptr, nullptr, nullptr);
	m_ctx->ResolveSubresource(*static_cast<GSTexture11*>(rt), 0,
		*static_cast<GSTexture11*>(m_msaa_rt.get()), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
	const GSVector4 rect(0, 0, ds->GetWidth(), ds->GetHeight());
	DoStretchRect(m_msaa_ds.get(), GSVector4(0, 0, 1, 1), ds, rect,
		m_msaa_depth_resolve.get(), nullptr, Nearest);
	rt->SetState(rt_state);
	ds->SetState(ds_state);
	m_msaa_internal = false;
	++m_msaa_epochs;
}

bool GSDevice11::TryRenderMSAA(GSHWDrawConfig& config)
{
	if (!m_msaa_checked)
	{
		m_msaa_checked = true;
		UINT color = 0, depth = 0;
		const UINT samples = GSConfig.MSAASamples;
		if (SUCCEEDED(m_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &color)) && color &&
			SUCCEEDED(m_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, samples, &depth)) && depth)
		{
			D3D11_RASTERIZER_DESC rd;
			m_rs->GetDesc(&rd);
			rd.MultisampleEnable = TRUE;
			const auto shader = ReadShaderSource("shaders/dx11/msaa_depth_resolve.fx");
			if (shader && SUCCEEDED(m_dev->CreateRasterizerState(&rd, m_msaa_rs.put())))
			{
				ShaderMacro sm;
				sm.AddMacro("MSAA_SAMPLES", samples);
				m_msaa_depth_resolve = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm.GetPtr(), "main");
				if (m_msaa_depth_resolve) m_msaa_samples = samples;
			}
		}
		std::fprintf(stderr, "[msaa] requested=%u supported_active=%u geometry samples\n", samples, m_msaa_samples);
	}
	if (m_msaa_samples < 2) return false;
	const bool eligible = config.rt && config.ds && config.rt->GetFormat() == GSTexture::Format::Color &&
		config.ds->GetFormat() == GSTexture::Format::DepthStencil && config.rt->GetSize() == config.ds->GetSize() &&
		config.topology == GSHWDrawConfig::Topology::Triangle && config.vs.expand == GSHWDrawConfig::VSExpand::None &&
		// Shader-side PS2 blending/feedback is separate from the D3D blend state.
		// It needs the original single-sample target even when blend.enable is false.
		config.depth.ztst >= 2 && !config.IsBlending() &&
		!config.IsFeedbackLoopRT(config.ps) && !config.IsFeedbackLoopDepth(config.ps) &&
		config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Off &&
		!config.require_one_barrier && !config.require_full_barrier &&
		!config.alpha_second_pass.enable && !config.blend_multi_pass.enable &&
		!config.ps.HasColorROV() && !config.ps.HasDepthROV() && !config.ps.colclip_hw &&
		!GetColorClipTexture() && !m_ds_as_rt && config.tex != config.rt && config.tex != config.ds;
	if (!eligible)
	{
		FlushMSAA();
		return false;
	}
	if (m_msaa_source_rt && (m_msaa_source_rt != config.rt || m_msaa_source_ds != config.ds ||
		config.rt->GetState() != GSTexture::State::Dirty || config.ds->GetState() != GSTexture::State::Dirty))
		FlushMSAA();
	if (!m_msaa_source_rt)
	{
		m_msaa_internal = true;
		ScopedGuard unlock([&]() { m_msaa_internal = false; });
		const GSVector2i size = config.rt->GetSize();
		if (!m_msaa_rt || m_msaa_rt->GetSize() != size)
		{
			OMSetRenderTargets(nullptr, nullptr, nullptr);
			PSUnbindConflictingSRVs(m_msaa_rt.get(), m_msaa_ds.get());
			m_msaa_rt.reset(); m_msaa_ds.reset();
			auto make = [&](bool depth) -> std::unique_ptr<GSTexture> {
				D3D11_TEXTURE2D_DESC td = {};
				td.Width = size.x; td.Height = size.y; td.MipLevels = td.ArraySize = 1;
				td.Format = depth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
				td.SampleDesc.Count = m_msaa_samples;
				td.Usage = D3D11_USAGE_DEFAULT;
				td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);
				wil::com_ptr_nothrow<ID3D11Texture2D> tex;
				if (FAILED(m_dev->CreateTexture2D(&td, nullptr, tex.put()))) return {};
				return std::make_unique<GSTexture11>(std::move(tex), td,
					depth ? GSTexture::DepthStencil : GSTexture::RenderTarget,
					depth ? GSTexture::Format::DepthStencil : GSTexture::Format::Color);
			};
			m_msaa_rt = make(false); m_msaa_ds = make(true);
			if (!m_msaa_rt || !m_msaa_ds)
			{
				std::fprintf(stderr, "[msaa] target allocation failed; reverting to original geometry path\n");
				m_msaa_samples = 1;
				return false;
			}
		}
		const GSVector4 rect(0, 0, size.x, size.y);
		StretchRect(config.rt, GSVector4(0, 0, 1, 1), m_msaa_rt.get(), rect, ShaderConvert::COPY, Nearest);
		// DEPTH_COPY defaults to a color output selector; explicitly write SV_Depth.
		StretchRect(config.ds, GSVector4(0, 0, 1, 1), m_msaa_ds.get(), rect,
			ShaderConvertSelector(ShaderConvert::DEPTH_COPY).SetDepthOutput(true), Nearest);
		m_msaa_source_rt = config.rt; m_msaa_source_ds = config.ds;
	}
	m_msaa_internal = true;
	GSHWDrawConfig draw = config;
	draw.rt = m_msaa_rt.get(); draw.ds = m_msaa_ds.get();
	m_ctx->RSSetState(m_msaa_rs.get());
	RenderHW(draw);
	m_ctx->RSSetState(m_rs.get());
	m_msaa_internal = false;
	if (++m_msaa_draws == 1 || (std::getenv("DOD_AA_PROFILE") && (m_msaa_draws % 10000) == 0))
		std::fprintf(stderr, "[msaa] samples=%u geometry_draws=%llu resolves=%llu size=%dx%d\n",
			m_msaa_samples, static_cast<unsigned long long>(m_msaa_draws),
			static_cast<unsigned long long>(m_msaa_epochs), config.rt->GetWidth(), config.rt->GetHeight());
	return true;
}
