// name: c64 glitch
// original author: adam


#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }

void main() {
    vec2 blocks = vec2(40.0, 25.0);
    vec2 blockUv = floor(TexCoords * blocks) / blocks;
    float noise = hash(blockUv + floor(uProgress * 10.0));
    
    if (uProgress < 0.4) {
        FragColor = texture(uTextureA, TexCoords);
    } else if (uProgress > 0.6) {
        FragColor = texture(uTextureB, TexCoords);
    } else {
        // Glitch zone
        vec3 c64blue = vec3(0.2, 0.2, 0.8);
        vec3 c64light = vec3(0.4, 0.4, 1.0);
        FragColor = vec4(mix(c64blue, c64light, noise), 1.0);
        
        // Horizontal stripes
        if (hash(vec2(blockUv.y, floor(uProgress * 20.0))) > 0.7) {
            FragColor = vec4(1.0, 0.5, 1.0, 1.0); // Purple flash
        }
    }
}
