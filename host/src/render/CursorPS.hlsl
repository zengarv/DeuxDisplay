Texture2D cursorTexture : register(t0);
SamplerState pointSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return cursorTexture.Sample(pointSampler, uv);
}
