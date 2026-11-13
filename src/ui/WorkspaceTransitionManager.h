#pragma once

#include <glad/glad.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace adm::ui {

class WorkspaceTransitionManager
{
public:
    enum class State { Idle, CapturingA, WaitingForB, CapturingB, Transitioning, NameHold };

    WorkspaceTransitionManager();
    ~WorkspaceTransitionManager();

    void Initialize();
    void StartTransition(const std::string& workspaceName,
                         std::function<void()> applyLayoutCallback);

    // Called in the main loop
    void Update(float deltaTime);
    void OnRenderEnd(int width, int height);

    void SetDuration(float duration) { m_duration = duration; }

    bool IsTransitioning() const { return m_state != State::Idle; }
    bool IsShowingName() const
    {
        return m_state == State::Transitioning || m_state == State::NameHold;
    }
    State GetState() const { return m_state; }
    float GetProgress() const { return m_progress; }
    float GetNameProgress() const
    {
        float total = m_duration * 2.0f;
        return total > 0.0f ? std::clamp(m_nameTimer / total, 0.0f, 1.0f) : 1.0f;
    }

    const std::string& GetPendingWorkspace() const { return m_pendingWorkspace; }
    std::string GetCurrentTransitionName() const
    {
        if (m_shaders.empty() || m_currentTransitionIndex >= static_cast<int>(m_shaders.size()))
            return "";
        return m_shaders[m_currentTransitionIndex].name;
    }
    std::string GetCurrentTransitionAuthor() const
    {
        if (m_shaders.empty() || m_currentTransitionIndex >= static_cast<int>(m_shaders.size()))
            return "";
        return m_shaders[m_currentTransitionIndex].author;
    }

private:
    void InitGL();
    void CaptureFramebuffer(GLuint textureId, int width, int height);
    void RenderTransition(int width, int height);
    void RenderTexture(GLuint textureId, int width, int height);
    GLuint CompileShader(const char* vertexSource, const char* fragmentSource);

    State m_state = State::Idle;
    int m_currentTransitionIndex = 0;
    float m_progress = 0.0f;
    float m_duration = 0.6f;
    int m_waitFrames = 0;

    GLuint m_textureA = 0;
    GLuint m_textureB = 0;
    GLuint m_quadVao = 0;
    GLuint m_quadVbo = 0;
    GLuint m_textureProgram = 0;
    GLint m_uTexProgTexture = -1;
    GLint m_uTexProgResolution = -1;

    struct Shader
    {
        GLuint program = 0;
        GLint uProgress = -1;
        GLint uResolution = -1;
        GLint uTextureA = -1;
        GLint uTextureB = -1;
        GLint uTime = -1;
        std::string name;
        std::string author;
    };

    std::vector<Shader> m_shaders;
    std::string m_pendingWorkspace;
    std::function<void()> m_applyLayoutCallback;

    float m_time = 0.0f;
    float m_nameTimer = 0.0f;
};

} // namespace adm::ui
