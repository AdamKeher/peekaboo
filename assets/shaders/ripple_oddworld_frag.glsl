// name: oddworld ripple
// original author: tb
// based on: https://www.shadertoy.com/view/3ljfzV

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

/*
This attempts to replicate the transition seen in Oddworld - Abe's Oddysee.
Adapted for workspace transition using uProgress.
*/

float freq = 8.0;
float speed = 2.0;
float fade = 4.0;
float displacement = 0.2;

void main()
{
    vec2 R = uResolution.xy;
    vec2 fragCoord = TexCoords * R;
    
    // U: Centered and aspect-corrected coordinates
    vec2 U = ((2. * fragCoord.xy) - R) / min(R.x, R.y);
    // T: Normalized coordinates for sampling
    vec2 T = TexCoords;
    
    float D = length(U);

    // Map uProgress (0.0 to 1.0) to a virtual frame_time
    // Since Abe's Oddysee transition is a ripple that fades out, 
    // we use uProgress to drive the wave propagation.
    float frame_time = uProgress * 6.0; // Scaled to allow wave to reach edges
    float pixel_time = max(0.0, frame_time - D);

    // Ripple math
    float wave_height = (cos(pixel_time * freq) + 1.0) / 2.0;
    float wave_scale = (1.0 - min(1.0, pixel_time / fade));
    
    // Transition fraction: progress driven
    // We want a smooth crossover from A to B based on the wave
    float frac = smoothstep(0.0, 1.0, uProgress);
    
    // Add the "Oddworld" ripple modulation to the mix factor
    // This makes the transition feel like it's "pulsing" into existence
    float wave_mix = wave_height * wave_scale * (1.0 - abs(uProgress - 0.5) * 2.0);
    float final_frac = clamp(frac + wave_mix * 0.5, 0.0, 1.0);

    // UV Displacement (Refraction)
    // Avoid division by zero for D
    vec2 offset = vec2(0.0);
    if (D > 0.0001) {
        offset = (U / D) * -((sin(pixel_time * freq) / fade) * wave_scale) * displacement;
    }
    
    vec2 tc = T + offset;
    
    vec4 colA = texture(uTextureA, tc);
    vec4 colB = texture(uTextureB, tc);
    
    FragColor = mix(colA, colB, final_frac);
}
