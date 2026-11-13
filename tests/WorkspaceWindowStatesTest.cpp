#include <gtest/gtest.h>

#include "../src/ui/WorkspaceWindowStates.h"

using adm::ui::ApplyWorkspaceWindowStates;
using adm::ui::WindowState;

namespace {

WindowState Window(const char* name, bool* open, const char* imguiId = "")
{
    WindowState w;
    w.name = name;
    w.openPtr = open;
    w.imguiId = imguiId;
    return w;
}

} // namespace

TEST(WorkspaceWindowStates, SavedStatesAreApplied)
{
    bool cpu = false, memory = true;
    ApplyWorkspaceWindowStates({{"CPU", true}, {"Memory", false}},
                               {Window("CPU", &cpu), Window("Memory", &memory)});
    EXPECT_TRUE(cpu);
    EXPECT_FALSE(memory);
}

// A window added after the workspace was saved must not carry over from the workspace before.
TEST(WorkspaceWindowStates, AWindowTheWorkspaceDoesNotMentionIsClosed)
{
    bool cpu = false, floppyDrives = true;
    ApplyWorkspaceWindowStates({{"CPU", true}},
                               {Window("CPU", &cpu), Window("Floppy Drives", &floppyDrives)});
    EXPECT_TRUE(cpu);
    EXPECT_FALSE(floppyDrives);
}

// A workspace saved before a window was renamed knows it by its ImGui id.
TEST(WorkspaceWindowStates, ARenamedWindowIsFoundByItsImguiId)
{
    bool capture = false;
    ApplyWorkspaceWindowStates({{"GIF Capture", true}},
                               {Window("GIF & Video Capture", &capture, "GIF Capture")});
    EXPECT_TRUE(capture);
}

TEST(WorkspaceWindowStates, TheMenuNameWinsOverTheImguiId)
{
    bool capture = true;
    ApplyWorkspaceWindowStates({{"GIF & Video Capture", false}, {"GIF Capture", true}},
                               {Window("GIF & Video Capture", &capture, "GIF Capture")});
    EXPECT_FALSE(capture);
}

TEST(WorkspaceWindowStates, AnEmptyWorkspaceClosesEverything)
{
    bool a = true, b = true;
    ApplyWorkspaceWindowStates({}, {Window("A", &a), Window("B", &b)});
    EXPECT_FALSE(a);
    EXPECT_FALSE(b);
}

TEST(WorkspaceWindowStates, AnEntryWithNoOpenFlagIsSkipped)
{
    EXPECT_NO_FATAL_FAILURE(ApplyWorkspaceWindowStates({{"A", true}}, {Window("A", nullptr)}));
}
