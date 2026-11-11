#include "app/screens/common/downloads.h"

#include "app/model/downloads.h"
#include "app/model/jobs.h"
#include "app/screens/common/remote_images.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "plat/plat.h"

#include <memory>

namespace screens {

void fetchFile(
    plat::App               &app,
    model::Backend          &backend,
    const std::string       &source,
    std::string              toPath,
    model::Backend::Done     done,
    model::Backend::Progress progress
) {
    if (RemoteImages::isRemote(source)) {
        backend.downloadFile(source, std::move(toPath), std::move(done), std::move(progress));
        return;
    }
    auto ok = std::make_shared<bool>(false);
    model::runInBackground(
        app,
        [ok, from = source, toPath = std::move(toPath)] {
            *ok = from == toPath || file::copy(from, toPath);
        },
        [ok, done = std::move(done)] {
            if (done)
                done(*ok, *ok ? std::string() : std::string("copy_failed"));
        }
    );
}

std::string uniqueDownloadPath(std::string_view dir, std::string_view name) {
    std::string base(file::baseName(name));
    if (base.empty() || base == "." || base == "..")
        base = "file";
    // "archive.tar.gz" numbers before its first dot, "report.pdf" before
    // ".pdf"; a leading dot (".env") is part of the stem.
    const size_t      dot   = base.find('.', 1);
    const std::string stem  = base.substr(0, dot),
                      ext   = dot == std::string::npos ? "" : base.substr(dot);
    const auto        taken = [](const std::string &p) {
        if (file::exists(p))
            return true;
        for (const model::Downloads::Item &it : model::downloads().items())
            if (it.state == model::Downloads::State::Running && it.path == p)
                return true;
        return false;
    };
    std::string path = file::join(dir, base);
    for (int n = 1; taken(path) && n < 10000; ++n)
        path = file::join(dir, str::concat({stem, " (", str::number(int64_t(n)), ")", ext}));
    return path;
}

void startDownload(
    plat::App         &app,
    model::Backend    &backend,
    std::string        name,
    const std::string &source,
    std::string        toPath,
    std::string        kind
) {
    const int id =
        model::downloads().begin(std::move(name), std::move(toPath), std::move(kind), source);
    runDownload(app, backend, id);
}

// The fetch of a listed download (its first run, or a resume): a job while
// it runs, its progress and end into the list. A pause or cancel stops the
// transfer (done: "cancelled"), which finish() then ignores.
void runDownload(plat::App &app, model::Backend &backend, int id) {
    const model::Downloads::Item *it = model::downloads().find(id);
    if (!it)
        return;
    const int job = model::jobs().begin(i18n::arg(i18n::tr("Downloading %1"), it->name));
    fetchFile(
        app,
        backend,
        it->source,
        it->path,
        [job, id](bool ok, const std::string &err) {
            if (!ok && err != "cancelled")
                LOG_WARN("messages", "File download failed: %s", err.c_str());
            model::jobs().end(job);
            model::downloads().finish(id, ok, err);
        },
        [id](int64_t received, int64_t total) { model::downloads().progress(id, received, total); }
    );
}

void pauseDownload(model::Backend &backend, int id) {
    auto &list = model::downloads();
    if (const model::Downloads::Item *it = list.find(id); it && list.pause(id))
        backend.cancelDownload(it->path);
}

void resumeDownload(plat::App &app, model::Backend &backend, int id) {
    // shortcut: from the start (the transfer keeps the body in memory, so a
    // paused one has nothing on disk); an HTTP Range resume would go on.
    if (model::downloads().restart(id))
        runDownload(app, backend, id);
}

void cancelDownload(model::Backend &backend, int id) {
    auto                         &list = model::downloads();
    const model::Downloads::Item *it   = list.find(id);
    if (!it)
        return;
    const std::string path    = it->path;
    const bool        running = it->state == model::Downloads::State::Running;
    list.remove(id);
    if (running)
        backend.cancelDownload(path);
}

std::string tempDownloadPath(plat::App &app, std::string_view name) {
    const std::string tmp = app.standardDir(plat::StandardDir::Temp);
    if (tmp.empty())
        return {};
    const std::string rnd = crypto::randomHex(8);
    if (rnd.empty())
        return {};
    const std::string dir = file::join(file::join(tmp, "msga/downloads"), rnd);
    if (!file::makeDirs(dir))
        return {};
    // Only the last path component, and never "" / "." / "..".
    std::string base(file::baseName(name));
    if (base.empty() || base == "." || base == "..")
        base = "file";
    return file::join(dir, base);
}

void removeTempDownload(const std::string &path) {
    file::remove(path);
    file::remove(file::dirName(path));
}

} // namespace screens
