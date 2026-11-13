#include "WorkspaceTransitionManager.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include "../core/Logger.h"

namespace adm::ui {

namespace fs = std::filesystem;

static std::string LoadShaderSource(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        core::Logger::Error("Failed to open shader file: {}", path);
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

WorkspaceTransitionManager::WorkspaceTransitionManager() { m_duration = 0.6f; }

void WorkspaceTransitionManager::Initialize() { InitGL(); }

WorkspaceTransitionManager::~WorkspaceTransitionManager()
{
    if (m_textureA)
        glDeleteTextures(1, &m_textureA);
    if (m_textureB)
        glDeleteTextures(1, &m_textureB);
    if (m_quadVao)
        glDeleteVertexArrays(1, &m_quadVao);
    if (m_quadVbo)
        glDeleteBuffers(1, &m_quadVbo);
    if (m_textureProgram)
        glDeleteProgram(m_textureProgram);
    for (auto& s : m_shaders) {
        glDeleteProgram(s.program);
    }
}

void WorkspaceTransitionManager::InitGL()
{
    // Create the two capture textures (linear filter, clamp to edge).
    auto createCaptureTexture = [](GLuint& tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    createCaptureTexture(m_textureA);
    createCaptureTexture(m_textureB);

    // Create quad
    float vertices[] = {-1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,

                        -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f};

    glGenVertexArrays(1, &m_quadVao);
    glGenBuffers(1, &m_quadVbo);
    glBindVertexArray(m_quadVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    // Compile shaders
    std::string vertSrc = LoadShaderSource("assets/shaders/transition_vert.glsl");
    if (vertSrc.empty()) {
        core::Logger::Error(
            "WorkspaceTransitionManager: Vertex shader missing (transition_vert.glsl).");
        return;
    }

    // Compile simple texture shader
    const char* fragTexSrc = R"(
        #version 330 core
        in vec2 TexCoords;
        out vec4 FragColor;
        uniform sampler2D uTexture;
        void main() {
            FragColor = texture(uTexture, TexCoords);
        }
    )";
    m_textureProgram = CompileShader(vertSrc.c_str(), fragTexSrc);
    if (m_textureProgram != 0) {
        m_uTexProgTexture = glGetUniformLocation(m_textureProgram, "uTexture");
    }

    // Traverse assets/shaders and load all *_frag.glsl files
    std::string shaderDir = "assets/shaders";
    if (!fs::exists(shaderDir)) {
        core::Logger::Error("WorkspaceTransitionManager: Shader directory not found: {}",
                            shaderDir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(shaderDir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.ends_with("_frag.glsl")) {
                std::string fragSrc = LoadShaderSource(entry.path().string());
                if (fragSrc.empty())
                    continue;

                // Extract name and author from comments
                std::string shaderName = "";
                std::string shaderAuthor = "";

                std::istringstream iss(fragSrc);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.rfind("// name: ", 0) == 0) {
                        shaderName = line.substr(9);
                    } else if (line.rfind("// original author: ", 0) == 0) {
                        shaderAuthor = line.substr(20);
                    } else if (line.rfind("// original author:", 0) == 0) {
                        shaderAuthor = line.substr(19);
                    }

                    // Stop searching after first few lines or if both found
                    if (!shaderName.empty() && !shaderAuthor.empty())
                        break;
                    if (line.size() > 0 && line[0] != '/' && line[0] != '\r' && line[0] != '\n')
                        break;
                }

                // Trim whitespace
                auto trim = [](std::string& s) {
                    s.erase(0, s.find_first_not_of(" \t\r\n"));
                    s.erase(s.find_last_not_of(" \t\r\n") + 1);
                };
                if (!shaderName.empty())
                    trim(shaderName);
                if (!shaderAuthor.empty())
                    trim(shaderAuthor);

                if (shaderName.empty()) {
                    shaderName = filename.substr(0, filename.size() - 10); // Strip "_frag.glsl"
                }

                Shader s;
                s.program = CompileShader(vertSrc.c_str(), fragSrc.c_str());
                if (s.program != 0) {
                    s.uProgress = glGetUniformLocation(s.program, "uProgress");
                    s.uResolution = glGetUniformLocation(s.program, "uResolution");
                    s.uTextureA = glGetUniformLocation(s.program, "uTextureA");
                    s.uTextureB = glGetUniformLocation(s.program, "uTextureB");
                    s.uTime = glGetUniformLocation(s.program, "uTime");
                    s.name = shaderName;
                    s.author = shaderAuthor;
                    m_shaders.push_back(s);
                    core::Logger::Info("Loaded transition shader: {} (author: {})", s.name,
                                       s.author);
                }
            }
        }
    }

    if (m_shaders.empty()) {
        core::Logger::Error("WorkspaceTransitionManager: No transition shaders loaded!");
    }
}

GLuint WorkspaceTransitionManager::CompileShader(const char* vertexSource,
                                                 const char* fragmentSource)
{
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexSource, nullptr);
    glCompileShader(vertexShader);

