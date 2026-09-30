#version 450

// Converts PyroWave's three planes (full-resolution Y, Cb/Cr at full or half
// resolution with chroma sited at the centre of each 2x2 quad) to RGB,
// letterboxed and pre-rotated for the swapchain's surface transform.

layout(set = 0, binding = 0) uniform sampler2D planeY;
layout(set = 0, binding = 1) uniform sampler2D planeCb;
layout(set = 0, binding = 2) uniform sampler2D planeCr;

layout(push_constant) uniform Params {
    // RGB = rows * (YCbCr - offset); range scaling is folded into the rows
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 offset;
    // xy: target extent in pixels, zw: its reciprocal
    vec4 target;
    // Video rectangle in upright normalized coordinates: xy origin, zw 1/size
    vec4 videoRect;
    // Surface transform: 0 identity, 1 rotate 90, 2 rotate 180, 3 rotate 270
    int rotation;
} params;

layout(location = 0) out vec4 outColor;

void main()
{
    // Target position in clip space, then undo the display's rotation
    vec2 n = gl_FragCoord.xy * params.target.zw * 2.0 - 1.0;
    vec2 v = n;
    if (params.rotation == 1) {
        v = vec2(n.y, -n.x);
    }
    else if (params.rotation == 2) {
        v = -n;
    }
    else if (params.rotation == 3) {
        v = vec2(-n.y, n.x);
    }

    vec2 uv = ((v * 0.5 + 0.5) - params.videoRect.xy) * params.videoRect.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 ycbcr = vec3(texture(planeY, uv).r, texture(planeCb, uv).r, texture(planeCr, uv).r) - params.offset.xyz;
    vec3 rgb = vec3(dot(params.row0.xyz, ycbcr), dot(params.row1.xyz, ycbcr), dot(params.row2.xyz, ycbcr));
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
