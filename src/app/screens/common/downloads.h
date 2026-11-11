// Files the screens fetch for the user: the
// file bar's Download, "Copy full image", the CSV preview, Forward.
//
//   fetchFile(ctx, file.source(), path, [](bool ok, const std::string &err) {…});
//
// A local path (a pending upload, the demo, a Claude Code file) is copied on
// a worker thread; an http(s) URL goes to the backend, which downloads it
// with the service's credentials and writes it off the UI thread too. done
// runs on the UI thread, never inside the call. Register the wait with
// model::jobs() (app/model/jobs.h) around it.
#pragma once

#include "app/model/backend.h"

#include <string>
#include <string_view>

namespace plat {
class App;
}

namespace screens {

// `progress` (remote sources only) hears the transfer as it goes.
void fetchFile(
    plat::App               &app,
    model::Backend          &backend,
    const std::string       &source,
    std::string              toPath,
    model::Backend::Done     done,
    model::Backend::Progress progress = {}
);

// A free path for `name` in `dir`: "report.pdf", else "report (1).pdf", …,
// skipping files there and downloads still running to it. The name is
// reduced to its last path component ("file" when nothing is left).
std::string uniqueDownloadPath(std::string_view dir, std::string_view name);
// A download the user asked for: `source` to `toPath`, listed in
// model::downloads() (the tray: progress, then Show in Finder) and as a job
// while it runs.
void        startDownload(
    plat::App         &app,
    model::Backend    &backend,
    std::string        name,
    const std::string &source,
    std::string        toPath
);

// A fresh path for a downloaded copy of `name` under the OS temp directory
// (<Temp>/msga/downloads/<random>/<name>, the directory created), so
// a viewer or an upload can open it under its own name; "" if none.
std::string tempDownloadPath(plat::App &app, std::string_view name);
// Removes such a copy and the directory made for it.
void        removeTempDownload(const std::string &path);

} // namespace screens
