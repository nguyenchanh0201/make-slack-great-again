#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>

#include "plat/video.h"
#include "cocoa/cocoa_internal.h"

#include <algorithm>
#include <cmath>

@class CocoaVideoObserver;

namespace plat::video {

class CocoaVideoPlayer final : public VideoPlayer {
public:
    CocoaVideoPlayer(plat::Window *window);
    ~CocoaVideoPlayer() override;

    void load(const std::string &pathOrUrl) override;
    void play() override;
    void pause() override;
    void seek(int64_t ms) override;
    void setMuted(bool mute) override;
    void setVolume(float volume) override;
    void setFrame(plat::Rect bounds) override;
    void setVisible(bool visible) override;

    int64_t positionMs() const override;
    int64_t durationMs() const override;
    bool    isPlaying() const override;

    void handleReadyToPlay();
    void handleFailed(int err);
    void handleEnded();
    void handlePeriodicTime(CMTime time);

private:
    void cleanupItemObservers();

    plat::Window       *_window = nullptr;
    AVPlayer           *_player = nil;
    AVPlayerLayer      *_playerLayer = nil;
    AVPlayerItem       *_currentItem = nil;
    CocoaVideoObserver *_observer = nil;
    id                  _timeObserver = nil;
    bool                _isPlaying = false;
    int64_t             _lastKnownDurationMs = 0;
};

} // namespace plat::video

@interface CocoaVideoObserver : NSObject
@property(nonatomic, assign) plat::video::CocoaVideoPlayer *owner;
- (void)itemDidPlayToEnd:(NSNotification *)note;
@end

@implementation CocoaVideoObserver

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    if (!_owner)
        return;
    if ([keyPath isEqualToString:@"status"]) {
        AVPlayerItem *item = (AVPlayerItem *)object;
        if (item.status == AVPlayerItemStatusReadyToPlay) {
            _owner->handleReadyToPlay();
        } else if (item.status == AVPlayerItemStatusFailed) {
            _owner->handleFailed(1);
        }
    }
}

- (void)itemDidPlayToEnd:(NSNotification *)note {
    if (_owner) {
        _owner->handleEnded();
    }
}

@end

