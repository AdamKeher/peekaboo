#include "EmulatorFilter.h"
#include "../core/Logger.h"

namespace adm::ui {

namespace {

// Pass-through vertex shader. The quad maps clip position directly to texture
// coordinates (pos.xy*0.5+0.5 == uv), so with no effects enabled the pass is an
// identity copy regardless of the source texture's internal orientation.
const char* kVertexSrc = R"(
    #version 330 core
    layout (location = 0) in vec2 aPos;
    layout (location = 1) in vec2 aTexCoords;
    out vec2 TexCoords;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
        TexCoords = aTexCoords;
    }
)";

// Every effect is driven by a continuous intensity uniform; a disabled effect simply has its
// intensity set to the neutral value (0, or 1 for gamma/contrast/saturation) by the C++ side,
// so the shader needs no per-effect enable branch — it just skips work that would be a no-op.
const char* kFragmentSrc = R"(
    #version 330 core
    in vec2 TexCoords;
    out vec4 FragColor;

    uniform sampler2D uTexture;
    uniform vec2 uSrcRes;   // emulator framebuffer resolution
    uniform vec2 uOutRes;   // output (display) resolution
    uniform float uScanline;   // 0..1 line darkening
    uniform float uMask;       // 0..1 RGB triad strength
    uniform float uGlow;       // 0..0.5 phosphor bloom
    uniform float uCurve;      // 0..2 barrel distortion
    uniform float uVignette;   // 0..1 corner darkening
    uniform float uGamma;      // gamma (1 = neutral)
    uniform float uContrast;   // contrast (1 = neutral)
    uniform float uSaturation; // saturation (1 = neutral)
    uniform float uWarmth;     // warm/cool tint (0 = neutral)

    // Barrel distortion in 0..1 uv space, scaled by amt.
    vec2 curveUV(vec2 uv, float amt) {
        uv = uv * 2.0 - 1.0;
        vec2 offset = abs(uv.yx) / vec2(6.0, 5.0);
        uv = uv + uv * offset * offset * amt;
        return uv * 0.5 + 0.5;
    }

    vec3 sampleGlow(vec2 uv) {
        vec3 c = texture(uTexture, uv).rgb;
        if (uGlow > 0.001) {
            vec2 texel = 1.0 / uSrcRes;
            vec3 g = vec3(0.0);
            g += texture(uTexture, uv + vec2( texel.x, 0.0)).rgb;
            g += texture(uTexture, uv + vec2(-texel.x, 0.0)).rgb;
            g += texture(uTexture, uv + vec2(0.0,  texel.y)).rgb;
            g += texture(uTexture, uv + vec2(0.0, -texel.y)).rgb;
            c += g * uGlow;
        }
        return c;
    }

    vec3 colorGrade(vec3 c) {
        c = pow(max(c, 0.0), vec3(1.0 / uGamma));        // gamma lift
        c = (c - 0.5) * uContrast + 0.5;                 // contrast
        float l = dot(c, vec3(0.299, 0.587, 0.114));
        c = mix(vec3(l), c, uSaturation);                // saturation
        c *= vec3(1.0 + uWarmth, 1.0, 1.0 - uWarmth);    // warm/cool tint
        return c;
    }

    void main() {
        vec2 uv = TexCoords;
        if (uCurve > 0.001)
            uv = curveUV(uv, uCurve);

        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        vec3 col = sampleGlow(uv);

        // colorGrade() is neutral when gamma/contrast/saturation==1 and warmth==0, so this is
        // safe to run unconditionally (the disabled state passes those neutral values).
        col = colorGrade(col);

        if (uScanline > 0.001) {
            float s = sin(uv.y * uSrcRes.y * 3.14159265); // one band per source line
            col *= mix(1.0 - uScanline, 1.0, s * s);
        }

        if (uMask > 0.001) {
            float fx = fract(gl_FragCoord.x / 3.0); // RGB triad per 3 output columns
            float lo = 1.0 - uMask;
            vec3 mask = (fx < 1.0 / 3.0)   ? vec3(1.0, lo, lo)
                      : (fx < 2.0 / 3.0)   ? vec3(lo, 1.0, lo)
                                           : vec3(lo, lo, 1.0);
            col *= mask * (1.0 + uMask * 0.5); // compensate brightness lost to the mask
        }

        if (uVignette > 0.001) {
            vec2 v = uv * (1.0 - uv.yx);
            float vig = clamp(pow(v.x * v.y * 18.0, 0.18), 0.0, 1.0);
            col = mix(col, col * vig, uVignette);
        }

        FragColor = vec4(col, 1.0);
    }
)";

} // namespace

