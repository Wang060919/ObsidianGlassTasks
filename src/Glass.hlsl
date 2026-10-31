// Material reference: shayann07/liquidglass (Apache-2.0).
// Copyright 2026 Shayan. Modified 2026-11-01: Windows HLSL port and continuous lens.
// See resources/licenses/liquidglass-NOTICE.txt. The rest of this Windows shader
// is a small desktop-specific implementation, not Apple's private material.
cbuffer Material : register(b0) {
    float2 viewport; float2 origin;
    float2 desktopOrigin; float2 desktopSize;
    float2 pointer; float2 unused0;
    float dpi; float press; float unusedMaterial; float refraction;
    float rotation; float hasBackdrop; float2 unused1;
};
Texture2D backdrop : register(t0);
Texture2D previous : register(t1);
Texture2D horizontalBlur : register(t2);
Texture2D reducedScene : register(t3);
SamplerState linearClamp : register(s0);
RWStructuredBuffer<uint> changed : register(u0);

float4 Vertex(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 desktopUV(float2 p) {
    float2 uv = (origin + p - desktopOrigin) / desktopSize;
    if (rotation == 1) return float2(uv.y, 1 - uv.x);
    if (rotation == 2) return 1 - uv;
    if (rotation == 3) return float2(1 - uv.y, uv.x);
    return uv;
}
float3 sampleScene(float2 p) { return backdrop.SampleLevel(linearClamp, desktopUV(p), 0).rgb; }
// Dense prefiltering removes pixel-scale noise before reducing resolution. Stretching a
// few bilinear Gaussian taps over the full-resolution image leaves visible copies.
float4 Downsample(float4 position : SV_Position) : SV_Target {
    float2 p = position.xy * (4 * dpi) - 48 * dpi;
    float3 color = 0;
    [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++)
        color += sampleScene(p + (float2(x, y) - 1.5) * dpi);
    return float4(color / 16, 1);
}
// A sigma=.5 Gaussian at quarter resolution, plus the prefilter above, gives
// roughly 2-3 CSS pixels of diffusion. Keep medium detail visible through the lens.
static const float gaussianCenter = .786571;
static const float gaussianOffset = 1.002473;
static const float gaussianWeight = .1067145;
float4 BlurHorizontal(float4 position : SV_Position) : SV_Target {
    uint w, h; reducedScene.GetDimensions(w, h);
    float2 size = float2(w, h), uv = position.xy / size;
    float2 tap = float2(gaussianOffset / w, 0);
    float3 color = reducedScene.SampleLevel(linearClamp, uv, 0).rgb * gaussianCenter;
    color += (reducedScene.SampleLevel(linearClamp, uv + tap, 0).rgb
            + reducedScene.SampleLevel(linearClamp, uv - tap, 0).rgb) * gaussianWeight;
    return float4(color, 1);
}
float3 sampleHorizontal(float2 p) {
    uint w, h; horizontalBlur.GetDimensions(w, h);
    return horizontalBlur.SampleLevel(linearClamp, (p + 48 * dpi) / (float2(w, h) * 4 * dpi), 0).rgb;
}
float3 softScene(float2 p) {
    float2 tap = float2(0, gaussianOffset * 4 * dpi);
    return sampleHorizontal(p) * gaussianCenter
         + (sampleHorizontal(p + tap) + sampleHorizontal(p - tap)) * gaussianWeight;
}
float roundDistance(float2 p, float2 halfSize, float r) {
    float2 q = abs(p) - halfSize + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}
float4 Glass(float4 position : SV_Position) : SV_Target {
    float2 pixel = position.xy, p = pixel - viewport * .5;
    float2 halfSize = viewport * .5 - 12 * dpi;
    float radius = min(28 * dpi, min(halfSize.x, halfSize.y));
    float d = roundDistance(p, halfSize, radius);
    float alpha = saturate(.5 - d);
    if (alpha <= 0) {
        float shadowDistance = max(roundDistance(p - float2(0, 3 * dpi), halfSize, radius), 0);
        float shadow = exp(-shadowDistance / (3.6 * dpi)) * .16 * saturate(1 - max(d, 0) / (12 * dpi));
        return float4(float3(.02, .035, .055) * shadow, shadow);
    }
    float2 q = abs(p) - halfSize + radius;
    float2 normal = max(q, 0);
    normal = length(normal) > .001 ? normalize(normal) : (q.x > q.y ? float2(1, 0) : float2(0, 1));
    normal *= sign(p);
    // Use one optical profile around the entire perimeter. Its displacement
    // reaches zero before the rounded-distance normal becomes ambiguous inside.
    float depth = max(-d, 0), band = min(32 * dpi, radius), u = depth / band;
    float shoulder = saturate(1 - u);
    float3 surfaceNormal = normalize(float3(normal * shoulder * shoulder * 2.0, 1));
    float3 ray = refract(float3(0, 0, -1), surfaceNormal, 1.0 / 1.45);
    float offset = -dot(ray.xy, normal) / max(-ray.z, .1) * 20 * dpi * refraction * (1 + press * .14);
    float2 source = pixel - normal * offset;
    float polish = exp(-depth / (2.3 * dpi));
    float3 color = softScene(source);
    float split = offset * .035 * polish;
    float3 spectral = float3(sampleScene(source + normal * split).r, sampleScene(source).g, sampleScene(source - normal * split).b);
    color = lerp(color, spectral, polish * .82);
    if (hasBackdrop < .5) color = float3(.21, .25, .31);
    // Dock-style light absorption keeps the dark palette without a grey veil.
    float transmission = .85 + .06 * shoulder * shoulder;
    color = color * transmission + float3(.004, .006, .010);
    float topLight = pow(saturate(dot(normal, normalize(float2(-.50, -.86)))), 3);
    float bottomLight = pow(saturate(dot(normal, normalize(float2(.65, .76)))), 5);
    float bevel = exp(-depth / (.85 * dpi));
    float innerBevel = exp(-pow((depth - 3.2 * dpi) / (1.6 * dpi), 2));
    float proximity = exp(-length(pixel - pointer) / (95 * dpi));
    float highlight = bevel * (.08 + .62 * topLight + .32 * bottomLight + .18 * proximity)
                    + innerBevel * (.018 + .08 * bottomLight)
                    + exp(-depth / (5.5 * dpi)) * .065 * topLight;
    color += highlight * (1 - color) * (1 + press * .10);
    color *= 1 - .15 * exp(-pow((depth - 1.8 * dpi) / (1.1 * dpi), 2)) * (1 - topLight);
    return float4(saturate(color) * alpha, alpha);
}
groupshared uint groupChanged;
[numthreads(16, 16, 1)]
void Compare(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID) {
    if (local.x == 0 && local.y == 0) groupChanged = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < (uint)ceil(viewport.x + 96 * dpi) && id.y < (uint)ceil(viewport.y + 96 * dpi)) {
        uint w, h; backdrop.GetDimensions(w, h);
        int2 coord = clamp(int2(desktopUV(float2(id.xy)+.5-48*dpi) * float2(w, h)), int2(0, 0), int2(w-1, h-1));
        float3 difference = abs(backdrop.Load(int3(coord, 0)).rgb - previous.Load(int3(coord, 0)).rgb);
        if (max(difference.r, max(difference.g, difference.b)) > .0038) InterlockedOr(groupChanged, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (local.x == 0 && local.y == 0 && groupChanged != 0) InterlockedOr(changed[0], 1);
}
