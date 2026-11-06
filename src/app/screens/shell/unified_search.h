// The unified search panel (Slack's "Search <workspace>"): one field over
// recent places, people, channels and messages. Empty, it lists Recent places;
// typing groups People, Channels (instant, from the Store) and the first few
// Messages (Backend::search, debounced), then "Search all messages", which
// shows every hit in the panel. ↑/↓ move across the groups, Enter opens, Esc
// leaves the full list, then closes. With several workspaces it carries the
// quick switcher's tab strip; every group follows the selected tab.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "screens/shell/quick_switcher.h"
#include "screens/shell/sidebar.h"
#include "screens/shell/unified_search_model.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shell {

class UnifiedSearch : public ui::Popup {
public:
    // In the order the groups show (rows() is sorted by kind).
    enum class RowKind : uint8_t {
        Recent,
        Person,
        Channel,
        Message,
        AllMessages,
        FullResult,
        Status
    };
    struct Row {
        RowKind     kind;
        std::string text;
        size_t      index = 0; // into the kind's own list
    };

    // `tabs` empty: the open workspace alone. `backendFor(tab)`: that tab's
    // Backend for message search (null: none).
    UnifiedSearch(
        screens::Context                        &ctx,
        Avatars                                 &avatars,
        std::vector<QuickSwitchTab>              tabs,
        int                                      openTab,
        std::function<model::Backend *(int tab)> backendFor
    );
    ~UnifiedSearch() override;

    ui::TextEdit           &field() { return *_field; }
    const std::vector<Row> &rows() const { return _rows; }
    int                     selected() const { return _current; } // 0 after every new query
    void                    choose(int row);   // opens it and closes (AllMessages expands instead)
    void                    showAllMessages(); // every hit for the query, no second request
    bool                    showingAll() const { return _all; }
    int                     tab() const { return _tab; }
    void                    setTab(int index); // a click on its bubble
    void                    stepTab(int delta);

    // Outlets (`key`: the selected tab's workspace).
    std::function<void(const std::string &key, model::ConvRef)>            onOpenConv;
    std::function<void(const std::string &key, Sidebar::Nav)>              onOpenPage;
    std::function<void(const std::string &key, model::UserRef)>            onMessageUser;
    std::function<void(const std::string &key, model::ConvRef, model::Ts)> onOpenMessage;

    static constexpr double kDebounceMs = 300;

    bool tick(double nowMs) override; // the message search's debounce
    // Closed: a search still out (or pending) is dropped, as the panel lingers
    // in the window's graveyard until the next flush.
    void willClose() override;

private:
    class Adapter;
    friend class Adapter;
    void                applyQuery(); // re-aims the tab, then requery()
    void                requery();    // the groups for the field and tab; search restarts
    void                rebuild(bool keepPick);
    void                aimTab(const std::string &query);
    void                highlight(int index);
    void                refreshStrip();
    const model::Store &store() const { return *_tabs[size_t(_tab)].store; }
    static const std::vector<QuickSwitchName> &names(QuickSwitchTab &t); // brought up to date

    Avatars                               &_avatars;
    std::vector<QuickSwitchTab>            _tabs;
    std::function<model::Backend *(int)>   _backendFor;
    int                                    _tab       = 0;
    bool                                   _manualTab = false; // picked by hand since typing began
    std::vector<bool>                      _dimmed;
    std::string                            _query; // trimmed
    std::vector<RecentPlace>               _recent;
    std::vector<PersonHit>                 _people;
    std::vector<model::ConvRef>            _channels;
    std::vector<model::Backend::SearchHit> _hits;
    bool                                   _searching = false; // a search pending or out
    bool                                   _all       = false;
    std::vector<Row>                       _rows;
    int                                    _current       = 0;
    uint32_t                               _generation    = 0;  // the search a reply belongs to
    double                                 _debounceStart = -1; // -1: from the next tick
    bool                                   _debouncing    = false;
    std::shared_ptr<char>                  _alive         = std::make_shared<char>();
    ui::TextEdit                          *_field         = nullptr;
    ui::View                              *_strip         = nullptr;
    ui::Label                             *_tabName       = nullptr;
    std::unique_ptr<Adapter>               _adapter;
    ui::VirtualList                       *_list = nullptr;
};

} // namespace shell
