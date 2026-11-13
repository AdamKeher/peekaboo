// name: zoom blur
// original author: aferriss
// based on: https://www.shadertoy.com/view/Ml3XR2

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

const float strength_val = 0.3;
const float PI = 3.141592653589793;

float Linear_ease(in float begin, in float change, float time) {
    return change * time + begin;
}

float Exponential_easeInOut(in float begin, in float change, float time) {
    if (time == 0.0) return begin;
    if (time == 1.0) return begin + change;
    time = time * 2.0;
    if (time < 1.0) return change / 2.0 * pow(2.0, 10.0 * (time - 1.0)) + begin;
    return change / 2.0 * (-pow(2.0, -10.0 * (time - 1.0)) + 2.0) + begin;
}

float Sinusoidal_easeInOut(in float begin, in float change, float time) {
    return -change / 2.0 * (cos(PI * time) - 1.0) + begin;
}

float random(in vec3 scale, in float seed) {
    return fract(sin(dot(vec3(TexCoords * uResolution, uTime) + seed, scale)) * 43758.5453 + seed);
}

vec3 crossFade(in vec2 uv, in float dissolve) {
    return mix(texture(uTextureA, uv).rgb, texture(uTextureB, uv).rgb, dissolve);
}

void main() {
    vec2 uv = TexCoords;
    float progress = uProgress;
    
    // Linear interpolate center
    vec2 center = vec2(Linear_ease(0.5, 0.0, progress), 0.5);
    float dissolve = Exponential_easeInOut(0.0, 1.0, progress);

    // Mirrored sinusoidal loop. 0->strength then strength->0
    float s = Sinusoidal_easeInOut(0.0, strength_val, progress);

    vec3 color = vec3(0.0);
    float total = 0.0;
    vec2 toCenter = center - uv;

    /* randomize the lookup values to hide the fixed number of samples */
    float offset = random(vec3(12.9898, 78.233, 151.7182), 0.0) * 0.5;

    for (float t = 0.0; t <= 20.0; t++) {
        float percent = (t + offset) / 20.0;
        float weight = 1.0 * (percent - percent * percent);
        color += crossFade(uv + toCenter * percent * s, dissolve) * weight;
        total += weight;
    }

    FragColor = vec4(color / total, 1.0);
}
