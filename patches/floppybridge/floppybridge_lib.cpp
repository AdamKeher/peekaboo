/* floppybridge_lib
*
* Copyright (C) 2021-2024 Robert Smith (@RobSmithDev)
* https://amiga.robsmithdev.co.uk
*
* ADM: this file replaces upstream's DLL-shim implementation wholesale.
*
* Upstream, FloppyBridgeAPI is a thin proxy that LoadLibrary()s a separate
* FloppyBridge.dll and forwards every call through exported C functions. We
* compile the driver sources directly into the core instead (see
* docs/floppy.md milestone 2), so there is no DLL to find, no ABI to keep and
* no reason for the indirection: this is a static factory over the vendored
* concrete drivers, and every FloppyDiskBridge virtual forwards straight to the
* held CommonBridgeTemplate.
*
* The class declaration in floppybridge_lib.h is unchanged apart from four
* methods the 2024 floppybridge_abstract.h added, so disk.c needs no
* modification whatsoever.
*
* What is deliberately NOT implemented (all stubbed to false/empty):
*
*   - the whole profile-ID system (createDriverFromProfileID, getAllProfiles,
*     importProfilesFromString, exportProfilesToString, getProfileConfigAsString,
*     setProfileConfigFromString, createNewProfile, setProfileName,
*     deleteProfile). That exists only for RobSmithDev's own Windows
*     configuration GUI. disk.c only ever reaches it as a fallback after
*     createDriverFromString() has failed, and this app always supplies a
*     fully-specified config string, so the path is never exercised.
*   - showProfileConfigDialog(), which is an HWND-based Win32 dialog. The host
*     app has its own configuration UI.
*   - the DNS update check in getBridgeDriverInformation(). We are pinned to a
*     specific commit; asking a name server whether a newer FloppyBridge exists
*     would be both useless and a surprise network call from an emulator core.
*
* Config string format is upstream's own, so profiles remain interchangeable:
*
*     [bridgeIndex|flags|comPort|bridgeMode|bridgeDensity]
*
* optionally prefixed by a profile name. bridgeIndex is 0 = DrawBridge/Arduino,
* 1 = Greaseweazle, 2 = SuperCard Pro - the same order as getDriverList()
* returns, which is now our own choice rather than a DLL ABI.
*
* This is free and unencumbered released into the public domain
* But please don't remove the above information
*
* For more details visit <http://unlicense.org>.
*
*/

#include "floppybridge_lib.h"
#include "floppybridge_common.h"

/* Brings in ArduinoFloppyBridge.h, GreaseWeazleBridge.h and SuperCardProBridge.h
   with their ROMTYPE_ enable switches already defined. */
#include "floppybridge_config.h"

#include "CommonBridgeTemplate.h"
#include "AdmXCopyBridge.h"
#include "AdmXCopyBufferedBridge.h"
#include "SerialIO.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define MAX_NUM_DRIVERS 5

///////////////////////////////////////////////////////////////////////////////
// Internal state
///////////////////////////////////////////////////////////////////////////////

namespace {

// Copy a C string into one of the fixed-size TCharString fields, always
// null-terminated. TCHAR is char in this build (see floppybridge_abstract.h).
void setTCharString(TCharString& dest, const char* src)
{
	if (!src) {
		dest[0] = '\0';
		return;
	}
	std::snprintf(dest, BRIDGE_STRING_MAX_LENGTH, "%s", src);
}

// The settings a bridge is created with. This mirrors upstream's BridgeConfig
// in windows/FloppyBridge.cpp - including its serialisation format, so config
// strings written by WinUAE's FloppyBridge.dll are read identically here.
struct BridgeConfig
{
	unsigned int bridgeIndex = 0;

	FloppyBridge::BridgeMode bridgeMode = FloppyBridge::BridgeMode::bmFast;
	FloppyBridge::BridgeDensityMode bridgeDensity = FloppyBridge::BridgeDensityMode::bdmAuto;

	// Not part of the [...] payload
	char profileName[128] = { 0 };

	// Not every driver uses every one of these
	char comPortToUse[128] = { 0 };
	bool autoDetectComPort = true;
	FloppyBridge::DriveSelection driveCable = FloppyBridge::DriveSelection::dsDriveA;
	bool autoCache = false;
	bool smartSpeed = false;
	/* Driver 4 only, and OURS rather than upstream's - flags bit 64, which upstream
	   leaves free. Writing to a real disk is destructive and irreversible, so a live
	   slot is read-only until something explicitly says otherwise; the driver reports
	   write-protected to the guest whatever the disk's own tab says. An old config
	   string never sets the bit, which is exactly the default wanted. */
	bool allowWrites = false;

