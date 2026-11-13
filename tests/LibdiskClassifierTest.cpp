#include <gtest/gtest.h>

#ifdef ADM_HAVE_LIBDISK

#include <array>
#include <string>
#include <vector>

#include "core/AmigaMfmDecoder.h"
#include "core/LibdiskClassifier.h"

using adm::core::AmigaMfmDecoder;
using adm::core::LibdiskClassifier;

// The classifier must create its libdisk probe disk, and running the full probe
// path (soft stream -> every handler -> cleanup) must not crash. We do NOT
// assert a specific recognised format here: EncodeStandardTrack zeroes the MFM
// clock bits (fine for our own decoder, which masks them) so the synthetic track
// is not flux-valid the way a real capsimg track is. This test validates the
// integration plumbing; format-recognition correctness needs a real IPF.
TEST(LibdiskClassifierTest, AvailableAndProbesWithoutCrashing)
{
    LibdiskClassifier classifier;
    ASSERT_TRUE(classifier.Available());

    std::vector<std::array<uint8_t, AmigaMfmDecoder::kSectorSize>> sectors(
        AmigaMfmDecoder::kSectorsPerTrack);
    auto mfm = AmigaMfmDecoder::EncodeStandardTrack(0, sectors);

    std::string name = classifier.Classify(mfm.data(), mfm.size(), 0);
    (void)name; // reaching here means the libdisk path ran end-to-end
    SUCCEED();
}

// Deterministic guard rails: empty input and out-of-range track numbers must
// return an empty string without touching libdisk.
TEST(LibdiskClassifierTest, EmptyAndOutOfRangeReturnEmpty)
{
    LibdiskClassifier classifier;
    EXPECT_EQ(classifier.Classify(nullptr, 0, 0), "");

    std::vector<uint8_t> buf(256, 0);
    EXPECT_EQ(classifier.Classify(buf.data(), buf.size(), 9999), ""); // tracknr >= 160
}

#endif // ADM_HAVE_LIBDISK
