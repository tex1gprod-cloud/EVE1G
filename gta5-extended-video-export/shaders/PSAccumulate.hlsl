// EVE1G motion blur: adds one game frame (one moment of the exposure) to the accumulation buffer.
//
// Works like a camera sensor collecting light while its shutter is open:
//  - light-correct: the frame is turned back into linear light before it is added, so bright lights streak and
//    dark edges blend like a real exposure (blending the display image directly darkens and dulls the blur)
//  - shutter: each moment counts by how open the shutter is then - a hard box, or soft edges like a real rotary
//    shutter whose blade takes a moment to sweep across the gate
//  - rolling shutter (optional): each row of the image is exposed a little later than the row above it, like a
//    CMOS sensor read out line by line - fast pans lean, spinning things bend
// Each pixel also adds its weight to alpha, and PSDivide divides by it.
struct VSOutput
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

Texture2D<float4> MyTexture : register(t0);

cbuffer params : register(b0)
{
	float4 p0; // x: this sample's time in the frame interval (0..1)   y: exposure (shutter angle / 360)
	           // z: rolling-shutter readout (share of the interval)  w: soft edges (share of the exposure faded)
	float4 p1; // x: 1 = decode sRGB to linear light   y: image height   z: half a sample step, in exposures
};

float3 ToLinear(float3 c)
{
	c = saturate(c);
	return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float4 main(VSOutput input) : SV_TARGET
{
	float4 c = MyTexture.Load(int3(input.position.xy, 0));
	const float row = saturate(input.position.y / max(p1.y, 1.0));
	// this row's exposure window ends at 1 for the bottom row (and for every row without rolling shutter)
	const float start = 1.0 - p0.y - p0.z + row * p0.z;
	const float u = (p0.x - start) / max(p0.y, 1e-4);
	const float eps = 1e-4;
	if (u < -eps || u > 1.0 + eps)
		return float4(0, 0, 0, 0);
	float w = 1.0;
	if (p0.w > 0.0)
		w = saturate((min(u, 1.0 - u) + p1.z) / p0.w);
	const float3 rgb = p1.x > 0.5 ? ToLinear(c.rgb) : c.rgb;
	return float4(rgb * w, w);
}
