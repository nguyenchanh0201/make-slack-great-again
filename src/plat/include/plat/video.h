#pragma once

#include "plat/plat.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace plat::video {

class VideoPlayer {
public:
    static std::unique_ptr<VideoPlayer> create(plat::Window *window);
    virtual ~VideoPlayer() = default;

    virtual void load(const std::string &pathOrUrl) = 0;
    virtual void play()                             = 0;
    virtual void pause()                            = 0;
    virtual void seek(int64_t ms)                   = 0;
    virtual void setMuted(bool mute)                = 0;
    virtual void setVolume(float volume)            = 0; // 0..1
    virtual void setFrame(plat::Rect bounds)        = 0;
    virtual void setVisible(bool visible)           = 0;

    virtual int64_t positionMs() const              = 0;
    virtual int64_t durationMs() const              = 0;
    virtual bool    isPlaying() const               = 0;

    std::function<void()>           onLoaded;
    std::function<void(int64_t ms)> onPosition;
    std::function<void()>           onEnded;
    std::function<void(int err)>    onFailed;
};

} // namespace plat::video