	// Filled in by toString(); the returned pointer is into here
	char lastSerialise[255] = { 0 };

	bool fromString(const char* serialisedOptions);
	void toString(char** serialisedOptions);
};

bool BridgeConfig::fromString(const char* serialisedOptions)
{
	if (!serialisedOptions) return false;
	if (strlen(serialisedOptions) < 7) return false;

	std::string tmp = serialisedOptions;

	// An optional profile name may precede the '['
	size_t pos = tmp.find('[');
	if (pos == std::string::npos) return false;
	if (pos) {
		std::snprintf(profileName, sizeof(profileName), "%s", tmp.substr(0, pos).c_str());
		tmp = tmp.substr(pos);
	} else {
		profileName[0] = '\0';
	}

	if ((tmp[0] != '[') || (tmp[tmp.length() - 1] != ']')) return false;
	tmp = tmp.substr(1, tmp.length() - 2);

	// Split on '|'
	std::vector<std::string> params;
	pos = tmp.find('|');
	while (pos != std::string::npos) {
		params.push_back(tmp.substr(0, pos));
		tmp = tmp.substr(pos + 1);
		pos = tmp.find('|');
	}
	params.push_back(tmp);

	if (params.size() < 5) return false;

	bridgeIndex = (unsigned int)atoi(params[0].c_str());
	if (bridgeIndex >= MAX_NUM_DRIVERS) return false;

	unsigned int i = (unsigned int)atoi(params[1].c_str());
	autoCache = (i & 1) != 0;
	driveCable = (FloppyBridge::DriveSelection)(((i & 2) >> 1) | ((i & 48) >> 3));
	autoDetectComPort = (i & 4) != 0;
	smartSpeed = (i & 8) != 0;
	allowWrites = (i & 64) != 0;

	std::snprintf(comPortToUse, sizeof(comPortToUse), "%s", params[2].c_str());

	i = (unsigned int)atoi(params[3].c_str());
	if (i > (unsigned int)FloppyBridge::BridgeMode::bmMax) return false;
	bridgeMode = (FloppyBridge::BridgeMode)i;

	i = (unsigned int)atoi(params[4].c_str());
	if (i > (unsigned int)FloppyBridge::BridgeDensityMode::bdmMax) return false;
	bridgeDensity = (FloppyBridge::BridgeDensityMode)i;

	return true;
}

void BridgeConfig::toString(char** serialisedOptions)
{
	const unsigned int i = (unsigned int)driveCable;
	const unsigned int flags = (autoCache ? 1 : 0) | ((i & 1) << 1) | ((i & 6) << 3) |
	                           (autoDetectComPort ? 4 : 0) | (smartSpeed ? 8 : 0) |
	                           (allowWrites ? 64 : 0);

	const std::string tmp = std::string(profileName) + "[" + std::to_string(bridgeIndex) + "|" +
	                        std::to_string(flags) + "|" + std::string(comPortToUse) + "|" +
	                        std::to_string((unsigned int)bridgeMode) + "|" +
	                        std::to_string((unsigned int)bridgeDensity) + "]";

	std::snprintf(lastSerialise, sizeof(lastSerialise), "%s", tmp.c_str());

	if (serialisedOptions) *serialisedOptions = lastSerialise;
}

// Everything FloppyBridgeAPI owns. m_handle points at one of these; keeping it
// out of the header means the header stays byte-for-byte compatible with what
// disk.c already compiles against.
struct BridgeState
{
	BridgeConfig config;
	// FloppyDiskBridge, not CommonBridgeTemplate: driver 3 (XCopy streaming) is our
	// own streaming bridge, which implements the interface directly rather than
	// deriving from upstream's buffered template.
	FloppyDiskBridge* bridge = nullptr;
	// The same object when it IS one of upstream's buffered drivers, null for the
	// streaming one. Only changeBridgeMode/changeBridgeDensity need it.
	CommonBridgeTemplate* buffered = nullptr;
};

// Storage backing enumCOMPorts()' returned pointers, which are documented as
// valid only until the next call.
std::vector<SerialIO::SerialPortInformation> g_serialPorts;
std::vector<std::string> g_serialPortNames;

const FloppyDiskBridge::BridgeDriver* staticDriverInfo(unsigned int driverIndex)
{
	switch (driverIndex) {
	case 0: return ArduinoFloppyDiskBridge::staticBridgeInformation();
	case 1: return GreaseWeazleDiskBridge::staticBridgeInformation();
	case 2: return SupercardProDiskBridge::staticBridgeInformation();
	case 3: return AdmXCopyBridge::staticBridgeInformation();
	case 4: return AdmXCopyBufferedBridge::staticBridgeInformation();
	default: return nullptr;
	}
}

// Create the concrete driver for a config. Note upstream only constructs the
// bridge at open time, not at createDriver time - we keep that, because
// constructing a CommonBridgeTemplate spins up its worker thread.
FloppyDiskBridge* makeBridge(const BridgeConfig& c)
{
	switch (c.bridgeIndex) {
	case 0:
		return new ArduinoFloppyDiskBridge(c.bridgeMode, c.bridgeDensity, c.autoCache, c.smartSpeed,
		                                   c.autoDetectComPort, (char*)c.comPortToUse);
	case 1:
		return new GreaseWeazleDiskBridge(c.bridgeMode, c.bridgeDensity, c.autoCache, c.smartSpeed,
		                                  c.autoDetectComPort, (char*)c.comPortToUse, c.driveCable);
	case 2:
		return new SupercardProDiskBridge(c.bridgeMode, c.bridgeDensity, c.autoCache, c.smartSpeed,
		                                  c.autoDetectComPort, (char*)c.comPortToUse,
		                                  c.driveCable == FloppyBridge::DriveSelection::dsDriveB);
	case 3:
		// XCopy Standalone, streaming: no bridge mode, no cache, no smart speed -
		// it has none of them by design. driveCable is passed for signature
		// symmetry and ignored - the board has one drive on its own cable. See
		// AdmXCopyBridge.h. (Index 3 was the streaming Greaseweazle driver until
		// 2026-09-02; removed, docs/floppy.md section 12.)
		return new AdmXCopyBridge(c.bridgeDensity, c.autoDetectComPort,
		                          (char*)c.comPortToUse, c.driveCable);
	case 4:
		// XCopy Standalone, buffered: a CommonBridgeTemplate like drivers 0-2, so
		// bridge mode, auto-cache and smart speed all apply. One drive on its own
		// cable, so no driveCable. See AdmXCopyBufferedBridge.h.
		return new AdmXCopyBufferedBridge(c.bridgeMode, c.bridgeDensity, c.autoCache, c.smartSpeed,
		                                  c.autoDetectComPort, (char*)c.comPortToUse, c.allowWrites);
	default:
		return nullptr;
	}
}

}  // namespace