const std::vector<EmulatorFilterPreset>& EmulatorFilterPresets()
{
    static const std::vector<EmulatorFilterPreset> presets = [] {
        std::vector<EmulatorFilterPreset> p;

        // "Off" — every effect disabled. Always first; the default state.
        p.push_back({"Off", EmulatorFilterParams{}});

        // "CRT Monitor" — the full moderate stack (matches the legacy all-on look).
        {
            EmulatorFilterParams m;
            m.scanlines = true;
            m.crt = true;
            m.curvature = true;
            m.colorGrade = true;
            p.push_back({"CRT Monitor", m});
        }

        // "Arcade" — punchy: heavy scanlines + strong mask, flat (no curvature), richer colour.
        {
            EmulatorFilterParams a;
            a.scanlines = true;
            a.scanlineStrength = 0.45f;
            a.crt = true;
            a.maskStrength = 0.40f;
            a.glowStrength = 0.20f;
            a.curvature = false;
            a.colorGrade = true;
            a.contrast = 1.15f;
            a.saturation = 1.25f;
            a.warmth = 0.0f;
            p.push_back({"Arcade", a});
        }

        // "Composite" — soft glow without the RGB mask, gentle warm grade, no curvature.
        {
            EmulatorFilterParams c;
            c.scanlines = true;
            c.scanlineStrength = 0.15f;
            c.crt = true;
            c.maskStrength = 0.0f; // glow only
            c.glowStrength = 0.30f;
            c.curvature = false;
            c.colorGrade = true;
            c.gamma = 1.15f;
            c.contrast = 1.05f;
            c.saturation = 0.95f;
            c.warmth = 0.07f;
            p.push_back({"Composite", c});
        }

        return p;
    }();
    return presets;
}

EmulatorFilter::~EmulatorFilter()
{
    if (m_program)
        glDeleteProgram(m_program);
    if (m_outTex)
        glDeleteTextures(1, &m_outTex);
    if (m_fbo)
        glDeleteFramebuffers(1, &m_fbo);
    if (m_quadVbo)
        glDeleteBuffers(1, &m_quadVbo);
    if (m_quadVao)
        glDeleteVertexArrays(1, &m_quadVao);
    if (m_sampler)
        glDeleteSamplers(1, &m_sampler);
}

