#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "../protocol/Protocol.h"

namespace dd::media
{

// Windows volume and media playback state for the client's dock. A thread of its own polls the
// default playback device's volume and the current media session (the one the play/pause key
// controls) a few times a second, and reports every change. Polling instead of change events
// keeps up with default-device switches and apps coming and going at no measurable cost.
class MediaControls
{
  public:
    using Listener = std::function<void(const protocol::MediaState&)>;

    // `listener` runs on the controls' thread: once right away, then on every change.
    explicit MediaControls(Listener listener);
    ~MediaControls();
    MediaControls(const MediaControls&) = delete;
    MediaControls& operator=(const MediaControls&) = delete;

    // One step (+1 up, -1 down) of the Windows volume. Stepping down to zero mutes, stepping up
    // unmutes. Queued: returns at once, and the new state is reported right after.
    void StepVolume(int direction);

  private:
    void Run();

    Listener m_listener;
    std::mutex m_lock;
    std::condition_variable m_wake;
    bool m_stop = false;
    int m_pendingSteps = 0;
    std::thread m_thread;
};

} // namespace dd::media