#define STATE (*(BridgeState*)m_handle)

///////////////////////////////////////////////////////////////////////////////
// Static API
///////////////////////////////////////////////////////////////////////////////

// Compiled in, so always available. Upstream this reported whether the DLL was found.
const bool FloppyBridgeAPI::isAvailable()
{
	return true;
}

bool FloppyBridgeAPI::getBridgeDriverInformation(bool allowCheckForUpdates, BridgeInformation& bridgeInformation)
{
	// allowCheckForUpdates is accepted and ignored: see the note at the top of
	// this file about the DNS version lookup.
	(void)allowCheckForUpdates;

	setTCharString(bridgeInformation.about,
	               "FloppyBridge, Copyright (C) 2021-2024 RobSmithDev - compiled in");
	setTCharString(bridgeInformation.url, "https://amiga.robsmithdev.co.uk/winuae");
	bridgeInformation.majorVersion = 1;
	bridgeInformation.minorVersion = 6;
	bridgeInformation.isBeta = false;
	bridgeInformation.isUpdateAvailable = false;
	bridgeInformation.updateMajorVersion = 0;
	bridgeInformation.updateMinorVersion = 0;
	return true;
}

FloppyBridgeAPI* FloppyBridgeAPI::createDriver(unsigned int driverIndex)
{
	if (driverIndex >= MAX_NUM_DRIVERS) return nullptr;

	BridgeState* state = new BridgeState();
	state->config.bridgeIndex = driverIndex;
	state->config.autoDetectComPort = true;

	return new FloppyBridgeAPI(driverIndex, (BridgeDriverHandle)state);
}

