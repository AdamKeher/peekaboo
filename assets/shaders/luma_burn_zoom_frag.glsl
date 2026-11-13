// name: luma burn
// original author: aiekick
// based on: https://www.shadertoy.com/view/ltV3RG

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

// Simple procedural noise for the luma wipe
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

void main()
{
    vec2 uv = TexCoords;
    
    // Drive the threshold with uProgress (0.0 to 1.0)
    // We adjust the range to ensure it fully wipes across
    float th = uProgress * 1.4 - 0.2;
    
    // Procedural noise combined with horizontal sweep
    float n = noise(uv * 10.0 + uTime * 0.1);
    float tex = (n + 2.0 * uv.x) / 3.0;
    
    // Derived masks from snippet
    float mask = smoothstep(th - 0.1, th, tex);
    float dist = smoothstep(th - 0.3, th + 0.05, tex);
    float colFactor = pow(smoothstep(th - 0.2, th + 0.15, tex), 3.0);
    
    // Workspace A (Source)
    vec4 colorA = texture(uTextureA, uv);
    
    // Workspace B (Destination) with the zoom/distortion from snippet
    vec2 zoomedUv = uv * (0.7 + pow(dist, 2.0) * 0.3);
    // Center the zoom
    zoomedUv += (1.0 - (0.7 + pow(dist, 2.0) * 0.3)) * 0.5;
    
    vec3 colorB = texture(uTextureB, zoomedUv).rgb;
    vec3 discolorB = colorB * vec3(0.8, 0.4, 0.2); // Burnt orange tint
    
    // Mix Workspace B with its discolored version
    vec3 mixedB = mix(discolorB, colorB, colFactor);
    
    // Final transition mix:
    // If mask is 0 (leading edge), show colorA.
    // As mask increases, blend into mixedB.
    vec3 finalColor = mix(colorA.rgb, mixedB, mask);
    
    FragColor = vec4(finalColor, 1.0);
}
