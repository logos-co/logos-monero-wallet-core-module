#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include <set>
#include <nlohmann/json.hpp>

/// The worker that owns the monero_c handles. Every wallet2 MUTATION runs on its thread;
/// reads take a shared lock on the handle so a close cannot race them.
class WalletRuntime {
public:
    using EmitFn = std::function<void(const std::string& event, const nlohmann::json& payload)>;
    /// Resolves a network name to the node module's config `{ url, proxy?, proxyRequired?,
    /// trusted?, torUse?, route?, catchUp?: { url } }`.
    using NodeResolver = std::function<nlohmann::json(const std::string& network)>;
    /// Tells the node module a wallet opened or closed on a network, which locks its settings.
    using OpenNotifier = std::function<void(const std::string& network, bool open)>;

    WalletRuntime(EmitFn emit, NodeResolver resolve, OpenNotifier notify = {});
    ~WalletRuntime();
    WalletRuntime(const WalletRuntime&) = delete;
    WalletRuntime& operator=(const WalletRuntime&) = delete;

    void setWalletsDir(const std::string& dir);

    nlohmann::json startJob(const std::string& kind, const nlohmann::json& params);
    nlohmann::json jobStatus(const std::string& id, const std::string& receipt);
    nlohmann::json jobResult(const std::string& id, const std::string& receipt);
    bool ackJob(const std::string& id, const std::string& receipt);
    bool cancelJob(const std::string& id, const std::string& receipt);

    nlohmann::json status();
    std::string balance(uint32_t account);
    std::string unlockedBalance(uint32_t account);
    std::string address(uint64_t account, uint64_t index);
    nlohmann::json subaddresses(uint32_t account);
    nlohmann::json createSubaddress(uint32_t account, const std::string& label);
    nlohmann::json setSubaddressLabel(uint32_t account, uint32_t index, const std::string& label);
    nlohmann::json history();
    bool addressValid(const std::string& addr, const std::string& network);
    nlohmann::json revealSeed(const std::string& password);
    nlohmann::json revealViewKey(const std::string& password);
    nlohmann::json listWallets();
    bool walletExists(const std::string& name);

    /// Store, close, and join the worker. Bounded; idempotent.
    void shutdown();

private:
    enum class State { NoWallet, Opening, Syncing, Ready, Closing, Failed };
    static const char* stateName(State s);
    /// Where wallet2 dials. CatchUp and WaitingForTor are Tor routes not yet on Tor: sends wait.
    enum class Route { Direct, CatchUp, WaitingForTor, Tor };
    static const char* routeName(Route r);

    struct Job {
        std::string id, receipt, kind, state = "queued", error;
        nlohmann::json params, result, progress;
        bool acked = false;
    };

    void threadMain();
    /// Its own thread: reads whether wallet2 holds a connection, and the daemon's height while
    /// synchronized, as wallet2's daemon reads wait behind any request in flight. status()
    /// reports what it last saw.
    void probeMain();
    void run(const std::shared_ptr<Job>& job);
    void tick();
    void setState(State s, const std::string& error = {});
    nlohmann::json statusLocked();
    /// Worker thread: from the catch-up node, or from waiting, onto the node over Tor. `abandon`
    /// drops the catch-up node's connection first, as a stuck one may never end its request.
    void moveToTor(bool abandon = false);
    void notifyOpen(const std::string& network, bool open);
    void resetLocksOnce();
    bool sendsWaitForTor();

    // Job bodies — worker thread only.
    nlohmann::json doOpenLike(const std::shared_ptr<Job>& job);
    nlohmann::json doClose();
    nlohmann::json doRescan();
    nlohmann::json doCreateTransaction(const nlohmann::json& p);
    nlohmann::json doCommit(const nlohmann::json& p);
    nlohmann::json doDispose(const nlohmann::json& p);
    nlohmann::json doChangePassword(const nlohmann::json& p);

    std::string walletPath(const std::string& name) const;
    bool verifyPassword(const std::string& password);

    EmitFn m_emit;
    NodeResolver m_resolve;
    OpenNotifier m_notify;
    bool m_locksReset = false;
    std::string m_walletsDir;

    std::thread m_thread, m_probeThread;
    std::mutex m_qmu;
    std::condition_variable m_cv, m_probeCv;
    std::deque<std::shared_ptr<Job>> m_queue;
    std::map<std::string, std::shared_ptr<Job>> m_jobs;
    bool m_stop = false;
    uint64_t m_nextId = 1;

    // The handles. m_handleMu means EXCLUSIVE ACCESS TO wallet2, not merely "the pointer is
    // alive": wallet2 is not safe for concurrent use, and this module is multi-dispatch, so a
    // read arriving on an IPC thread would otherwise be inside wallet2 while the worker is
    // committing. Every mutation takes it uniquely; reads share it, with a deadline.
    mutable std::shared_timed_mutex m_handleMu;
    void* m_wm = nullptr;
    void* m_wallet = nullptr;
    std::map<std::string, void*> m_pendingTx;   // txHandle -> PendingTransaction*
    uint64_t m_nextTx = 1;

    mutable std::mutex m_stateMu;
    State m_state = State::NoWallet;
    std::string m_walletName, m_network, m_primaryAddress, m_lastError;
    int m_nettype = 0;
    bool m_lastSynced = false;
    Route m_route = Route::Direct;
    // The probe's last reading of the daemon, for the open wallet, and where wallet2 dials:
    // the node, or the SOCKS port on a Tor route.
    bool m_probed = false, m_daemonConnected = false;
    std::string m_dialPeer;
    uint64_t m_daemonHeight = 0;
    // The wallet height, and when it last rose on the current route.
    uint64_t m_progressHeight = 0;
    std::chrono::steady_clock::time_point m_progressAt;
    std::string m_torUse;   // from the node module at open; empty from one that predates it
};
