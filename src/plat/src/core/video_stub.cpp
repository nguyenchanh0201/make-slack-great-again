#include "plat/video.h"

#ifndef PLAT_HAS_COCOA
namespace plat::video {

std::unique_ptr<VideoPlayer> VideoPlayer::create(plat::Window *) {
    return nullptr;
}

} // namespace plat::video
#endif
