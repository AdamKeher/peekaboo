#pragma once

namespace adm::core {

// The ranges of a FloppyBridge slot's settings, as the Floppy Drives window's tables hold them:
// bridge mode (Fast, Compatible, Turbo AmigaDOS, Stalling), density (Auto, DD, HD) and drive
// cable (drive A/B, Shugart 0-3). Settings read from prefs or a .peek are clamped to these.
constexpr int kMaxBridgeMode = 3;
constexpr int kMaxBridgeDensity = 2;
constexpr int kMaxDriveCable = 5;

} // namespace adm::core
