// name: bitplane slide
// original author: adam

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;

void main() {
    // Parallax slide
    float p = smoothstep(0.0, 1.0, uProgress);
    
    vec4 res = vec4(0.0);
    
    // Slide A out to the left at different speeds
    float offA1 = p * 1.0;
    float offA2 = p * 1.5;
    float offA3 = p * 2.0;
    
    // Slide B in from the right
    float offB1 = (1.0 - p) * 1.0;
    float offB2 = (1.0 - p) * 1.5;
    float offB3 = (1.0 - p) * 2.0;

    vec4 texA1 = texture(uTextureA, TexCoords + vec2(offA1, 0.0));
    vec4 texA2 = texture(uTextureA, TexCoords + vec2(offA2, 0.0));
    
    vec4 texB1 = texture(uTextureB, TexCoords - vec2(offB1, 0.0));
    vec4 texB2 = texture(uTextureB, TexCoords - vec2(offB2, 0.0));

    if (uProgress < 0.5) {
        FragColor = mix(texA1, texA2, 0.5);
    } else {
        FragColor = mix(texB1, texB2, 0.5);
    }
}
