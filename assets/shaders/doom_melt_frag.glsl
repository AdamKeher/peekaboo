// name: doom melt
// original author: Andrew900460
// based on: https://www.shadertoy.com/view/7dSfzK

#version 330 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D uTextureA;
uniform sampler2D uTextureB;
uniform float uProgress;
uniform float uTime;
uniform vec2 uResolution;

// Doom Melt Transition (as close to the original as possible)
// based off of the "rndtable" that doom uses for random number generation
const int melt_table[160] = int[](
    -8, -8, -8, -9, -9, -8, -7, -8, -7, -6,
    -5, -5, -6, -5, -6, -6, -5, -4, -3, -3,
    -4, -4, -3, -2, -1, 0, -1, 0, 0, 0,
    -1, 0, 0, 0, -1, -1, -1, 0, 0, 0,
    0, 0, 0, 0, -1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, -1,
    0, 0, 0, -1, 0, 0, -1, -1, 0, 0,
    -1, -2, -1, -2, -3, -4, -4, -5, -6, -5,
    -5, -6, -7, -7, -6, -6, -5, -5, -6, -6,
    -5, -5, -5, -6, -7, -6, -6, -6, -5, -4,
    -4, -5, -4, -5, -5, -6, -5, -4, -3, -3,
    -2, -2, -1, 0, 0, 0, 0, -1, 0, 0,
    -1, 0, 0, -1, -2, -2, -3, -2, -1, -2,
    -1, -2, -2, -2, -1, 0, 0, 0, 0, -1,
    -1, -2, -1, -2, -1, -2, -1, 0, 0, 0,
    0, 0, 0, 0, 0, -1, -1, -2, -3, -4
);

float getMeltTableValue(float x) {
    float screenWidth = 320.0; // original doom resolution width was 320
    int index = int(floor(x * screenWidth)) % 160;
    int melt_height = melt_table[index];
    
    // Process height to float [0,1]
    float result_height = float(melt_height + 10) / 10.0;
    return result_height;
}

void main() {
    vec2 uv = TexCoords;
    
    // Drive the melt with uProgress
    // We multiply uProgress to ensure everything eventually melts off screen
    float push = (1.0 - getMeltTableValue(uv.x)) * 0.2 + uProgress * 1.5;
    
    vec2 uvA = uv;
    uvA.y -= push; // Melt downwards (subtract because 0 is bottom in GL)
    
    if (uvA.y >= 0.0 && uvA.y <= 1.0) {
        FragColor = texture(uTextureA, uvA);
    } else {
        FragColor = texture(uTextureB, uv);
    }
}
