// The files the user downloads, as the downloads tray shows them: each one's
// name, where it goes, its progress and how it ended. UI-thread only, like
// Jobs (jobs.h); the screens register a download from the click:
//
//   const int id = model::downloads().begin("report.pdf", "/…/Downloads/report.pdf");
//   … fetchFile(…, done: downloads().finish(id, ok, err),
//                  progress: downloads().progress(id, received, total));
#pragma once

#include "base/observers.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace model {

class Downloads {
public:
    enum class State : uint8_t { Running, Paused, Done, Failed };
    struct Item {
        int         id = 0;
        std::string name;   // as shown ("report.pdf")
        std::string path;   // where it is written
        std::string kind;   // its type as the service names it ("Markdown (raw)"); may be ""
        std::string source; // what is fetched (a URL or a local file), for a resume
        int64_t     received = 0, total = 0; // bytes; total 0 = unknown
        State       state = State::Running;
        std::string error; // Failed: why
    };

    int  begin(std::string name, std::string path, std::string kind = {}, std::string source = {});
    // These are safe with an unknown or removed id. pause: a running one
    // stops where it is; restart: a paused or failed one runs again from
    // the start (false when it can't).
    bool pause(int id);
    bool restart(int id);
    void                     progress(int id, int64_t received, int64_t total);
    void                     finish(int id, bool ok, std::string error = {});
    void                     remove(int id); // dismissed from the tray
    // Oldest first.
    const std::vector<Item> &items() const { return _items; }
    const Item              *find(int id) const;

    // After every change. Returns an id for unobserve().
    using ObserverId = base::Observers::Id;
    ObserverId observe(std::function<void()> fn);
    void       unobserve(ObserverId id) { _observers.remove(id); }

private:
    Item             *get(int id);
    std::vector<Item> _items;
    base::Observers   _observers;
    int               _nextId = 1;
};

// The app's one list.
Downloads &downloads();

} // namespace model
