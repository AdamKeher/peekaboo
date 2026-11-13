// name: explosion zoom
// original author: markg85
// based on: https://www.shadertoy.com/view/wX3BR4

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

const float DYNAMIC_OFFSET_Y = 0.46;
const float EXPLOSION_LIGHT_INTENSITY   = 0.16;
const float EXPLOSION_LIGHT_2_INTENSITY = 0.14;
const float EXPLOSION_LIGHT_SHIFT       = 8.0;
const float EXPLOSION_LIGHT_2_SHIFT     = 8.0;
const float EXPLOSION_LIGHT_Y_OFFSET    = 0.4;
const float EXPLOSION_LIGHT_2_Y_OFFSET  = 0.4;

// Timings mapped to 0.0-1.0 range
const float RIPPLE_START   = 0.2;            
const float XFADE_DELAY    = 0.05;           
const float XFADE_START    = RIPPLE_START + XFADE_DELAY;
const float ZOOM_STRENGTH_MAX = 0.35;
const float ZOOM_SAMPLES      = 20.0;
const float SQUEEZE_POWER = 6.0;

float easeOutExpo(float t) { return (t >= 1.0) ? 1.0 : 1.0 - pow(2.0, -10.0 * t); }
float easeOutCubic(float t){ float u = 1.0 - t; return 1.0 - (u*u*u); }

float random(in vec3 scale, in float seed) {
    return fract(sin(dot(vec3(TexCoords, uTime) + seed, scale)) * 43758.5453 + seed);
}

void main() {
    float p = uProgress;
    vec2 uv = TexCoords;

    // --------- PRE-EXPLOSION DISTORTION ----------
    float timeSq = p * p;
    float timeCu = p * timeSq;
    float k = pow(max(1.0 - timeSq * 0.5, 0.0), 8.0); 
    float yBottom = 1.0 - uv.y;
    float yBottomDist = yBottom + yBottom * yBottom * (k - 1.0);
    float yDist = 1.0 - yBottomDist;

    vec2 distortedUv = vec2(
        uv.x + (uv.x - 0.5) * pow(1.0 - uv.y, SQUEEZE_POWER) * timeCu * 2.0,
        yDist
    );
    distortedUv = mix(uv, distortedUv, smoothstep(0.3, 0.0, p));

    // --------- EXPLOSION PULL/SHIFT + LIGHTS ----------
    vec2 explosionShift = vec2(0.0);
    float explosionLight = 0.0;
    float explosionLight2 = 0.0;

    if (p >= RIPPLE_START) {
        float adjustedTime = (p - RIPPLE_START) * 2.0;
        vec2 nUv = uv - 0.5;
        nUv.x *= uResolution.x / uResolution.y;
        nUv.x -= 0.1;
        vec2 uvExplode = vec2(nUv.x + 0.1, nUv.y + DYNAMIC_OFFSET_Y);

        explosionLight = (adjustedTime * EXPLOSION_LIGHT_INTENSITY) / max(length(uvExplode), 1e-5);
        explosionLight = smoothstep(0.09, 0.05, explosionLight) *
                         smoothstep(0.04, 0.07, explosionLight) *
                         (nUv.y + 0.05);

        explosionShift += vec2(
            -EXPLOSION_LIGHT_SHIFT * explosionLight * nUv.x,
            -4.0 * explosionLight * (nUv.y - EXPLOSION_LIGHT_Y_OFFSET)
        ) * 0.1;

        float el2 = ((adjustedTime - 0.085) * EXPLOSION_LIGHT_2_INTENSITY) / max(length(uvExplode), 1e-5);
        el2 = smoothstep(0.09, 0.05, el2) *
              smoothstep(0.04, 0.07, el2) *
              (nUv.y + 0.05);
        explosionLight2 = el2;

        explosionShift += vec2(
            -EXPLOSION_LIGHT_2_SHIFT * el2 * nUv.x,
            -4.0 * el2 * (nUv.y - EXPLOSION_LIGHT_2_Y_OFFSET)
        ) * -0.02;
    }

    vec2 uvBase = distortedUv + explosionShift;

    // --------- TRANSITION CONTROLS ----------
    bool rippleActive = (p >= RIPPLE_START);
    bool xfadeActive  = (p >= XFADE_START);

    float xfadeProgress = xfadeActive ? clamp((p - XFADE_START) / (1.0 - XFADE_START), 0.0, 1.0) : 0.0;
    float dissolve = easeOutExpo(xfadeProgress);

    float zoomStrength = 0.0;
    if (rippleActive) {
        float zoomProgress = clamp((p - RIPPLE_START) / (1.0 - RIPPLE_START), 0.0, 1.0);
        zoomStrength = ZOOM_STRENGTH_MAX * (1.0 - easeOutCubic(zoomProgress));
    }

    vec2 center = vec2(0.5, 0.5 - DYNAMIC_OFFSET_Y); 
    vec2 toCenter = center - uvBase;

    vec3 colorA = texture(uTextureA, uvBase).rgb;
    vec3 colorB;

    if (rippleActive) {
        vec3 acc = vec3(0.0);
        float total = 0.0;
        float jitter = random(vec3(12.9898, 78.233, 151.7182), 0.0) * 0.5;

        for (float k2 = 0.0; k2 <= ZOOM_SAMPLES; k2 += 1.0) {
            float percent = (k2 + jitter) / ZOOM_SAMPLES;
            float weight  = (percent - percent * percent); 
            vec2  uvZoom  = uvBase + toCenter * percent * zoomStrength;
            acc += texture(uTextureB, uvZoom).rgb * weight;
            total += weight;
        }
        colorB = acc / max(total, 1e-5);
    } else {
        colorB = texture(uTextureB, uvBase).rgb;
    }

    vec3 color = mix(colorA, colorB, dissolve);
    float glowGate = smoothstep(0.2, 0.25, p);
    float glow = (explosionLight + 0.6 * explosionLight2) * glowGate;
    color += glow * 1.2;

    FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
