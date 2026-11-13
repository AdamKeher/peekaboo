#pragma once

#include <deque>
#include <functional>
#include <string>

namespace adm::ui {

/**
 * @brief The "you have unsaved changes" modal, and the queue behind it.
 *
 * A queue rather than a single dialog because Close All and Exit can each raise several at once,
 * and they have to be answered one at a time. Cancel, Escape and a failed save all clear the
 * whole queue: the user said stop, and asking again about the next five documents is not what
 * they meant.
 *
 * Owns no collaborators - every action arrives as a callback with the enqueued item, so this
 * knows nothing about documents, saving or exiting.
 *
 * Extracted from Application.
 */
class UnsavedChangesDialog
{
public:
    /**
     * @brief Queue one confirmation.
     *
     * @param filename  Shown in the dialog; empty gives the generic message.
     * @param onSave    Save action; returns true on success.
     * @param onProceed Called after a successful save, or after "Don't Save".
     * @param onCancel  Called on Cancel / Escape / failed save. Optional.
     */
    void Enqueue(const std::string& filename, std::function<bool()> onSave,
                 std::function<void()> onProceed, std::function<void()> onCancel = {});

    /// Draw the modal if anything is queued. Call once per frame.
    void Draw();

private:
    struct Confirmation
    {
        std::string filename;            // shown in dialog; empty = generic message
        std::function<bool()> onSave;    // save action; returns true on success
        std::function<void()> onProceed; // called after save or after "Don't Save"
        std::function<void()> onCancel;  // called on Cancel / Escape / failed save (optional)
    };

    std::deque<Confirmation> m_queue;
    bool m_trigger = false;
};

} // namespace adm::ui