FloppyBridgeAPI* FloppyBridgeAPI::createDriverFromString(const char* config)
{
	if (!config) return nullptr;

	// Peek at the driver index first so we create the right class, then let
	// setConfigFromString do the full parse and validation.
	BridgeConfig probe;
	if (!probe.fromString(config)) return nullptr;

	FloppyBridgeAPI* api = createDriver(probe.bridgeIndex);
	if (!api) return nullptr;

	if (!api->setConfigFromString((char*)config)) {
		delete api;
		return nullptr;
	}

	return api;
}

// Stub. See the note at the top of this file - disk.c only reaches this after
// createDriverFromString() has already failed.
FloppyBridgeAPI* FloppyBridgeAPI::createDriverFromProfileID(unsigned int profileID)
{
	(void)profileID;
	return nullptr;
}

void FloppyBridgeAPI::getDriverList(std::vector<DriverInformation>& driverList)
{
	driverList.clear();

	for (unsigned int index = 0; index < MAX_NUM_DRIVERS; index++) {
		const BridgeDriver* driver = staticDriverInfo(index);
		if (!driver) continue;

		DriverInformation info;
		info.driverIndex = index;
		setTCharString(info.name, driver->name);
		setTCharString(info.url, driver->url);
		setTCharString(info.manufacturer, driver->manufacturer);
		setTCharString(info.driverAuthor, driver->driverAuthor);
		info.configOptions = driver->configOptions;
		driverList.push_back(info);
	}
}

void FloppyBridgeAPI::enumCOMPorts(std::vector<const TCHAR*>& portList)
{
	portList.clear();

	SerialIO::enumSerialPorts(g_serialPorts);

	// Convert once and keep the strings alive; the vector is only rebuilt here,
	// which is exactly the lifetime the header documents.
	g_serialPortNames.clear();
	g_serialPortNames.reserve(g_serialPorts.size());
	for (const SerialIO::SerialPortInformation& port : g_serialPorts) {
		std::string name;
		quickw2a(port.portName, name);
		g_serialPortNames.push_back(name);
	}

	portList.reserve(g_serialPortNames.size());
	for (const std::string& name : g_serialPortNames) portList.push_back(name.c_str());
}

///////////////////////////////////////////////////////////////////////////////
// Profile management - all stubbed, see the note at the top of this file
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
bool FloppyBridgeAPI::showProfileConfigDialog(HWND hwndParent, unsigned int* profileID)
{
	(void)hwndParent;
	(void)profileID;
	return false;
}
#endif

bool FloppyBridgeAPI::getAllProfiles(std::vector<FloppyBridgeProfileInformation>& profileList)
{
	profileList.clear();
	return false;
}

bool FloppyBridgeAPI::importProfilesFromString(const char* profilesString)
{
	(void)profilesString;
	return false;
}

bool FloppyBridgeAPI::exportProfilesToString(char** profilesString)
{
	if (profilesString) *profilesString = nullptr;
	return false;
}

bool FloppyBridgeAPI::getProfileConfigAsString(unsigned int profileID, char** config)
{
	(void)profileID;
	if (config) *config = nullptr;
	return false;
}

bool FloppyBridgeAPI::setProfileConfigFromString(unsigned int profileID, const char* config)
{
	(void)profileID;
	(void)config;
	return false;
}

bool FloppyBridgeAPI::setProfileName(unsigned int profileID, const char* name)
{
	(void)profileID;
	(void)name;
	return false;
}

bool FloppyBridgeAPI::createNewProfile(unsigned int driverIndex, unsigned int* profileID)
{
	(void)driverIndex;
	if (profileID) *profileID = 0;
	return false;
}

bool FloppyBridgeAPI::deleteProfile(unsigned int profileID)
{
	(void)profileID;
	return false;
}

///////////////////////////////////////////////////////////////////////////////
// Per-instance configuration
///////////////////////////////////////////////////////////////////////////////

bool FloppyBridgeAPI::getConfigAsString(char** config)
{
	if (!config) return false;
	STATE.config.toString(config);
	return true;
}

