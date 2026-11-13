// name: organic splatter
// original author: markg85
// based on: https://www.shadertoy.com/view/wfGBWm

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

// --- Procedural FBM for Organic Smudge & Filaments ---
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p, int octaves) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < octaves; i++) {
        v += noise(p) * amp;
        p *= 2.5; 
        amp *= 0.5;
    }
    return v;
}

void main()
{
    // 1. Setup Coordinates
    vec2 uv = TexCoords;
    float aspect = uResolution.x / uResolution.y;
    vec2 center = vec2(0.5, 0.5);
    vec2 p = uv - center;
    p.x *= aspect;

    // 2. Transition Progress
    // Map uProgress (0.0 - 1.0) to the threshold range (0.0 - 4.0)
    float threshold = uProgress * 4.0; 

    // 3. THE SPLATTER FIELD (Organic Shape)
    float smudgePattern = fbm(p * 1.8 + 10.0, 4); 
    float smudgeField = length(p) * (0.4 + smudgePattern * 3.2); 

    // 4. THE REVEAL MASK (Soft transition)
    float feather = 0.12;
    float revealMask = smoothstep(threshold - feather, threshold + feather, smudgeField);
    revealMask = 1.0 - revealMask; 

    // 5. THE RAZOR EDGE PROFILE
    float distFromWave = smudgeField - threshold;
    // High-power exponential curve for the lensing peak
    float waveProfile = exp(-pow(abs(distFromWave / 0.1), 2.0));
    // The razor-thin line mask
    float razorMask = pow(waveProfile, 100.0);

    // 6. GRAVITATIONAL WARPING (Refraction)
    float strength = 0.3 * (1.0 - uProgress * 0.5);
    vec2 dir = normalize(p + 0.0001); // Avoid div by zero
    vec2 swirlDir = vec2(-dir.y, dir.x); 
    vec2 warpVec = (dir + swirlDir * 0.2) * waveProfile * strength;

    // 7. SPECTRAL SAMPLING
    vec3 spectral = vec3(2.0, 1.0, 0.0); 
    vec3 sceneOld = vec3(
        texture(uTextureA, uv - warpVec * spectral.r).r,
        texture(uTextureA, uv - warpVec * spectral.g).g,
        texture(uTextureA, uv - warpVec * spectral.b).b
    );
    vec3 sceneNew = vec3(
        texture(uTextureB, uv - warpVec * spectral.r).r,
        texture(uTextureB, uv - warpVec * spectral.g).g,
        texture(uTextureB, uv - warpVec * spectral.b).b
    );
    
    vec3 finalColor = mix(sceneOld, sceneNew, revealMask);

    // 8. THE FILAMENTOUS HOTSPOTS
    // Instead of random noise, we use high-frequency FBM tied to the smudge coordinates.
    // This makes the hotspots "stretch" along the jagged edge.
    float highFreqNoise = fbm(p * 25.0 + uTime * 0.2, 2);
    
    // We mask the hotspots with a medium-thin version of the wave profile
    float hotspotMask = pow(waveProfile, 30.0); 
    
    // Luminance detection: spots appear where the light is already "hot"
    float lum = dot(finalColor, vec3(0.299, 0.587, 0.114));
    
    // Create sharp "beads" of light by thresholding the noise
    float filaments = pow(highFreqNoise, 4.0) * hotspotMask * 50.0 * lum;
    
    // 9. FINAL COMPOSITION
    // Add the razor spectral rim first
    vec3 rimGlow = mix(sceneOld, sceneNew, 0.5) * razorMask * 5.0;
    finalColor += rimGlow;
    
    // Add the "Filament" hotspots (intense white beads)
    finalColor += vec3(1.0) * filaments;

    // Subtle atmospheric glow boost
    finalColor *= (1.0 + waveProfile * 0.2);

    FragColor = vec4(finalColor, 1.0);
}
