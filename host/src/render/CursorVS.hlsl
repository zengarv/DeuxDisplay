// Draws the pointer as a quad from four corners in NDC, so it can be placed rotated when the
// display is rotated. Strip order: (u,v) = (0,0) (1,0) (0,1) (1,1).
cbuffer Params : register(b0)
{
    float4 corners01; // xy = corner 0, zw = corner 1
    float4 corners23; // xy = corner 2, zw = corner 3
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 corner = id == 0 ? corners01.xy : id == 1 ? corners01.zw : id == 2 ? corners23.xy : corners23.zw;
    VSOut o;
    o.pos = float4(corner, 0, 1);
    o.uv = float2(id & 1, id >> 1);
    return o;
}
