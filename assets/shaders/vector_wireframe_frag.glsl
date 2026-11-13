// name: vector wireframe
// original author: adam

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;

void main() {
    vec4 colA = texture(uTextureA, TexCoords);
    vec4 colB = texture(uTextureB, TexCoords);
    
    // Simple edge detection
    vec2 size = vec2(1.0) / textureSize(uTextureA, 0);
    float edgeA = 0.0;
    for(int x=-1; x<=1; x++) {
        for(int y=-1; y<=1; y++) {
            edgeA += distance(colA, texture(uTextureA, TexCoords + vec2(x,y)*size));
        }
    }
    
    float edgeB = 0.0;
    for(int x=-1; x<=1; x++) {
        for(int y=-1; y<=1; y++) {
            edgeB += distance(colB, texture(uTextureB, TexCoords + vec2(x,y)*size));
        }
    }

    vec3 vectorColor = vec3(0.0, 1.0, 0.8); // Neon cyan
    float glowA = smoothstep(0.1, 0.5, edgeA);
    float glowB = smoothstep(0.1, 0.5, edgeB);

    if (uProgress < 0.5) {
        float p = uProgress * 2.0;
        FragColor = mix(colA, vec4(vectorColor * glowA, 1.0), p);
    } else {
        float p = (uProgress - 0.5) * 2.0;
        FragColor = mix(vec4(vectorColor * glowB, 1.0), colB, p);
    }
}
