// GPU passes for the SAR renderer (Windows / D3D9).
//
// Regenerate RendererShaders.hpp with tools/gen_renderer_shaders.bat after
// editing. All passes draw a pretransformed quad whose TEXCOORD0 is the target
// pixel coordinate + 0.5, so `tc` is exactly the centre of the output texel.
//
// Planes are written into A8R8G8B8 targets with four 8-bit samples per texel.
// A8R8G8B8 is stored as B,G,R,A in memory, so sample n of a texel goes to
// channel b,g,r,a for n = 0,1,2,3; the read-back rows are then ordinary
// 8-bit plane rows.
//
// c0 = (1/srcW, 1/srcH, weight, 0)
// c1 = Cb coefficients (xyz) + offset (w)
// c2 = Cr coefficients (xyz) + offset (w)

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);

// BT.709, limited range
static const float3 kY = float3(0.2126, 0.7152, 0.0722) * (219.0 / 255.0);
static const float kYOff = 16.0 / 255.0;

float luma(float2 uv) {
	return dot(tex2D(s0, uv).rgb, kY) + kYOff;
}

// Y plane: target (W/4) x H, point sampling
float4 ps_y(float2 tc : TEXCOORD0) : COLOR0 {
	float u = (4.0 * tc.x - 1.5) * c0.x;
	float v = tc.y * c0.y;
	float y0 = luma(float2(u, v));
	float y1 = luma(float2(u + c0.x, v));
	float y2 = luma(float2(u + 2.0 * c0.x, v));
	float y3 = luma(float2(u + 3.0 * c0.x, v));
	return float4(y2, y1, y0, y3);
}

// One chroma plane (U with c1, or V passed in c1): target (W/8) x (H/2).
// Bilinear sampling at the shared corner of each 2x2 block averages it.
float4 ps_c(float2 tc : TEXCOORD0) : COLOR0 {
	float u = (8.0 * tc.x - 3.0) * c0.x;
	float v = 2.0 * tc.y * c0.y;
	float a = dot(tex2D(s0, float2(u, v)).rgb, c1.xyz) + c1.w;
	float b = dot(tex2D(s0, float2(u + 2.0 * c0.x, v)).rgb, c1.xyz) + c1.w;
	float c = dot(tex2D(s0, float2(u + 4.0 * c0.x, v)).rgb, c1.xyz) + c1.w;
	float d = dot(tex2D(s0, float2(u + 6.0 * c0.x, v)).rgb, c1.xyz) + c1.w;
	return float4(c, b, a, d);
}

// Interleaved UV plane (NV12): target (W/4) x (H/2), bytes U0 V0 U1 V1
float4 ps_uv(float2 tc : TEXCOORD0) : COLOR0 {
	float u = (4.0 * tc.x - 1.0) * c0.x;
	float v = 2.0 * tc.y * c0.y;
	float3 p0 = tex2D(s0, float2(u, v)).rgb;
	float3 p1 = tex2D(s0, float2(u + 2.0 * c0.x, v)).rgb;
	float u0 = dot(p0, c1.xyz) + c1.w;
	float v0 = dot(p0, c2.xyz) + c2.w;
	float u1 = dot(p1, c1.xyz) + c1.w;
	float v1 = dot(p1, c2.xyz) + c2.w;
	return float4(u1, v0, u0, v1);
}

// Frame blending: s0 = accumulator, s1 = new subframe, weight in c0.z
float4 ps_acc_first(float2 tc : TEXCOORD0) : COLOR0 {
	return tex2D(s1, tc * c0.xy) * c0.z;
}

float4 ps_acc(float2 tc : TEXCOORD0) : COLOR0 {
	return tex2D(s0, tc * c0.xy) + tex2D(s1, tc * c0.xy) * c0.z;
}

// Plain copy (accumulator -> 8-bit frame)
float4 ps_copy(float2 tc : TEXCOORD0) : COLOR0 {
	return tex2D(s0, tc * c0.xy);
}