namespace plat::video {

CocoaVideoPlayer::CocoaVideoPlayer(plat::Window *window) : _window(window) {
    _observer = [[CocoaVideoObserver alloc] init];
    _observer.owner = this;

    _player = [[AVPlayer alloc] init];
    _player.actionAtItemEnd = AVPlayerActionAtItemEndPause;

    _playerLayer = [AVPlayerLayer playerLayerWithPlayer:_player];
    _playerLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    _playerLayer.hidden = YES;

    if (_window) {
        NSWindow *nswin = (__bridge NSWindow *)_window->nativeHandle();
        if (nswin && nswin.contentView) {
            NSView *contentView = nswin.contentView;
            if (!contentView.layer)
                contentView.wantsLayer = YES;
            [contentView.layer addSublayer:_playerLayer];
        }
    }

    __weak CocoaVideoObserver *weakObs = _observer;
    CocoaVideoPlayer *selfPtr = this;
    CMTime interval = CMTimeMakeWithSeconds(0.05, 600);
    _timeObserver = [_player addPeriodicTimeObserverForInterval:interval
                                                          queue:dispatch_get_main_queue()
                                                     usingBlock:^(CMTime time) {
        if (weakObs.owner == selfPtr) {
            selfPtr->handlePeriodicTime(time);
        }
    }];
}

CocoaVideoPlayer::~CocoaVideoPlayer() {
    _observer.owner = nullptr;
    if (_timeObserver) {
        [_player removeTimeObserver:_timeObserver];
        _timeObserver = nil;
    }
    cleanupItemObservers();
    [_player pause];
    [_player replaceCurrentItemWithPlayerItem:nil];
    [_playerLayer removeFromSuperlayer];
    _playerLayer = nil;
    _player = nil;
    _observer = nil;
}

void CocoaVideoPlayer::cleanupItemObservers() {
    if (_currentItem) {
        [[NSNotificationCenter defaultCenter] removeObserver:_observer
                                                        name:AVPlayerItemDidPlayToEndTimeNotification
                                                      object:_currentItem];
        [_currentItem removeObserver:_observer forKeyPath:@"status"];
        _currentItem = nil;
    }
}

void CocoaVideoPlayer::load(const std::string &pathOrUrl) {
    cleanupItemObservers();
    _lastKnownDurationMs = 0;
    _isPlaying = false;

    if (pathOrUrl.empty()) {
        [_player replaceCurrentItemWithPlayerItem:nil];
        return;
    }

    NSURL *url = nil;
    if (pathOrUrl.compare(0, 7, "http://") == 0 ||
        pathOrUrl.compare(0, 8, "https://") == 0 ||
        pathOrUrl.compare(0, 7, "file://") == 0) {
        url = [NSURL URLWithString:cocoa::nsString(pathOrUrl)];
    } else {
        url = [NSURL fileURLWithPath:cocoa::nsString(pathOrUrl)];
    }

    if (!url) {
        if (onFailed)
            onFailed(1);
        return;
    }

    _currentItem = [AVPlayerItem playerItemWithURL:url];
    [_currentItem addObserver:_observer
                   forKeyPath:@"status"
                      options:NSKeyValueObservingOptionNew
                      context:nullptr];
    [[NSNotificationCenter defaultCenter] addObserver:_observer
                                             selector:@selector(itemDidPlayToEnd:)
                                                 name:AVPlayerItemDidPlayToEndTimeNotification
                                               object:_currentItem];

    [_player replaceCurrentItemWithPlayerItem:_currentItem];
}

void CocoaVideoPlayer::play() {
    _isPlaying = true;
    [_player play];
}

void CocoaVideoPlayer::pause() {
    _isPlaying = false;
    [_player pause];
}

void CocoaVideoPlayer::seek(int64_t ms) {
    if (!_player)
        return;
    CMTime t = CMTimeMakeWithSeconds(double(ms) / 1000.0, 600);
    [_player seekToTime:t toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
}

void CocoaVideoPlayer::setMuted(bool mute) {
    if (_player)
        _player.muted = mute ? YES : NO;
}

void CocoaVideoPlayer::setVolume(float volume) {
    if (_player)
        _player.volume = std::clamp(volume, 0.0f, 1.0f);
}

void CocoaVideoPlayer::setFrame(plat::Rect bounds) {
    if (!_playerLayer)
        return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _playerLayer.frame = CGRectMake(bounds.x, bounds.y, bounds.w, bounds.h);
    [CATransaction commit];
}

void CocoaVideoPlayer::setVisible(bool visible) {
    if (!_playerLayer)
        return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _playerLayer.hidden = visible ? NO : YES;
    [CATransaction commit];
}

int64_t CocoaVideoPlayer::positionMs() const {
    if (!_player)
        return 0;
    CMTime t = _player.currentTime;
    if (CMTIME_IS_VALID(t) && !CMTIME_IS_INDEFINITE(t)) {
        double secs = CMTimeGetSeconds(t);
        return secs >= 0.0 ? int64_t(std::llround(secs * 1000.0)) : 0;
    }
    return 0;
}

int64_t CocoaVideoPlayer::durationMs() const {
    if (_currentItem) {
        CMTime d = _currentItem.duration;
        if (CMTIME_IS_VALID(d) && !CMTIME_IS_INDEFINITE(d)) {
            double secs = CMTimeGetSeconds(d);
            if (secs > 0.0) {
                return int64_t(std::llround(secs * 1000.0));
            }
        }
    }
    return _lastKnownDurationMs;
}

bool CocoaVideoPlayer::isPlaying() const {
    return _player && _player.rate != 0.0f && _player.error == nil;
}

void CocoaVideoPlayer::handleReadyToPlay() {
    int64_t d = durationMs();
    if (d > 0)
        _lastKnownDurationMs = d;
    if (onLoaded)
        onLoaded();
}

void CocoaVideoPlayer::handleFailed(int err) {
    _isPlaying = false;
    if (onFailed)
        onFailed(err);
}

void CocoaVideoPlayer::handleEnded() {
    _isPlaying = false;
    if (onEnded)
        onEnded();
}

void CocoaVideoPlayer::handlePeriodicTime(CMTime time) {
    if (onPosition) {
        if (CMTIME_IS_VALID(time) && !CMTIME_IS_INDEFINITE(time)) {
            double secs = CMTimeGetSeconds(time);
            onPosition(secs >= 0.0 ? int64_t(std::llround(secs * 1000.0)) : 0);
        }
    }
}

std::unique_ptr<VideoPlayer> VideoPlayer::create(plat::Window *window) {
    return std::make_unique<CocoaVideoPlayer>(window);
}

} // namespace plat::video
