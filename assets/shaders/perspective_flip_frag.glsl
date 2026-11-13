// name: perspective flip
// original author: aiekick
// based on: https://www.shadertoy.com/view/MlXGzf

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

// Perspective Flip Transition
// Based on: http://transitions.glsl.io/transition/b86b90161503a0023231

const float reflection = 0.4;
const float perspective = 0.2;
const float depth = 3.0;
const vec4 black = vec4(0.0, 0.0, 0.0, 1.0);
const vec2 boundMin = vec2(0.0, 0.0);
const vec2 boundMax = vec2(1.0, 1.0);

bool inBounds(vec2 p) {
    return all(greaterThan(p, boundMin)) && all(lessThan(p, boundMax));
}

vec2 project(vec2 p) {
    return p * vec2(1.0, -1.2) + vec2(0.0, -0.02);
}

vec4 bgColor(vec2 p, vec2 pfr, vec2 pto) {
    vec4 c = black;
    pfr = project(pfr);
    if (inBounds(pfr)) {
        c += mix(black, texture(uTextureA, pfr), reflection * mix(1.0, 0.0, pfr.y));
    }
    pto = project(pto);
    if (inBounds(pto)) {
        c += mix(black, texture(uTextureB, pto), reflection * mix(1.0, 0.0, pto.y));
    }
    return c;
}

void main() {
    float progress = uProgress;
    vec2 p = TexCoords;

    vec2 pfr, pto;

    float size = mix(1.0, depth, progress);
    float persp = perspective * progress;
    pfr = (p + vec2(-0.0, -0.5)) * vec2(size / (1.0 - perspective * progress), size / (1.0 - size * persp * p.x)) + vec2(0.0, 0.5);

    size = mix(1.0, depth, 1.0 - progress);
    persp = perspective * (1.0 - progress);
    pto = (p + vec2(-1.0, -0.5)) * vec2(size / (1.0 - perspective * (1.0 - progress)), size / (1.0 - size * persp * (0.5 - p.x))) + vec2(1.0, 0.5);

    bool fromOver = progress < 0.5;

    if (fromOver) {
        if (inBounds(pfr)) {
            FragColor = texture(uTextureA, pfr);
        } else if (inBounds(pto)) {
            FragColor = texture(uTextureB, pto);
        } else {
            FragColor = bgColor(p, pfr, pto);
        }
    } else {
        if (inBounds(pto)) {
            FragColor = texture(uTextureB, pto);
        } else if (inBounds(pfr)) {
            FragColor = texture(uTextureA, pfr);
        } else {
            FragColor = bgColor(p, pfr, pto);
        }
    }
}
