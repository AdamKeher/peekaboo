#pragma once

#include <glad/glad.h>

#include <string>
#include <vector>

namespace adm::ui {

// Post-process display effects applied to the emulator framebuffer. Each effect has an
// independent enable flag plus one or more continuous intensity parameters, so effects may
// be combined freely and dialled in by the Display Filters window. The defaults reproduce a
// moderate CRT look, but the enable flags all start false (no filtering until requested).
struct EmulatorFilterParams
{
    // CRT raster-line darkening.
    bool scanlines = false;
    float scanlineStrength = 0.25f; // 0..1, how dark the gaps between lines get

    // Aperture-grille RGB triad mask + phosphor glow (one toggle, two dials so the bloom can
    // be used without the mask, e.g. for a composite look).
    bool crt = false;
    float maskStrength = 0.30f; // 0..1, RGB triad contrast
    float glowStrength = 0.15f; // 0..0.5, neighbour bloom added back in

    // Barrel distortion + corner vignette.
    bool curvature = false;
    float curveAmount = 1.00f;      // 0..2, barrel distortion scale
    float vignetteStrength = 1.00f; // 0..1, corner darkening

    // Tone / colour shaping.
    bool colorGrade = false;
    float gamma = 1.10f;      // 0.5..2.0, gamma lift
    float contrast = 1.08f;   // 0.5..1.5
    float saturation = 1.15f; // 0..2
    float warmth = 0.04f;     // -0.2..0.2, warm (+) / cool (-) tint

    bool AnyEnabled() const { return scanlines || crt || curvature || colorGrade; }
};

// A named, combinable starting point the user can pick from the Display Filters window.
struct EmulatorFilterPreset
{
    std::string name;
    EmulatorFilterParams params;
};

// Built-in presets. The first entry is always "Off" (every effect disabled), which is also
// the out-of-the-box state.
const std::vector<EmulatorFilterPreset>& EmulatorFilterPresets();

// Renders the emulator texture through a single fragment-shader pass into an offscreen
// framebuffer, applying any enabled effects, and returns the resulting texture for display.
// GL resources are created lazily on first use and torn down in the destructor. All work
// happens off-screen, so the caller's framebuffer/viewport are saved and restored.
class EmulatorFilter
{
public:
    EmulatorFilter() = default;
    ~EmulatorFilter();

    EmulatorFilter(const EmulatorFilter&) = delete;
    EmulatorFilter& operator=(const EmulatorFilter&) = delete;

    // Eagerly build all GL objects (shader program, VAO, FBO, sampler) up front, while no
    // ImGui frame is in flight. Creating/compiling GL objects lazily inside a live frame can
    // intermittently deadlock NVIDIA's threaded driver, so callers should invoke this once at
    // startup. Returns false if GL resources could not be created. Safe to call repeatedly.
    bool Initialize() { return EnsureInit(); }

    // Render srcTex (the emulator framebuffer, srcW x srcH) through the filter into an
    // output texture of size outW x outH. Returns the output texture id, or 0 on failure
    // (the caller should then fall back to drawing srcTex directly).
    GLuint Apply(GLuint srcTex, int srcW, int srcH, int outW, int outH,
                 const EmulatorFilterParams& params);

    // True once Initialize()/the first Apply() has successfully built GL resources.
    bool IsInitialized() const { return m_initialized; }

private:
    bool EnsureInit();
    static GLuint CompileProgram(const char* vertexSource, const char* fragmentSource);

    bool m_initialized = false;
    bool m_initFailed = false;

    GLuint m_fbo = 0;
    GLuint m_outTex = 0;
    int m_outW = 0;
    int m_outH = 0;

    GLuint m_quadVao = 0;
    GLuint m_quadVbo = 0;
    GLuint m_sampler = 0; // NEAREST input sampler, keeps base pixels crisp
    GLuint m_program = 0;

    GLint m_uTexture = -1;
    GLint m_uSrcRes = -1;
    GLint m_uOutRes = -1;
    GLint m_uScanline = -1;
    GLint m_uMask = -1;
    GLint m_uGlow = -1;
    GLint m_uCurve = -1;
    GLint m_uVignette = -1;
    GLint m_uGamma = -1;
    GLint m_uContrast = -1;
    GLint m_uSaturation = -1;
    GLint m_uWarmth = -1;
};

} // namespace adm::ui
