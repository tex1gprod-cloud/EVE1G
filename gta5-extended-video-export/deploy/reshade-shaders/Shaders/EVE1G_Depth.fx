// EVE1G Depth - the depth pass for EVE1G exports.
//
// Writes the scene's linear depth into a texture that EVE1G reads while it exports. It draws NOTHING on screen and
// leaves your image alone. EVE1G switches this technique on by itself while a depth export is running and switches
// it off again afterwards - there is no need to enable it in the ReShade overlay.
//
// The depth is linearised exactly like ReShade's own depth (ReShade.fxh GetLinearizedDepth), with the same global
// settings (RESHADE_DEPTH_INPUT_IS_REVERSED, ..._IS_UPSIDE_DOWN, ..._LINEARIZATION_FAR_PLANE), so it matches what
// your depth of field sees.

#ifndef RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
    #define RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN 0
#endif
#ifndef RESHADE_DEPTH_INPUT_IS_REVERSED
    #define RESHADE_DEPTH_INPUT_IS_REVERSED 1
#endif
#ifndef RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
    #define RESHADE_DEPTH_INPUT_IS_LOGARITHMIC 0
#endif
#ifndef RESHADE_DEPTH_MULTIPLIER
    #define RESHADE_DEPTH_MULTIPLIER 1
#endif
#ifndef RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
    #define RESHADE_DEPTH_LINEARIZATION_FAR_PLANE 1000.0
#endif
#ifndef RESHADE_DEPTH_INPUT_X_SCALE
    #define RESHADE_DEPTH_INPUT_X_SCALE 1
#endif
#ifndef RESHADE_DEPTH_INPUT_Y_SCALE
    #define RESHADE_DEPTH_INPUT_Y_SCALE 1
#endif
#ifndef RESHADE_DEPTH_INPUT_X_OFFSET
    #define RESHADE_DEPTH_INPUT_X_OFFSET 0
#endif
#ifndef RESHADE_DEPTH_INPUT_Y_OFFSET
    #define RESHADE_DEPTH_INPUT_Y_OFFSET 0
#endif
#ifndef RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
    #define RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET 0
#endif
#ifndef RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
    #define RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET 0
#endif

texture EVE1G_DepthInput : DEPTH;
sampler EVE1G_DepthSampler { Texture = EVE1G_DepthInput; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

// Read by EVE1G: linear depth, 0 = camera, 1 = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE.
texture EVE1G_DepthTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };

void EVE1G_DepthVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float EVE1G_DepthPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
#if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
    texcoord.y = 1.0 - texcoord.y;
#endif
    texcoord.x /= RESHADE_DEPTH_INPUT_X_SCALE;
    texcoord.y /= RESHADE_DEPTH_INPUT_Y_SCALE;
#if RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
    texcoord.x -= RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET * BUFFER_RCP_WIDTH;
#else
    texcoord.x -= RESHADE_DEPTH_INPUT_X_OFFSET / 2.000000001;
#endif
#if RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
    texcoord.y += RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET * BUFFER_RCP_HEIGHT;
#else
    texcoord.y += RESHADE_DEPTH_INPUT_Y_OFFSET / 2.000000001;
#endif
    float depth = tex2Dlod(EVE1G_DepthSampler, float4(texcoord, 0, 0)).x * RESHADE_DEPTH_MULTIPLIER;
#if RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
    const float C = 0.01;
    depth = (exp(depth * log(C + 1.0)) - 1.0) / C;
#endif
#if RESHADE_DEPTH_INPUT_IS_REVERSED
    depth = 1 - depth;
#endif
    const float N = 1.0;
    depth /= RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - depth * (RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - N);
    return saturate(depth);
}

technique EVE1G_Depth <
    ui_label = "EVE1G Depth (used by EVE1G exports - draws nothing)";
    ui_tooltip = "EVE1G switches this on by itself during a depth export. It writes the scene depth for the depth pass and does not change the image. You can leave it off.";
>
{
    pass
    {
        VertexShader = EVE1G_DepthVS;
        PixelShader = EVE1G_DepthPS;
        RenderTarget = EVE1G_DepthTex;
    }
}
