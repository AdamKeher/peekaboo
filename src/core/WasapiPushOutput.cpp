#include "WasapiPushOutput.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Defines the device-enumeration and audio-client GUIDs in this translation unit; it is the only
// one that uses them.
#include <initguid.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstring>

namespace adm::core {

namespace {

template <typename T> void Release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string HrText(const char* what, HRESULT hr)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

std::wstring DefaultDeviceId(IMMDeviceEnumerator* enumerator)
{
    std::wstring id;
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(device->GetId(&raw)) && raw) {
            id = raw;
            CoTaskMemFree(raw);
        }
        device->Release();
    }
    return id;
}

bool DeviceLost(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED;
}

} // namespace

struct WasapiPushOutput::Impl
{
    bool comInitialized = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    std::wstring deviceId;
    uint32_t bufferFrames = 0;
    uint32_t enginePeriodFrames = 0;
    uint32_t deviceRate = 0;
    bool started = false;
};

WasapiPushOutput::WasapiPushOutput() : m_impl(std::make_unique<Impl>()) {}

WasapiPushOutput::~WasapiPushOutput() { Close(); }

bool WasapiPushOutput::Open(uint32_t sampleRate, uint32_t bufferMs, std::string& error)
{
    Close();
    Impl& d = *m_impl;

    // The main thread may already be in a single-threaded apartment (the file dialogs put it
    // there). That is fine for these interfaces; only a call that succeeded is balanced later.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    d.comInitialized = SUCCEEDED(co);

    HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&d.enumerator));
    if (FAILED(hr)) {
        error = HrText("CoCreateInstance(MMDeviceEnumerator)", hr);
        Close();
        return false;
    }

    IMMDevice* device = nullptr;
    hr = d.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        error = HrText("GetDefaultAudioEndpoint", hr);
        Close();
        return false;
    }
    LPWSTR rawId = nullptr;
    if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
        d.deviceId = rawId;
        CoTaskMemFree(rawId);
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&d.client));
    device->Release();
    if (FAILED(hr)) {
        error = HrText("IMMDevice::Activate(IAudioClient)", hr);
        Close();
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(d.client->GetMixFormat(&mix)) && mix) {
        d.deviceRate = mix->nSamplesPerSec;
        CoTaskMemFree(mix);
    }

    // The core's own format; the engine converts it to the device's and resamples.
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = sampleRate * 4;

    // No event callback: the producer writes whenever the core delivers, and polls the queue.
    const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
    hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              duration, 0, &format, nullptr);
    if (FAILED(hr)) {
        error = HrText("IAudioClient::Initialize", hr);
        Close();
        return false;
    }

    UINT32 frames = 0;
    d.client->GetBufferSize(&frames);
    d.bufferFrames = frames;

    REFERENCE_TIME period = 0;
    REFERENCE_TIME minimum = 0;
    if (SUCCEEDED(d.client->GetDevicePeriod(&period, &minimum)))
        d.enginePeriodFrames = static_cast<uint32_t>(period * sampleRate / 10000000);

    hr = d.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&d.render));
    if (FAILED(hr)) {
        error = HrText("GetService(IAudioRenderClient)", hr);
        Close();
        return false;
    }

    // Running with nothing queued is fine: a starved shared-mode stream plays silence.
    hr = d.client->Start();
    if (FAILED(hr)) {
        error = HrText("IAudioClient::Start", hr);
        Close();
        return false;
    }
    d.started = true;
    return true;
}

void WasapiPushOutput::Close()
{
    Impl& d = *m_impl;
    if (d.client && d.started)
        d.client->Stop();
    d.started = false;
    Release(d.render);
    Release(d.client);
    Release(d.enumerator);
    d.deviceId.clear();
    d.bufferFrames = 0;
    d.enginePeriodFrames = 0;
    if (d.comInitialized) {
        CoUninitialize();
        d.comInitialized = false;
    }
}

bool WasapiPushOutput::IsOpen() const { return m_impl->render != nullptr; }

bool WasapiPushOutput::Queued(uint32_t& frames)
{
    frames = 0;
    if (!m_impl->client)
        return false;
    UINT32 padding = 0;
    const HRESULT hr = m_impl->client->GetCurrentPadding(&padding);
    if (FAILED(hr))
        return !DeviceLost(hr);
    frames = padding;
    return true;
}

bool WasapiPushOutput::Write(const int16_t* frames, uint32_t count, uint32_t& written)
{
    written = 0;
    Impl& d = *m_impl;
    if (!d.render)
        return false;
    uint32_t queued = 0;
    if (!Queued(queued))
        return false;
    const uint32_t room = d.bufferFrames > queued ? d.bufferFrames - queued : 0;
    const uint32_t n = count < room ? count : room;
    if (n == 0)
        return true;
    BYTE* out = nullptr;
    HRESULT hr = d.render->GetBuffer(n, &out);
    if (FAILED(hr))
        return !DeviceLost(hr);
    if (frames)
        std::memcpy(out, frames, static_cast<size_t>(n) * 4);
    hr = d.render->ReleaseBuffer(n, frames ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr))
        return !DeviceLost(hr);
    written = n;
    return true;
}

void WasapiPushOutput::Flush()
{
    Impl& d = *m_impl;
    if (!d.client)
        return;
    // Reset needs a stopped stream.
    d.client->Stop();
    d.client->Reset();
    if (SUCCEEDED(d.client->Start()))
        d.started = true;
}

uint32_t WasapiPushOutput::BufferFrames() const { return m_impl->bufferFrames; }

uint32_t WasapiPushOutput::EnginePeriodFrames() const { return m_impl->enginePeriodFrames; }

uint32_t WasapiPushOutput::DeviceSampleRate() const { return m_impl->deviceRate; }

bool WasapiPushOutput::DefaultDeviceChanged()
{
    Impl& d = *m_impl;
    if (!d.enumerator)
        return false;
    const std::wstring now = DefaultDeviceId(d.enumerator);
    return !now.empty() && now != d.deviceId;
}

} // namespace adm::core

#else // !_WIN32

namespace adm::core {

struct WasapiPushOutput::Impl
{
};

WasapiPushOutput::WasapiPushOutput() : m_impl(std::make_unique<Impl>()) {}
WasapiPushOutput::~WasapiPushOutput() = default;

bool WasapiPushOutput::Open(uint32_t, uint32_t, std::string& error)
{
    error = "WASAPI is Windows-only";
    return false;
}
void WasapiPushOutput::Close() {}
bool WasapiPushOutput::IsOpen() const { return false; }
bool WasapiPushOutput::Queued(uint32_t& frames)
{
    frames = 0;
    return false;
}
bool WasapiPushOutput::Write(const int16_t*, uint32_t, uint32_t& written)
{
    written = 0;
    return false;
}
void WasapiPushOutput::Flush() {}
uint32_t WasapiPushOutput::BufferFrames() const { return 0; }
uint32_t WasapiPushOutput::EnginePeriodFrames() const { return 0; }
uint32_t WasapiPushOutput::DeviceSampleRate() const { return 0; }
bool WasapiPushOutput::DefaultDeviceChanged() { return false; }

} // namespace adm::core

#endif
