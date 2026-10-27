// D3D11 cannot ResolveSubresource a depth-stencil target. Preserve the nearest
// covered depth for subsequent single-sample PS2 feedback/alpha passes (GS uses
// reversed depth: larger values are nearer). Stencil is not written by this pass.
Texture2DMS<float, MSAA_SAMPLES> Depth : register(t0);
float main(float4 pos : SV_Position) : SV_Depth
{
    int2 p = int2(pos.xy);
    float z = Depth.Load(p, 0);
    [unroll] for (int i = 1; i < MSAA_SAMPLES; ++i) z = max(z, Depth.Load(p, i));
    return z;
}
