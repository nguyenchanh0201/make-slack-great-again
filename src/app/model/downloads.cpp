#include "app/model/downloads.h"

#include <algorithm>

namespace model {

int Downloads::begin(std::string name, std::string path) {
    Item it;
    it.id   = _nextId++;
    it.name = std::move(name);
    it.path = std::move(path);
    _items.push_back(std::move(it));
    _observers.notify();
    return _items.back().id;
}

Downloads::Item *Downloads::get(int id) {
    for (Item &it : _items)
        if (it.id == id)
            return &it;
    return nullptr;
}

const Downloads::Item *Downloads::find(int id) const {
    return const_cast<Downloads *>(this)->get(id);
}

void Downloads::progress(int id, int64_t received, int64_t total) {
    Item *it = get(id);
    if (!it || it->state != State::Running)
        return;
    it->received = received;
    it->total    = total;
    _observers.notify();
}

void Downloads::finish(int id, bool ok, std::string error) {
    Item *it = get(id);
    if (!it || it->state != State::Running)
        return;
    it->state = ok ? State::Done : State::Failed;
    it->error = ok ? std::string() : std::move(error);
    if (ok && it->total > 0)
        it->received = it->total;
    _observers.notify();
}

void Downloads::remove(int id) {
    const auto n = _items.size();
    std::erase_if(_items, [id](const Item &it) { return it.id == id; });
    if (_items.size() != n)
        _observers.notify();
}

Downloads::ObserverId Downloads::observe(std::function<void()> fn) {
    return _observers.add([fn = std::move(fn)](const std::string &) { fn(); });
}

Downloads &downloads() {
    static Downloads d;
    return d;
}

} // namespace model
