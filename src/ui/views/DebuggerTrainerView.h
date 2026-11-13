#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../../core/MemoryLocks.h"
#include "../../core/TrainerFile.h"

namespace adm::ui {

/**
 * @brief The Trainer window: every memory lock and patch, with somewhere to add more.
 *
 * This is the same core::MemoryLocks store the PR console command drives and the register
 * windows and memory inspector mutate, so a lock made anywhere shows up here immediately and
 * vice versa — there is no synchronisation code, only one object.
 *
 * The window deliberately never enables anything on its own. Loading a trainer file resolves
 * every anchor and reports the outcome per entry; turning one on is always the user's click,
 * because an entry whose guard bytes do not match may well be pointing at a different
 * release's data.
 */
class DebuggerTrainerView
{
public:
    DebuggerTrainerView(core::MemoryLocks* locks, std::function<core::LockContext()> makeContext)
        : m_locks(locks), m_makeContext(std::move(makeContext))
    {
    }

    void Draw();
    void Clear()
    {
        m_selected.clear();
        m_status.clear();
        m_statusIsError = false;
    }

    bool* GetOpenPtr() { return &m_open; }
    bool IsOpen() const { return m_open; }
    void Open() { m_open = true; }
    void Restore(bool open)
    {
        m_open = open;
        if (open)
            m_noFocus = true;
    }

    /// Whether the core can actually enforce a lock. Tri-state on purpose: before a game is
    /// started there is no core to ask, and answering "unsupported" then produces a scary
    /// rebuild-your-core warning on a perfectly good build, which disappears the moment
    /// anything boots. "I cannot tell yet" and "I checked, and no" are different answers.
    enum class ProtectionSupport {
        Unknown,     ///< No core loaded yet - say nothing.
        Unsupported, ///< Core loaded and it has no protection hooks - warn.
        Supported
    };

    /// Reports whether the loaded core can enforce a lock, so the window can say so rather
    /// than letting the user believe a lock is live when nothing enforces it.
    void SetProtectionSupport(std::function<ProtectionSupport()> fn)
    {
        m_supported = std::move(fn);
    }

    /// Called after a trainer file is auto-discovered for freshly loaded media, so the window
    /// can show which release it was authored for and how confident the match is.
    void SetLoadedTrainer(std::string path, core::TrainerMatch match)
    {
        m_trainerPath = std::move(path);
        m_trainerMatch = match;
    }

    /// Opens a trainer file by path (shared with the auto-discovery path in Application).
    bool LoadTrainerFile(const std::string& path, core::TrainerMatch match, std::string& err);

private:
    void DrawToolbar();
    void DrawTable();
    void DrawDetail();
    void DrawAddDialog();

    void SetStatus(const std::string& text, bool error = false)
    {
        m_status = text;
        m_statusIsError = error;
    }

    core::MemoryLocks* m_locks = nullptr;
    std::function<core::LockContext()> m_makeContext;
    std::function<ProtectionSupport()> m_supported;

    bool m_open = false;
    bool m_noFocus = false;

    std::string m_selected; ///< id of the highlighted row
    std::string m_status;
    bool m_statusIsError = false;

    std::string m_trainerPath;
    core::TrainerMatch m_trainerMatch = core::TrainerMatch::None;

    // "Add lock" dialog state.
    bool m_addOpen = false;
    char m_addDesc[128] = {};
    char m_addAddr[32] = {};
    char m_addValue[32] = {};
    char m_addBytes[128] = {};
    char m_addPattern[192] = {};
    char m_addVerify[64] = {};
    int m_addMode = 0;    ///< 0 block, 1 set, 2 patch
    int m_addAnchor = 0;  ///< 0 absolute, 1 seglist, 2 signature, 3 operand
    int m_addSizeIdx = 0; ///< 0 = 8-bit, 1 = 16-bit, 2 = 32-bit
    int m_addLen = 1;
    int m_addSeg = 0;
    int m_addSegOffset = 0;
    int m_addTargetOffset = 0;
    int m_addOperandAt = 2;
    int m_addOperandSize = 4;
};

} // namespace adm::ui
