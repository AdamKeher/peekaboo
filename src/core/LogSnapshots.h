#pragma once

#include "AudioLogSnapshot.h"
#include "BlitLogSnapshot.h"
#include "DiskLogSnapshot.h"
#include "RegLogSnapshot.h"

namespace adm::ui {
class EmulatorView;
class RegisterOpsView;
} // namespace adm::ui

namespace adm::core {

class BlitProfiler;
class DiskProfiler;
class RegWriteProfiler;

/**
 * @brief Builds the self-contained log snapshots the AR console and the MCP tools hand out.
 *
 * Every method here is a read over a profiler (or the emulator's audio counters) flattened into
 * a plain struct with no pointers back into live state, so a caller can hold the result across
 * frames. Nothing in this class reaches back into Application: it is given its sources once and
 * only ever reads them.
 *
 * ApplyAudioRecordRequest is the one write, and it lives here because it is the other half of
 * the audio-tap surface MakeAudioLogSnapshot reports on - splitting them would put one feature
 * in two files.
 *
 * Extracted from Application.
 */
class LogSnapshots
{
public:
    /**
     * @brief Supply the sources every snapshot reads from.
     *
     * All five are owned by Application and outlive this object; they are constructed together
     * in its initialiser list, so this is called from its constructor body. Any of them may be
     * null at call time, and each method degrades to an unavailable snapshot rather than
     * faulting.
     */
    void Init(ui::EmulatorView* emulatorView, BlitProfiler* blitProfiler,
              RegWriteProfiler* regWriteProfiler, DiskProfiler* diskProfiler,
              ui::RegisterOpsView* registerOpsView);

    // Health of the emulator's audio path, for the AR console AUDIO command, the MCP
    // get_audio_stats tool and the --record-audio startup flag. Session-scoped, not per frame.
    core::AudioLogSnapshot MakeAudioLogSnapshot() const;

    // Starts or stops the audio debug taps, for the AR console AUDREC command, the MCP
    // start_audio_record / stop_audio_record tools and the --record-audio startup flag.
    core::AudioRecordResult ApplyAudioRecordRequest(const core::AudioRecordRequest& req);

    // Builds a self-contained snapshot of the current frame's blitter activity from the
    // BlitProfiler, for the AR console BLIT command and the MCP get_blitter_log tool.
    // limit <= 0 returns all blits; otherwise the first `limit` are returned (truncated flag set).
    core::BlitLogSnapshot MakeBlitLogSnapshot(int limit) const;

    // Builds a self-contained snapshot of the current frame's custom-register writes from the
    // RegWriteProfiler, for the AR console REGLOG command and the MCP get_register_log tool.
    // limit <= 0 returns all writes; otherwise the first `limit` are returned (truncated set).
    core::RegLogSnapshot MakeRegLogSnapshot(int limit) const;

    // Builds a self-contained snapshot of the session's disk DMA traffic from the
    // DiskProfiler, for the AR console DSKLOG command and the MCP get_disk_log tool.
    // Unlike the blitter log this spans the whole session, not just the current frame.
    core::DiskLogSnapshot MakeDiskLogSnapshot(const core::DiskLogQuery& q) const;

private:
    ui::EmulatorView* m_emulatorView = nullptr;
    BlitProfiler* m_blitProfiler = nullptr;
    RegWriteProfiler* m_regWriteProfiler = nullptr;
    DiskProfiler* m_diskProfiler = nullptr;
    ui::RegisterOpsView* m_registerOpsView = nullptr;
};

} // namespace adm::core
