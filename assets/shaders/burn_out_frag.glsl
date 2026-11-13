// name: burning
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

// based on http://glslsandbox.com/e#35642.0

float Hash( vec2 p)
{
	vec3 p2 = vec3(p.xy,1.0);
    return fract(sin(dot(p2,vec3(37.1,61.7, 12.4)))*3758.5453123);
}

float noise(in vec2 p)
{
    vec2 i = floor(p);
	vec2 f = fract(p);
	f *= f * (3.0-2.0*f);

    return mix(mix(Hash(i + vec2(0.,0.)), Hash(i + vec2(1.,0.)),f.x),
		mix(Hash(i + vec2(0.,1.)), Hash(i + vec2(1.,1.)),f.x),
		f.y);
}

float fbm(vec2 p) 
{
	float v = 0.0;
	v += noise(p*1.)*.5;
	v += noise(p*2.)*.25;
	v += noise(p*4.)*.125;
	return v;
}

void main()
{
	vec2 uv = TexCoords;
    // Standard centered UV for noise calculation
    vec2 p = (uv - 0.5);
    p.x *= uResolution.x / uResolution.y;
	
	vec3 src = texture(uTextureA, uv).rgb;
	vec3 tgt = texture(uTextureB, uv).rgb;
	
	vec3 col = src;
	
	// Map uProgress to sweep the burn across the screen
    // We adjust the speed and offset so it fully clears A and shows B
	float sweep = uProgress * 3.0 - 1.5; 
	
	// burn distance field
	float d = p.x + p.y * 0.5 + 0.5 * fbm(p * 5.0) + sweep;
    
    // Thresholds adapted from original snippet
	if (d > 0.35) {
        col = clamp(col - (d - 0.35) * 10.0, 0.0, 1.0);
    }
    
	if (d > 0.47) {
		if (d < 0.5) {
            // Glowing ember edge
            float ember = (d - 0.47) * 33.0 * 0.5 * (noise(50.0 * p + vec2(-uTime * 2.0, 0.0)));
            col += ember * vec3(1.5, 0.5, 0.0);
        } else {
            col = tgt; 
        }
    }
	
	FragColor = vec4(col, 1.0);
}