    // Check compilation
    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        core::Logger::Error("Vertex shader compilation error: {}", infoLog);
        return 0;
    }

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentSource, nullptr);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        core::Logger::Error("Fragment shader compilation error: {}", infoLog);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        core::Logger::Error("Shader program link error: {}", infoLog);
        return 0;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return program;
}

void WorkspaceTransitionManager::StartTransition(const std::string& workspaceName,
                                                 std::function<void()> applyLayoutCallback)
{
    if (m_state != State::Idle || m_shaders.empty())
        return;

    m_pendingWorkspace = workspaceName;
    m_applyLayoutCallback = applyLayoutCallback;
    m_state = State::CapturingA;
    m_progress = 0.0f;
    m_time = 0.0f;

    // Cycle through transitions
    m_currentTransitionIndex = (m_currentTransitionIndex + 1) % m_shaders.size();
}

void WorkspaceTransitionManager::Update(float deltaTime)
{
    m_time += deltaTime;
    if (m_state == State::Transitioning) {
        m_nameTimer += deltaTime;
        m_progress += deltaTime / m_duration;
        if (m_progress > 1.0f) {
            m_progress = 1.0f;
        }
        // State transition is handled in OnRenderEnd
    } else if (m_state == State::NameHold) {
        m_nameTimer += deltaTime;
    }
}

void WorkspaceTransitionManager::CaptureFramebuffer(GLuint textureId, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, textureId);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);
}

void WorkspaceTransitionManager::OnRenderEnd(int width, int height)
{
    if (m_state == State::CapturingA) {
        CaptureFramebuffer(m_textureA, width, height);
        if (m_applyLayoutCallback) {
            m_applyLayoutCallback();
        }
        m_state = State::WaitingForB;
        m_waitFrames = 0;
    } else if (m_state == State::WaitingForB) {
        // Give ImGui frames to settle the new layout.
        // Complex docking layouts on Windows can take several frames to stabilize.
        m_waitFrames++;
        if (m_waitFrames < 10) {
            RenderTexture(m_textureA, width, height);
        } else {
            // Frame 10: ImGui has rendered the NEW layout B. Capture it now.
            CaptureFramebuffer(m_textureB, width, height);
            m_state = State::Transitioning;
            m_progress = 0.0f;
            m_nameTimer = 0.0f;
            // Start showing the transition immediately to hide the raw capture
            RenderTransition(width, height);
        }
    } else if (m_state == State::CapturingB) {
        // This state is now mostly bypassed by the logic above,
        // but kept for robustness if state is set externally.
        CaptureFramebuffer(m_textureB, width, height);
        m_state = State::Transitioning;
        RenderTransition(width, height);
    } else if (m_state == State::Transitioning) {
        RenderTransition(width, height);

        // Shader animation complete — hold so the name label can finish fading.
        if (m_progress >= 1.0f) {
            m_state = State::NameHold;
        }
    } else if (m_state == State::NameHold) {
        // Show destination frame while the name label finishes fading out.
        RenderTexture(m_textureB, width, height);

        // Total name display = 2 * m_duration; go idle once that elapses.
        if (m_nameTimer >= m_duration * 2.0f) {
            m_state = State::Idle;
        }
    }
}

void WorkspaceTransitionManager::RenderTransition(int width, int height)
{
    if (m_shaders.empty() || m_currentTransitionIndex >= static_cast<int>(m_shaders.size()))
        return;

    const auto& s = m_shaders[m_currentTransitionIndex];
    glUseProgram(s.program);

    glUniform1f(s.uProgress, m_progress);
    glUniform1f(s.uTime, m_time);
    glUniform2f(s.uResolution, (float)width, (float)height);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureA);
    glUniform1i(s.uTextureA, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureB);
    glUniform1i(s.uTextureB, 1);

    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore the state we touched — leaving GL_TEXTURE1 active (or our program/VAO bound)
    // corrupts whatever the ImGui backend draws next.
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void WorkspaceTransitionManager::RenderTexture(GLuint textureId, int width, int height)
{
    if (!m_textureProgram)
        return;

    glUseProgram(m_textureProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glUniform1i(m_uTexProgTexture, 0);

    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace adm::ui
