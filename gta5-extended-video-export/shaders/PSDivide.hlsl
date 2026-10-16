// EVE1G motion blur: the finished exposure - the collected light divided by how long each pixel was exposed,
// back to the display image (sRGB) when it was collected in linear light.
struct VSOutput
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

Texture2D<float4> MyTexture : register(t0);

cbuffer constantBuffer : register(b0) {
	float4 floats; // x: samples added (fallback divisor)   y: 1 = encode linear light back to sRGB
}

float3 ToSrgb(float3 c)
{
	c = saturate(c);
	return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

float4 main(VSOutput input) : SV_TARGET
{
	const float4 acc = MyTexture.Load(int3(input.position.xy, 0));
	const float3 rgb = acc.a > 1e-6 ? acc.rgb / acc.a : acc.rgb / max(floats.x, 1.0);
	return float4(floats.y > 0.5 ? ToSrgb(rgb) : rgb, 1.0);
}