GLuint EmulatorFilter::CompileProgram(const char* vertexSource, const char* fragmentSource)
{
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            core::Logger::Error("EmulatorFilter shader compile error: {}", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, vertexSource);
    if (!vs)
        return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        core::Logger::Error("EmulatorFilter program link error: {}", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool EmulatorFilter::EnsureInit()
{
    if (m_initialized)
        return true;
    if (m_initFailed)
        return false;

    // The framebuffer/sampler entry points are core in GL 3.0/3.3, but they are used
    // nowhere else in the app — verify they actually loaded before calling them, so a
    // missing entry point degrades to "no filter" instead of a null-pointer crash.
    if (!glGenFramebuffers || !glBindFramebuffer || !glFramebufferTexture2D ||
        !glCheckFramebufferStatus || !glGenVertexArrays || !glGenSamplers) {
        core::Logger::Error("EmulatorFilter: required GL entry points unavailable; "
                            "GL_VERSION={}",
                            (const char*)glGetString(GL_VERSION));
        m_initFailed = true;
        return false;
    }

    m_program = CompileProgram(kVertexSrc, kFragmentSrc);
    if (!m_program) {
        m_initFailed = true;
        return false;
    }

    m_uTexture = glGetUniformLocation(m_program, "uTexture");
    m_uSrcRes = glGetUniformLocation(m_program, "uSrcRes");
    m_uOutRes = glGetUniformLocation(m_program, "uOutRes");
    m_uScanline = glGetUniformLocation(m_program, "uScanline");
    m_uMask = glGetUniformLocation(m_program, "uMask");
    m_uGlow = glGetUniformLocation(m_program, "uGlow");
    m_uCurve = glGetUniformLocation(m_program, "uCurve");
    m_uVignette = glGetUniformLocation(m_program, "uVignette");
    m_uGamma = glGetUniformLocation(m_program, "uGamma");
    m_uContrast = glGetUniformLocation(m_program, "uContrast");
    m_uSaturation = glGetUniformLocation(m_program, "uSaturation");
    m_uWarmth = glGetUniformLocation(m_program, "uWarmth");

    // Fullscreen quad: clip position == uv (no vertical flip).
    const float vertices[] = {
        -1.0f, 1.0f,  0.0f, 1.0f, //
        -1.0f, -1.0f, 0.0f, 0.0f, //
        1.0f,  -1.0f, 1.0f, 0.0f, //
        -1.0f, 1.0f,  0.0f, 1.0f, //
        1.0f,  -1.0f, 1.0f, 0.0f, //
        1.0f,  1.0f,  1.0f, 1.0f, //
    };

    glGenVertexArrays(1, &m_quadVao);
    glGenBuffers(1, &m_quadVbo);
    glBindVertexArray(m_quadVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenFramebuffers(1, &m_fbo);

    // NEAREST sampler keeps the base emulator pixels crisp regardless of the source
    // texture's own filter state.
    glGenSamplers(1, &m_sampler);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create the output texture object now; Apply() only resizes its storage, so no GL
    // objects are ever created inside a live frame.
    glGenTextures(1, &m_outTex);
    glBindTexture(GL_TEXTURE_2D, m_outTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_initialized = true;
    core::Logger::Info("EmulatorFilter: GL resources initialized");
    return true;
}

GLuint EmulatorFilter::Apply(GLuint srcTex, int srcW, int srcH, int outW, int outH,
                             const EmulatorFilterParams& params)
{
    if (srcTex == 0 || srcW <= 0 || srcH <= 0 || outW <= 0 || outH <= 0)
        return 0;
    if (!EnsureInit())
        return 0;

    // Save the state we touch *before* anything below binds the framebuffer — in particular
    // the resize branch binds m_fbo, so capturing the binding afterwards would record m_fbo as
    // "previous" and leave the offscreen FBO bound on return, rendering the whole UI off-screen
    // (black screen). Capturing here guarantees prevFbo is the real default framebuffer.
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);

    // Resize the output texture storage when the display size changes (the texture object
    // itself is created once in EnsureInit).
    if (outW != m_outW || outH != m_outH) {
        glBindTexture(GL_TEXTURE_2D, m_outTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_outTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            core::Logger::Error("EmulatorFilter: framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
            m_outW = m_outH = 0;
            return 0;
        }
        m_outW = outW;
        m_outH = outH;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, outW, outH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_program);
    glUniform2f(m_uSrcRes, (float)srcW, (float)srcH);
    glUniform2f(m_uOutRes, (float)outW, (float)outH);

    // A disabled effect passes its neutral value (0, or 1 for the tone curves) so the shader
    // can run unconditionally without per-effect enable branches.
    glUniform1f(m_uScanline, params.scanlines ? params.scanlineStrength : 0.0f);
    glUniform1f(m_uMask, params.crt ? params.maskStrength : 0.0f);
    glUniform1f(m_uGlow, params.crt ? params.glowStrength : 0.0f);
    glUniform1f(m_uCurve, params.curvature ? params.curveAmount : 0.0f);
    glUniform1f(m_uVignette, params.curvature ? params.vignetteStrength : 0.0f);
    glUniform1f(m_uGamma, params.colorGrade ? params.gamma : 1.0f);
    glUniform1f(m_uContrast, params.colorGrade ? params.contrast : 1.0f);
    glUniform1f(m_uSaturation, params.colorGrade ? params.saturation : 1.0f);
    glUniform1f(m_uWarmth, params.colorGrade ? params.warmth : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glBindSampler(0, m_sampler);
    glUniform1i(m_uTexture, 0);

    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore.
    glBindVertexArray(0);
    glBindSampler(0, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevScissor)
        glEnable(GL_SCISSOR_TEST);
    if (prevDepth)
        glEnable(GL_DEPTH_TEST);
    if (prevBlend)
        glEnable(GL_BLEND);

    return m_outTex;
}

} // namespace adm::ui
