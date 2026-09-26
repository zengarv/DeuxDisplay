// Draws the pointer as a quad; rect = (left, top, right, bottom) in NDC.
cbuffer Params : register(b0)
{
    float4 rect;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1); // triangle strip: (0,0) (1,0) (0,1) (1,1)
    VSOut o;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    o.uv = t;
    return o;
}
