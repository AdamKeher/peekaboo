// name: interference
// original author: unknown
// based on: unknown

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

float rand(vec2 co){
    return fract(sin(dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453);
}

void main() {
    // 1. Pixelate the coordinate for the random check
    float resolution = 8.0;
    vec2 lowresxy = floor(TexCoords * uResolution / resolution);
    
    // 2. Drive the threshold with uProgress (0.0 to 1.0)
    float threshold = rand(lowresxy);
    
    if (uProgress > threshold) {
        // We are transitioning to B. 
        // Add a "flash" of the original interference pattern right as the block flips.
        float edge = uProgress - threshold;
        if (edge < 0.15) {
            // The specific interference look from your snippet
            vec3 interference = vec3(TexCoords, 0.5 + 0.5 * sin(5.0 * TexCoords.x * uResolution.x + uTime * 15.0));
            FragColor = vec4(interference, 1.0);
        } else {
            FragColor = texture(uTextureB, TexCoords);
        }
    } else {
        // Still showing A
        FragColor = texture(uTextureA, TexCoords);
    }
}
