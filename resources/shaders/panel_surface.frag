#version 440
// Re-bake (Qt 6.4+ compatible):
//   /usr/lib/qt6/bin/qsb --qsbversion 64 --glsl "100 es,120,150,300 es,330" --hlsl 50 --msl 12 -o resources/shaders/panel_surface.frag.qsb resources/shaders/panel_surface.frag

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 u_panelSize;
    float u_cornerRadius;
    float u_noiseStrength;
    float u_innerShadowStrength;
    float u_isRecessed;
};

// Rounded Box SDF
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Pseudo-random hash
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    vec2 pixelCoord = qt_TexCoord0 * u_panelSize;
    vec2 halfSize = u_panelSize * 0.5;
    vec2 centered = pixelCoord - halfSize;

    // Distance to edge (negative inside, 0 at boundary)
    float d = sdRoundedBox(centered, halfSize, u_cornerRadius);
    if (d > 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Normalized depth from outer boundary (0 at border edge, positive inwards in pixels)
    float depth = -d;

    // --- 1. Inner Shadow (Corner-aware rounded SDF) ---
    // Top-dominant ambient occluding light: top shadow reaches deeper than bottom
    float shadowRadius = u_isRecessed > 0.5 ? 16.0 : 12.0;
    float normDist = clamp(depth / shadowRadius, 0.0, 1.0);
    // Smooth falloff
    float innerShadow = (1.0 - smoothstep(0.0, 1.0, normDist)) * u_innerShadowStrength;

    // Subtle directional bias: top edge casts slightly stronger shadow than bottom edge
    float yRatio = clamp(pixelCoord.y / max(u_panelSize.y, 1.0), 0.0, 1.0);
    float topBias = mix(1.25, 0.75, yRatio);
    innerShadow *= topBias;

    // --- 2. Anisotropic & Grain Micro-Noise (Matte Anodized / Brushed Finish) ---
    // Stretched horizontally (x * 0.4, y * 1.5) to match brushed grain direction
    vec2 noiseCoord1 = vec2(floor(pixelCoord.x * 0.5), floor(pixelCoord.y * 1.5));
    vec2 noiseCoord2 = vec2(floor(pixelCoord.x * 1.0), floor(pixelCoord.y * 1.0));
    float n1 = hash(noiseCoord1) - 0.5;
    float n2 = hash(noiseCoord2) - 0.5;
    float grain = (n1 * 0.65 + n2 * 0.35) * u_noiseStrength;

    // Combine inner shadow (darkening) and grain (bipolar delta)
    // Darkening from inner shadow
    vec3 col = vec3(0.0);
    float alpha = innerShadow;

    // Add grain: positive adds faint silver highlight, negative darkens
    if (grain > 0.0) {
        col += vec3(grain * 1.4);
        alpha = max(alpha, grain * 1.2);
    } else {
        alpha = clamp(alpha - grain * 1.1, 0.0, 1.0);
    }

    fragColor = vec4(col, alpha) * qt_Opacity;
}
