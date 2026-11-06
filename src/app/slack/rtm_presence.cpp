// RtmPresence (see rtm_presence.h), over net::WebSocket.
#include "app/slack/rtm_presence.h"

#include "app/slack/web_api.h"

#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>

namespace slack {

namespace {

// Pings unanswered before a silent socket counts as dead (a minute at the
// default cadence; also what a sleep gap looks like).
constexpr int kMaxMissedPongs = 2;

// rtm.connect answers meaning "this token never gets a socket" (dead
// credentials, or the method refused for it): retrying would only churn the
// rate limit. Anything else backs off and retries.
bool fatalConnectError(const std::string &e) {
    return isAuthError(e) || isMethodUnavailable(e);
}

} // namespace

RtmPresence::RtmPresence(plat::App &app, Connect connect, std::string cookie)
    : _app(app), _connect(std::move(connect)), _cookie(std::move(cookie)),
      _alive(std::make_shared<bool>(true)) {}

RtmPresence::~RtmPresence() {
    *_alive        = false;
    onStateChanged = nullptr;
    teardown();
    stopTimer(_idleTimer);
}

void RtmPresence::stopTimer(uint64_t &id) {
    if (id)
        _app.cancelTimer(id);
    id = 0;
}

bool RtmPresence::connected() const {
    return _ws && _ws->isOpen();
}

void RtmPresence::setMode(Mode mode) {
    if (mode == _mode)
        return;
    _mode        = mode;
    // A deliberate change is a fresh start: forget a refusal (the session may
    // have been re-imported) and any inherited backoff.
    _unavailable = false;
    _reconnectMs = _t.reconnectMinMs;
    _idle        = false;
    stopTimer(_idleTimer);
    if (_mode == Mode::WhileUsing) // the click that chose it counts as activity
        armIdle();
    if (!holding())
        teardown();
    // An already-up socket goes straight to Active (only the tickles differ).
    refreshState();
    ensureHolding();
}

void RtmPresence::start() {
    if (_started)
        return;
    _started = true;
    ensureHolding();
}

void RtmPresence::reconnectNow() {
    if (!connected())
        return; // down already: its reconnect is under way
    teardown();
    scheduleReconnect();
}

void RtmPresence::armIdle() {
    stopTimer(_idleTimer);
    _idleTimer = _app.addTimer(_t.idleMs, false, [this] {
        _idleTimer = 0;
        onIdle();
    });
}

void RtmPresence::refreshState() {
    const Link was = _state;
    setState(
        _mode == Mode::Native ? Link::Off
        : _unavailable        ? Link::Unavailable
        : _idle               ? Link::Idle
        : _up && connected()  ? Link::Active
                              : Link::Connecting
    );
    // WhileRunning tickles on a clock; a link turning active is active at
    // once, not at the first input.
    stopTimer(_tickleTimer);
    if (_state == Link::Active && _mode == Mode::WhileRunning)
        _tickleTimer = _app.addTimer(_t.alwaysTickleMs, true, [this] { sendTickle(true); });
    if (_state == Link::Active && was != Link::Active)
        sendTickle(true);
}

void RtmPresence::noteActivity() {
    if (_mode == Mode::Native)
        return;
    if (_mode == Mode::WhileUsing) {
        armIdle();
        if (_idle) {
            _idle        = false;
            _reconnectMs = _t.reconnectMinMs; // the user is back: no inherited backoff
            refreshState();                   // tickles at once if the socket is up
            ensureHolding();
            return;
        }
    }
    sendTickle(false);
}

void RtmPresence::ensureHolding() {
    if (!holding() || _unavailable || _connecting || _reconnectTimer || connected())
        return;
    openAndConnect();
}

void RtmPresence::openAndConnect() {
    if (_connecting)
        return;
    _connecting = true;
    refreshState();
    // presence_sub keeps Slack from streaming the whole roster's
    // presence_change (we subscribe to nobody); batch_presence_aware is its
    // prerequisite.
    const int gen = _generation;
    _connect(
        "batch_presence_aware=1&presence_sub=true",
        [this, gen, alive = _alive](const json::Document &doc, const std::string &err) {
            if (!*alive || gen != _generation)
                return; // torn down meanwhile: never a competing socket
            if (err.empty()) {
                const std::string_view url = doc.root()["url"].str();
                if (!url.empty()) {
                    connectWs(std::string(url));
                    return;
                }
            }
            _connecting = false;
            if (fatalConnectError(err)) {
                LOG_WARN(
                    "slack",
                    "RtmPresence: rtm.connect refused (%s) — cannot keep this workspace active",
                    err.c_str()
                );
                _unavailable = true;
                refreshState();
                return;
            }
            LOG_INFO("slack", "RtmPresence: rtm.connect failed — %s", err.c_str());
            scheduleReconnect();
        }
    );
}

void RtmPresence::connectWs(const std::string &url) {
    // Never two sockets: each one counts as a client.
    retireSocket(_app, _ws);
    _ws           = std::make_unique<net::WebSocket>(_app);
    _ws->onOpen   = [this] { onOpen(); };
    _ws->onText   = [this](std::string text) { onText(text); };
    _ws->onClosed = [this](int code, std::string reason) { onClosed(code, reason); };
    // The URL carries no auth: the `d` cookie on the handshake IS the auth.
    _ws->open(url, {{"Cookie", "d=" + _cookie}});
}

void RtmPresence::teardown() {
    ++_generation;
    _connecting     = false;
    _up             = false;
    _connectedSince = 0;
    _awaitingPongs  = 0;
    stopTimer(_reconnectTimer);
    stopTimer(_pingTimer);
    stopTimer(_tickleTimer);
    retireSocket(_app, _ws); // not inside its own callback
}

void RtmPresence::scheduleReconnect() {
    if (!holding() || _unavailable || _reconnectTimer)
        return; // one pending reconnect at a time
    refreshState();
    const int delay = std::max(_reconnectMs, _t.reconnectMinMs);
    _reconnectMs    = std::min(delay * 2, _t.reconnectMaxMs);
    _reconnectTimer = _app.addTimer(delay, false, [this] {
        _reconnectTimer = 0;
        ensureHolding();
    });
}

void RtmPresence::onOpen() {
    _connecting     = false;
    _connectedSince = base::monotonicMs();
    _awaitingPongs  = 0;
    stopTimer(_pingTimer);
    _pingTimer = _app.addTimer(_t.pingMs, true, [this] { sendPing(); });
    // Active comes with `hello`: an unauthenticated handshake opens too.
}

void RtmPresence::onClosed(int code, const std::string &reason) {
    const int64_t now = base::monotonicMs();
    LOG_INFO(
        "slack",
        "RtmPresence: socket closed — code %d %s (up %lld ms)",
        code,
        reason.c_str(),
        (long long)(_connectedSince ? now - _connectedSince : 0)
    );
    _connecting = false;
    _up         = false;
    stopTimer(_pingTimer);
    retireSocket(_app, _ws);
    // Only a durable connection resets the backoff (rtm.connect is Tier 1).
    if (_connectedSince && now - _connectedSince >= _t.stableMs)
        _reconnectMs = _t.reconnectMinMs;
    _connectedSince = 0;
    _awaitingPongs  = 0;
    refreshState();
    scheduleReconnect();
}

void RtmPresence::onText(const std::string &text) {
    // Unstarted, nobody listens to the event stream: only hello, pong and
    // error matter, and a frame without one of the words is none of them.
    const std::string_view t(text);
    if (!onEvent && t.find("hello") == t.npos && t.find("pong") == t.npos &&
        t.find("error") == t.npos)
        return;
    json::Document doc;
    if (!doc.parse(std::string_view(text), nullptr))
        return;
    const std::string_view type = doc.root()["type"].str();
    if (type == "hello") {
        _reconnectMs   = _t.reconnectMinMs;
        _up            = true;
        const bool gap = _hadHello;
        _hadHello      = true;
        refreshState();
        if (onHello)
            onHello(gap);
        return;
    }
    if (type == "pong") {
        _awaitingPongs = 0;
        return;
    }
    if (type == "error") {
        // e.g. invalid_auth without the cookie: Slack drops the socket soon
        // anyway; don't wait for it.
        LOG_WARN(
            "slack",
            "RtmPresence: server error frame — %.*s",
            int(std::min<size_t>(text.size(), 200)),
            text.data()
        );
        teardown();
        scheduleReconnect();
        return;
    }
    if (type == "reconnect_url" || !onEvent)
        return; // a migration hint we don't use; the event stream unheard
    onEvent(doc.root());
}

void RtmPresence::sendPing() {
    if (!connected())
        return;
    if (_awaitingPongs >= kMaxMissedPongs) {
        LOG_WARN("slack", "RtmPresence: no pong for %d pings — reconnecting", _awaitingPongs);
        teardown();
        scheduleReconnect();
        return;
    }
    ++_awaitingPongs;
    _ws->sendText(str::concat({"{\"type\":\"ping\",\"id\":", str::number(++_pingId), "}"}));
}

void RtmPresence::sendTickle(bool force) {
    if (_state != Link::Active || !connected())
        return;
    const int64_t now = base::monotonicMs();
    if (!force && _lastTickle && now - _lastTickle < _t.tickleGapMs)
        return;
    _lastTickle = now;
    _ws->sendText("{\"type\":\"tickle\"}");
}

void RtmPresence::onIdle() {
    if (_mode != Mode::WhileUsing)
        return;
    _idle = true;
    if (_started) {
        LOG_INFO("slack", "RtmPresence: no input for %d s — no more tickles", _t.idleMs / 1000);
    } else {
        LOG_INFO("slack", "RtmPresence: no input for %d s — dropping the link", _t.idleMs / 1000);
        teardown();
    }
    refreshState();
}

void RtmPresence::setState(Link s) {
    if (s == _state)
        return;
    _state = s;
    if (onStateChanged) {
        auto fn = onStateChanged;
        fn(s);
    }
}

} // namespace slack