bool FloppyBridgeAPI::setConfigFromString(char* config)
{
	BridgeConfig parsed;
	if (!parsed.fromString(config)) return false;

	// A config string can only be applied to the driver it was written for.
	if (parsed.bridgeIndex != m_driverIndex) return false;

	// Keep the serialisation scratch buffer out of it; everything else is data.
	parsed.lastSerialise[0] = '\0';
	STATE.config = parsed;

	// Bridge mode and density can be changed while running.
	if (STATE.bridge) {
		if (STATE.buffered) {
			STATE.buffered->changeBridgeMode(STATE.config.bridgeMode);
			STATE.buffered->changeBridgeDensity(STATE.config.bridgeDensity);
		}
	}

	return true;
}

bool FloppyBridgeAPI::getBridgeMode(BridgeMode* mode)
{
	if (!mode) return false;
	*mode = (BridgeMode)STATE.config.bridgeMode;
	return true;
}

bool FloppyBridgeAPI::setBridgeMode(BridgeMode newMode)
{
	if ((unsigned char)newMode > (unsigned char)FloppyBridge::BridgeMode::bmMax) return false;
	STATE.config.bridgeMode = (FloppyBridge::BridgeMode)newMode;
	if (STATE.buffered) STATE.buffered->changeBridgeMode(STATE.config.bridgeMode);
	return true;
}

bool FloppyBridgeAPI::getBridgeDensityMode(BridgeDensityMode* mode)
{
	if (!mode) return false;
	*mode = (BridgeDensityMode)STATE.config.bridgeDensity;
	return true;
}

bool FloppyBridgeAPI::setBridgeDensityMode(BridgeDensityMode newMode)
{
	if ((unsigned char)newMode > (unsigned char)FloppyBridge::BridgeDensityMode::bdmMax) return false;
	STATE.config.bridgeDensity = (FloppyBridge::BridgeDensityMode)newMode;
	if (STATE.buffered) STATE.buffered->changeBridgeDensity(STATE.config.bridgeDensity);
	return true;
}

bool FloppyBridgeAPI::getAutoCacheMode(bool* autoCacheMode)
{
	if (!autoCacheMode) return false;
	*autoCacheMode = STATE.config.autoCache;
	return true;
}

// Only read when the bridge is created, so this has no effect while running.
bool FloppyBridgeAPI::setAutoCacheMode(bool autoCacheMode)
{
	STATE.config.autoCache = autoCacheMode;
	return true;
}

bool FloppyBridgeAPI::getComPort(TCharString* comPort)
{
	if (!comPort) return false;
	setTCharString(*comPort, STATE.config.comPortToUse);
	return true;
}

bool FloppyBridgeAPI::setComPort(TCHAR* comPort)
{
	if (!comPort) return false;
	std::snprintf(STATE.config.comPortToUse, sizeof(STATE.config.comPortToUse), "%s", comPort);
	return true;
}

bool FloppyBridgeAPI::getComPortAutoDetect(bool* autoDetect)
{
	if (!autoDetect) return false;
	*autoDetect = STATE.config.autoDetectComPort;
	return true;
}

bool FloppyBridgeAPI::setComPortAutoDetect(bool autoDetect)
{
	STATE.config.autoDetectComPort = autoDetect;
	return true;
}

bool FloppyBridgeAPI::getDriveCableSelection(bool* connectToDriveB)
{
	if (!connectToDriveB) return false;
	*connectToDriveB = STATE.config.driveCable == FloppyBridge::DriveSelection::dsDriveB;
	return true;
}

bool FloppyBridgeAPI::setDriveCableSelection(bool connectToDriveB)
{
	STATE.config.driveCable = connectToDriveB ? FloppyBridge::DriveSelection::dsDriveB
	                                          : FloppyBridge::DriveSelection::dsDriveA;
	return true;
}

bool FloppyBridgeAPI::getSmartSpeedEnabled(bool* enabled)
{
	if (!enabled) return false;
	*enabled = STATE.config.smartSpeed;
	return true;
}

