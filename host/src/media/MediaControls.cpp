#include "MediaControls.h"

#include <windows.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <windows.media.control.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "../common/Log.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace Control = ABI::Windows::Media::Control;
namespace Foundation = ABI::Windows::Foundation;

namespace dd::media
{
namespace
{

constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr int kVolumeSteps = 20; // 5 % per press, close to Android's own volume steps

using ManagerOperation = Foundation::IAsyncOperation<Control::GlobalSystemMediaTransportControlsSessionManager*>;

// The session manager (Windows 10 1809+). Null if unavailable: the dock then shows no media state.
ComPtr<Control::IGlobalSystemMediaTransportControlsSessionManager> RequestSessionManager()
{
    ComPtr<Control::IGlobalSystemMediaTransportControlsSessionManagerStatics> statics;
    ComPtr<ManagerOperation> operation;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager).Get(),
        IID_PPV_ARGS(&statics));
    if (SUCCEEDED(hr))
    {
        hr = statics->RequestAsync(&operation);
    }
    ComPtr<Foundation::IAsyncInfo> info;
    if (SUCCEEDED(hr))
    {
        hr = operation.As(&info);
    }
    // Poll rather than register a completion handler, so nothing outlives this function if the
    // request never finishes.
    Foundation::AsyncStatus status = Foundation::AsyncStatus::Started;
    for (int i = 0; SUCCEEDED(hr) && i < 500; ++i)
    {
        hr = info->get_Status(&status);
        if (status != Foundation::AsyncStatus::Started)
        {
            break;
        }
        Sleep(10);
    }
    ComPtr<Control::IGlobalSystemMediaTransportControlsSessionManager> manager;
    if (SUCCEEDED(hr) && status == Foundation::AsyncStatus::Completed)
    {
        hr = operation->GetResults(&manager);
    }
    if (FAILED(hr) || !manager)
    {
        Log(L"media: no media session manager (0x%08lX, status %d); play/pause state is unavailable", Hr(hr),
            static_cast<int>(status));
        return nullptr;
    }
    return manager;
}

protocol::Playback ReadPlayback(Control::IGlobalSystemMediaTransportControlsSessionManager* manager)
{
    ComPtr<Control::IGlobalSystemMediaTransportControlsSession> session;
    ComPtr<Control::IGlobalSystemMediaTransportControlsSessionPlaybackInfo> info;
    auto status = Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Closed;
    if (!manager || FAILED(manager->GetCurrentSession(&session)) || !session ||
        FAILED(session->GetPlaybackInfo(&info)) || FAILED(info->get_PlaybackStatus(&status)))
    {
        return protocol::Playback::None;
    }
    return status == Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing
               ? protocol::Playback::Playing
               : protocol::Playback::Paused;
}

// The default playback device's volume control, looked up each time so a switch to headphones or
// another device is followed.
ComPtr<IAudioEndpointVolume> DefaultEndpointVolume(IMMDeviceEnumerator* enumerator)
{
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume)))
    {
        return nullptr;
    }
    return volume;
}

void Step(IAudioEndpointVolume* volume, int direction)
{
    float level = 0;
    BOOL muted = FALSE;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&level)) || FAILED(volume->GetMute(&muted)))
    {
        return;
    }
    const int current = static_cast<int>(std::lround(level * kVolumeSteps));
    const int next = std::clamp(current + direction, 0, kVolumeSteps);
    volume->SetMasterVolumeLevelScalar(static_cast<float>(next) / kVolumeSteps, nullptr);
    const BOOL mute = next == 0 ? TRUE : FALSE;
    if (mute != muted)
    {
        volume->SetMute(mute, nullptr);
    }
}

} // namespace

MediaControls::MediaControls(Listener listener) : m_listener(std::move(listener))
{
    m_thread = std::thread([this] { Run(); });
}

MediaControls::~MediaControls()
{
    {
        std::lock_guard lock(m_lock);
        m_stop = true;
    }
    m_wake.notify_one();
    m_thread.join();
}

void MediaControls::StepVolume(int direction)
{
    {
        std::lock_guard lock(m_lock);
        m_pendingSteps += direction > 0 ? 1 : -1;
    }
    m_wake.notify_one();
}

void MediaControls::Run()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
            Log(L"media: no audio device enumerator; volume is unavailable");
        }
        const auto manager = RequestSessionManager();

        bool first = true;
        protocol::MediaState last;
        std::unique_lock lock(m_lock);
        while (!m_stop)
        {
            const int steps = std::exchange(m_pendingSteps, 0);
            lock.unlock();

            protocol::MediaState state;
            if (const auto volume = DefaultEndpointVolume(enumerator.Get()))
            {
                for (int i = 0; i < std::abs(steps); ++i)
                {
                    Step(volume.Get(), steps > 0 ? 1 : -1);
                }
                float level = 0;
                BOOL muted = FALSE;
                if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level)) && SUCCEEDED(volume->GetMute(&muted)))
                {
                    state.volumePercent = static_cast<uint8_t>(std::clamp(std::lround(level * 100), 0L, 100L));
                    state.muted = muted != FALSE;
                }
            }
            state.playback = ReadPlayback(manager.Get());
            if (first || state != last)
            {
                first = false;
                last = state;
                m_listener(state);
            }

            lock.lock();
            m_wake.wait_for(lock, kPollInterval, [this] { return m_stop || m_pendingSteps != 0; });
        }
    }
    if (SUCCEEDED(coInit))
    {
        CoUninitialize();
    }
}

} // namespace dd::media
