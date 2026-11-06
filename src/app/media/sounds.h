// Notification sounds: the chime bundled in
// the binary plus the OS's own sounds (plat/audio.h), chosen in Settings →
// Notifications and played when a notification fires.
//
// A sound's id is the persisted token (Settings::soundId): "bundled:<name>"
// for a sound shipped in the app, "system:<native>" for an OS sound.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace sounds {

struct Entry {
    std::string id;
    std::string label; // for the Settings dropdown
};

// The id used when none is configured, or when a stored one won't resolve.
inline constexpr const char *kDefaultId = "bundled:notify";

// The sounds bundled with the app (just the chime), labels translated.
std::vector<Entry> bundled();

// The OS's sounds, enumerated on a worker thread (Linux asks gsettings, a
// fresh list per call so a newly added sound shows up); done(list) on the UI
// thread, never inside this call.
void systemSounds(plat::App &app, std::function<void(std::vector<Entry>)> done);

// Native macOS notification resource name; no playback or filesystem access.
// Unknown/empty ids use the bundled chime; system ids retain their native name.
std::string notificationSound(const std::string &id);

// Legacy callers' default. macOS delivery sets silent from the sound setting
// and attaches notificationSound(); previews still use play().
#ifdef __APPLE__
inline constexpr bool kSilentNotifications = true;
#else
inline constexpr bool kSilentNotifications = false;
#endif

// Plays the sound `id` (empty or unresolvable: the default chime, as is a
// system sound that fails to play). Fire and forget, from the UI thread.
void play(plat::App &app, const std::string &id);

} // namespace sounds
