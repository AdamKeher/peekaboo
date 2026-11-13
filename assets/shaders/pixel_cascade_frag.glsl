// name: pixel cascade
// original author: toji
// based on: https://www.shadertoy.com/view/Xd3yzs

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

// Pixel Cascade Transition 
// Adjusted for a more pronounced "falling" effect

const float kPixelSize = 16.0;         // Larger pixels for better visibility
const float kVerticalSpread = 1.0;     // How much the top leads the bottom
const float kNoiseSpread = 0.4;        // Randomness in pixel timing
const float kSharpness = 4.0;          // How quickly each pixel flips

float rand(vec2 co) {
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec2 uv = TexCoords;
    
    // Pixelate the coordinate for noise lookup
    vec2 noise_cell = floor(uv * uResolution / kPixelSize);
    vec2 noise_uv = noise_cell * kPixelSize / uResolution;

    float noise = rand(noise_uv) * kNoiseSpread;
    
    // We want a sweep that goes from top to bottom.
    // The total range of the sweep needs to include the height of the screen (1.0)
    // plus the vertical spread and noise so it starts/ends off-screen.
    float total_range = 1.0 + kVerticalSpread + kNoiseSpread;
    float sweep = uProgress * total_range;
    
    // Calculate the transition value for this pixel.
    // At uProgress = 0, 'val' will be negative for all y.
    // At uProgress = 1, 'val' will be positive for all y.
    float val = sweep - (1.0 - uv.y) * kVerticalSpread - noise;
    
    float transition = clamp(val * kSharpness, 0.0, 1.0);

    vec4 imgA = texture(uTextureA, uv);
    vec4 imgB = texture(uTextureB, uv);
    
    FragColor = mix(imgA, imgB, transition);
}