bool FloppyBridgeAPI::setSmartSpeedEnabled(bool enabled)
{
	STATE.config.smartSpeed = enabled;
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Lifetime
///////////////////////////////////////////////////////////////////////////////

FloppyBridgeAPI::FloppyBridgeAPI(unsigned int driverIndex, BridgeDriverHandle handle)
	: m_handle(handle), m_driverIndex(driverIndex),
	  m_driverInfo((BridgeDriver*)staticDriverInfo(driverIndex))
{
	m_error[0] = '\0';
	m_warning[0] = '\0';
}

FloppyBridgeAPI::~FloppyBridgeAPI()
{
	shutdown();
	delete (BridgeState*)m_handle;
	m_handle = nullptr;
}

bool FloppyBridgeAPI::initialise()
{
	// disk.c can call this again on a re-attach without an intervening shutdown.
	shutdown();

	m_error[0] = '\0';
	m_warning[0] = '\0';

	STATE.bridge = makeBridge(STATE.config);
	// Driver 3 is the streaming bridge and is not a CommonBridgeTemplate; a
	// static_cast of it to one was a latent bug until driver 4 was added.
	STATE.buffered = (STATE.config.bridgeIndex == 3)
					 ? nullptr
					 : static_cast<CommonBridgeTemplate*>(STATE.bridge);
	if (!STATE.bridge) {
		setTCharString(m_error, "Unsupported FloppyBridge driver index.");
		return false;
	}

	const bool ok = STATE.bridge->initialise();

	// On success the message, if any, is a compatibility warning rather than an
	// error - the same convention upstream's BRIDGE_Open uses.
	const char* message = STATE.bridge->getLastErrorMessage();
	setTCharString(ok ? m_warning : m_error, message);

	if (!ok) {
		delete STATE.bridge;
		STATE.bridge = nullptr;
		return false;
	}

	m_isOpen = true;
	return true;
}

void FloppyBridgeAPI::shutdown()
{
	if (STATE.bridge) {
		STATE.bridge->shutdown();
		delete STATE.bridge;
		STATE.bridge = nullptr;
	}
	m_isOpen = false;
}

///////////////////////////////////////////////////////////////////////////////
// FloppyDiskBridge - forwarded to the concrete driver
//
// Every one of these must be null-safe: disk.c assigns the bridge to
// floppy[dr].bridge even when initialise() failed, and then keeps polling it.
///////////////////////////////////////////////////////////////////////////////

// Answered from the static driver table, so it is valid before initialise() and
// after a failed one - which is exactly when disk.c asks, to name the drive in
// its error message.
const FloppyDiskBridge::BridgeDriver* FloppyBridgeAPI::getDriverInfo()
{
	return staticDriverInfo(m_driverIndex);
}

const char* FloppyBridgeAPI::getLastErrorMessage()
{
	// Prefer the driver's own message while one exists: CommonBridgeTemplate keeps
	// m_lastError live as the hardware misbehaves, whereas m_error/m_warning are
	// snapshots taken at initialise(). After a *failed* initialise there is no driver
	// left and m_error is the only record of why - which is exactly when disk.c asks.
	if (STATE.bridge) {
		const char* message = STATE.bridge->getLastErrorMessage();
		if (message && message[0]) return message;
	}
	if (m_error[0]) return m_error;
	if (m_warning[0]) return m_warning;
	return "";
}

unsigned char FloppyBridgeAPI::getBitSpeed()
{
	return STATE.bridge ? STATE.bridge->getBitSpeed() : 2;
}

FloppyDiskBridge::DriveTypeID FloppyBridgeAPI::getDriveTypeID()
{
	return STATE.bridge ? STATE.bridge->getDriveTypeID() : DriveTypeID::dti35DD;
}

bool FloppyBridgeAPI::resetDrive(int trackNumber)
{
	return STATE.bridge ? STATE.bridge->resetDrive(trackNumber) : false;
}

bool FloppyBridgeAPI::isAtCylinder0()
{
	return STATE.bridge ? STATE.bridge->isAtCylinder0() : false;
}

unsigned char FloppyBridgeAPI::getMaxCylinder()
{
	return STATE.bridge ? STATE.bridge->getMaxCylinder() : 80;
}

void FloppyBridgeAPI::gotoCylinder(int cylinderNumber, bool side)
{
	if (STATE.bridge) STATE.bridge->gotoCylinder(cylinderNumber, side);
}

void FloppyBridgeAPI::handleNoClickStep(bool side)
{
	if (STATE.bridge) STATE.bridge->handleNoClickStep(side);
}

unsigned char FloppyBridgeAPI::getCurrentCylinderNumber()
{
	return STATE.bridge ? STATE.bridge->getCurrentCylinderNumber() : 0;
}

bool FloppyBridgeAPI::isMotorRunning()
{
	return STATE.bridge ? STATE.bridge->isMotorRunning() : false;
}

void FloppyBridgeAPI::setMotorStatus(bool side, bool turnOn)
{
	if (STATE.bridge) STATE.bridge->setMotorStatus(side, turnOn);
}

bool FloppyBridgeAPI::isReady()
{
	return STATE.bridge ? STATE.bridge->isReady() : false;
}

bool FloppyBridgeAPI::isDiskInDrive()
{
	return STATE.bridge ? STATE.bridge->isDiskInDrive() : false;
}

// No bridge means no disk, and "changed" is what disk.c reads as "empty".
bool FloppyBridgeAPI::hasDiskChanged()
{
	return STATE.bridge ? STATE.bridge->hasDiskChanged() : true;
}

bool FloppyBridgeAPI::isStillWorking()
{
	return STATE.bridge ? STATE.bridge->isStillWorking() : false;
}

bool FloppyBridgeAPI::getCurrentSide()
{
	return STATE.bridge ? STATE.bridge->getCurrentSide() : false;
}

bool FloppyBridgeAPI::isMFMPositionAtIndex(int mfmPositionBits)
{
	return STATE.bridge ? STATE.bridge->isMFMPositionAtIndex(mfmPositionBits) : false;
}

bool FloppyBridgeAPI::isMFMDataAvailable()
{
	return STATE.bridge ? STATE.bridge->isMFMDataAvailable() : false;
}

bool FloppyBridgeAPI::getMFMBit(const int mfmPositionBits)
{
	return STATE.bridge ? STATE.bridge->getMFMBit(mfmPositionBits) : false;
}

int FloppyBridgeAPI::getMFMTrack(bool side, unsigned int track, bool resyncRotation,
                                 const int bufferSizeInBytes, void* output)
{
	return STATE.bridge
	           ? STATE.bridge->getMFMTrack(side, track, resyncRotation, bufferSizeInBytes, output)
	           : 0;
}

bool FloppyBridgeAPI::writeMFMTrackToBuffer(bool side, unsigned int track, bool writeFromIndex,
                                            int sizeInBytes, void* mfmData)
{
	return STATE.bridge
	           ? STATE.bridge->writeMFMTrackToBuffer(side, track, writeFromIndex, sizeInBytes, mfmData)
	           : false;
}

bool FloppyBridgeAPI::setDirectMode(bool directModeEnable)
{
	return STATE.bridge ? STATE.bridge->setDirectMode(directModeEnable) : false;
}

// 1000 = 100% speed, which is the neutral answer when there is no drive.
int FloppyBridgeAPI::getMFMSpeed(const int mfmPositionBits)
{
	return STATE.bridge ? STATE.bridge->getMFMSpeed(mfmPositionBits) : 1000;
}

void FloppyBridgeAPI::mfmSwitchBuffer(bool side)
{
	if (STATE.bridge) STATE.bridge->mfmSwitchBuffer(side);
}

void FloppyBridgeAPI::setSurface(bool side)
{
	if (STATE.bridge) STATE.bridge->setSurface(side);
}

// disk.c divides by this, so it must never be zero. A standard DD track.
int FloppyBridgeAPI::maxMFMBitPosition()
{
	return STATE.bridge ? STATE.bridge->maxMFMBitPosition() : 100000;
}

void FloppyBridgeAPI::writeShortToBuffer(bool side, unsigned int track, unsigned short mfmData,
                                         int mfmPosition)
{
	if (STATE.bridge) STATE.bridge->writeShortToBuffer(side, track, mfmData, mfmPosition);
}

// Without a drive, refuse writes rather than silently swallowing them.
bool FloppyBridgeAPI::isWriteProtected()
{
	return STATE.bridge ? STATE.bridge->isWriteProtected() : true;
}

unsigned int FloppyBridgeAPI::commitWriteBuffer(bool side, unsigned int track)
{
	return STATE.bridge ? STATE.bridge->commitWriteBuffer(side, track)
	                    : (unsigned int)maxMFMBitPosition();
}

bool FloppyBridgeAPI::isWritePending()
{
	return STATE.bridge ? STATE.bridge->isWritePending() : false;
}

bool FloppyBridgeAPI::isWriteComplete()
{
	return STATE.bridge ? STATE.bridge->isWriteComplete() : false;
}

bool FloppyBridgeAPI::canTurboWrite()
{
	return STATE.bridge ? STATE.bridge->canTurboWrite() : false;
}

bool FloppyBridgeAPI::isReadyToWrite()
{
	return STATE.bridge ? STATE.bridge->isReadyToWrite() : false;
}
