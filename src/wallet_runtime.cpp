#include "wallet_runtime.h"
#include "address_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>

extern "C" {
#include "monero_wallet2_api_c.h"
}

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// monero_c returns malloc'd copies; every const char* is freed exactly once here.
std::string take(const char* p) {
    std::string s = p ? p : "";
    if (p) MONERO_free(const_cast<char*>(p));
    return s;
}

int nettypeOf(const std::string& network) {
    if (network == "testnet") return 1;
    if (network == "stagenet") return 2;
    return 0;   // mainnet, and regtest (a fakechain uses mainnet address rules)
}

bool isLoopback(const std::string& url) {
    return url.find("127.0.0.1") != std::string::npos || url.find("localhost") != std::string::npos
        || url.find("[::1]") != std::string::npos;
}

// wallet2 wants host:port for its SOCKS proxy; the node module stores a URL.
std::string proxyHostPort(std::string p) {
    const auto at = p.find("://");
    if (at != std::string::npos) p = p.substr(at + 3);
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string hostPort(std::string url) {
    const auto at = url.find("://");
    if (at != std::string::npos) url = url.substr(at + 3);
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::string randomToken() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[33];
    for (int i = 0; i < 32; ++i) buf[i] = "0123456789abcdef"[rng() & 15];
    buf[32] = 0;
    return buf;
}

bool parseAtomic(const std::string& s, uint64_t& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
    errno = 0;
    out = std::strtoull(s.c_str(), nullptr, 10);
    return errno == 0;
}

json err(const std::string& msg) { return json{{"ok", false}, {"error", msg}}; }

} // namespace

const char* WalletRuntime::stateName(State s) {
    switch (s) {
        case State::NoWallet: return "no_wallet";
        case State::Opening:  return "opening";
        case State::Syncing:  return "syncing";
        case State::Ready:    return "ready";
        case State::Closing:  return "closing";
        case State::Failed:   return "failed";
    }
    return "unknown";
}

WalletRuntime::WalletRuntime(EmitFn emit, NodeResolver resolve)
    : m_emit(std::move(emit)), m_resolve(std::move(resolve)) {
    MONERO_WalletManagerFactory_setLogLevel(0);
    m_wm = MONERO_WalletManagerFactory_getWalletManager();
    m_thread = std::thread([this] { threadMain(); });
}

WalletRuntime::~WalletRuntime() { shutdown(); }

void WalletRuntime::setWalletsDir(const std::string& dir) {
    std::lock_guard<std::mutex> g(m_stateMu);
    m_walletsDir = dir;
}

std::string WalletRuntime::walletPath(const std::string& name) const {
    return (fs::path(m_walletsDir) / name).string();
}

// ── Tickets ──────────────────────────────────────────────────────────────

json WalletRuntime::startJob(const std::string& kind, const json& params) {
    static const char* kinds[] = {"open_wallet", "create_wallet", "restore_from_seed", "restore_from_keys",
                                  "close_wallet", "rescan", "create_transaction", "commit_transaction",
                                  "dispose_transaction", "change_password"};
    bool known = false;
    for (auto k : kinds) known |= (kind == k);
    if (!known) return err("unknown job kind: " + kind);
    if (m_walletsDir.empty()) return err("module context not ready (no wallets directory)");

    auto job = std::make_shared<Job>();
    job->kind = kind;
    job->params = params.is_object() ? params : json::object();
    job->receipt = randomToken();
    {
        std::lock_guard<std::mutex> g(m_qmu);
        job->id = "j" + std::to_string(m_nextId++);
        m_jobs[job->id] = job;
        m_queue.push_back(job);
    }
    m_cv.notify_one();
    return json{{"ok", true}, {"jobId", job->id}, {"receipt", job->receipt}};
}

json WalletRuntime::jobStatus(const std::string& id, const std::string& receipt) {
    std::lock_guard<std::mutex> g(m_qmu);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->second->receipt != receipt) return err("unknown job");
    const auto& j = *it->second;
    return json{{"ok", true}, {"state", j.state}, {"progress", j.progress}, {"error", j.error}};
}

json WalletRuntime::jobResult(const std::string& id, const std::string& receipt) {
    std::lock_guard<std::mutex> g(m_qmu);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->second->receipt != receipt) return err("unknown job");
    auto& j = *it->second;
    if (j.acked) return err("job already acked");
    if (j.state == "failed") return err(j.error);
    if (j.state != "done") return err("job not finished: " + j.state);
    return json{{"ok", true}, {"result", j.result}};
}

bool WalletRuntime::ackJob(const std::string& id, const std::string& receipt) {
    std::lock_guard<std::mutex> g(m_qmu);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->second->receipt != receipt) return false;
    if (it->second->state != "done" && it->second->state != "failed") return false;
    m_jobs.erase(it);
    return true;
}

bool WalletRuntime::cancelJob(const std::string& id, const std::string& receipt) {
    std::lock_guard<std::mutex> g(m_qmu);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->second->receipt != receipt) return false;
    if (it->second->state != "queued") return false;
    for (auto q = m_queue.begin(); q != m_queue.end(); ++q) {
        if ((*q)->id == id) { m_queue.erase(q); break; }
    }
    m_jobs.erase(it);
    return true;
}

// ── Worker ───────────────────────────────────────────────────────────────

void WalletRuntime::threadMain() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(m_qmu);
            m_cv.wait_for(lk, std::chrono::seconds(2), [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            if (!m_queue.empty()) { job = m_queue.front(); m_queue.pop_front(); job->state = "running"; }
        }
        if (job) run(job); else tick();
    }
}

void WalletRuntime::run(const std::shared_ptr<Job>& job) {
    json out;
    try {
        const auto& k = job->kind;
        if (k == "open_wallet" || k == "create_wallet" || k == "restore_from_seed" || k == "restore_from_keys")
            out = doOpenLike(job);
        else if (k == "close_wallet")        out = doClose();
        else if (k == "rescan")              out = doRescan();
        else if (k == "create_transaction")  out = doCreateTransaction(job->params);
        else if (k == "commit_transaction")  out = doCommit(job->params);
        else if (k == "dispose_transaction") out = doDispose(job->params);
        else if (k == "change_password")     out = doChangePassword(job->params);
    } catch (const std::exception& e) {
        out = err(std::string("exception: ") + e.what());
    }
    std::string state;
    {
        std::lock_guard<std::mutex> g(m_qmu);
        if (out.value("ok", false)) { job->state = "done"; job->result = out.value("result", json::object()); }
        else { job->state = "failed"; job->error = out.value("error", "unknown error"); }
        state = job->state;
    }
    m_emit("jobFinished", json{{"jobId", job->id}, {"state", state}});
}

// Idle tick: flip syncing -> ready when wallet2 reports synchronized (and back if it
// falls behind), so the state event fires without anyone polling status().
void WalletRuntime::tick() {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return;
    const bool synced = MONERO_Wallet_synchronized(m_wallet);
    h.unlock();
    State cur;
    { std::lock_guard<std::mutex> g(m_stateMu); cur = m_state; }
    if (cur == State::Syncing && synced) setState(State::Ready);
    else if (cur == State::Ready && !synced) setState(State::Syncing);
}

void WalletRuntime::setState(State s, const std::string& error) {
    json payload;
    {
        std::lock_guard<std::mutex> g(m_stateMu);
        m_state = s;
        if (!error.empty()) m_lastError = error;
        payload = statusLocked();
    }
    m_emit("walletStateChanged", payload);
}

// ── Job bodies ───────────────────────────────────────────────────────────

json WalletRuntime::doOpenLike(const std::shared_ptr<Job>& job) {
    const json& p = job->params;
    const std::string name = p.value("name", ""), password = p.value("password", ""), network = p.value("network", "");
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos)
        return err("bad wallet name");
    if (network.empty()) return err("network is required");

    // Node policy first, so a refusal costs nothing.
    const json node = m_resolve(network);
    if (node.contains("error")) return err(node["error"].get<std::string>());
    const std::string url = node.value("url", "");
    const std::string proxy = node.value("proxy", "");
    const bool proxyRequired = node.value("proxyRequired", false);
    if (url.empty()) return err("no daemon configured for " + network);
    if (proxyRequired && proxy.empty())
        return err("proxy required for " + network + " but none configured (fail-closed: refusing to connect in the clear)");

    {
        std::shared_lock<std::shared_mutex> h(m_handleMu);
        if (m_wallet) return err("a wallet is already open; close it first");
    }
    setState(State::Opening);

    const int nettype = nettypeOf(network);
    const std::string path = walletPath(name);
    MONERO_WalletManager_setDaemonAddress(m_wm, hostPort(url).c_str());

    void* w = nullptr;
    const auto& k = job->kind;
    if (k == "open_wallet") {
        if (!MONERO_WalletManager_walletExists(m_wm, path.c_str())) { setState(State::NoWallet); return err("no such wallet: " + name); }
        w = MONERO_WalletManager_openWallet(m_wm, path.c_str(), password.c_str(), nettype);
    } else if (k == "create_wallet") {
        if (MONERO_WalletManager_walletExists(m_wm, path.c_str())) { setState(State::NoWallet); return err("wallet already exists: " + name); }
        w = MONERO_WalletManager_createWallet(m_wm, path.c_str(), password.c_str(), p.value("language", "English").c_str(), nettype);
    } else if (k == "restore_from_seed") {
        if (MONERO_WalletManager_walletExists(m_wm, path.c_str())) { setState(State::NoWallet); return err("wallet already exists: " + name); }
        w = MONERO_WalletManager_recoveryWallet(m_wm, path.c_str(), password.c_str(), p.value("seed", "").c_str(), nettype,
                                                 p.value("restoreHeight", 0ULL), 1, p.value("seedOffset", "").c_str());
    } else {
        if (MONERO_WalletManager_walletExists(m_wm, path.c_str())) { setState(State::NoWallet); return err("wallet already exists: " + name); }
        w = MONERO_WalletManager_createWalletFromKeys(m_wm, path.c_str(), password.c_str(), "English", nettype,
                                                       p.value("restoreHeight", 0ULL), p.value("address", "").c_str(),
                                                       p.value("viewKey", "").c_str(), p.value("spendKey", "").c_str(), 1);
    }
    if (!w || MONERO_Wallet_status(w) != 0) {
        const std::string e = w ? take(MONERO_Wallet_errorString(w)) : take(MONERO_WalletManager_errorString(m_wm));
        if (w) MONERO_WalletManager_closeWallet(m_wm, w, false);
        setState(State::Failed, e.empty() ? "open failed" : e);
        return err(e.empty() ? "open failed" : e);
    }

    // A fresh regtest wallet must not inherit the timestamp-based mainnet height estimate
    // (P0: it puts the scan start at ~3.6M on a 76-block fakechain).
    if (network == "regtest" && k != "open_wallet") MONERO_Wallet_setRefreshFromBlockHeight(w, 0);

    const bool ok = MONERO_Wallet_init(w, hostPort(url).c_str(), 0, "", "", false, false, proxyHostPort(proxy).c_str());
    if (!ok) {
        const std::string e = take(MONERO_Wallet_errorString(w));
        MONERO_WalletManager_closeWallet(m_wm, w, k != "open_wallet");
        setState(State::Failed, e);
        return err("init failed: " + e);
    }
    MONERO_Wallet_setTrustedDaemon(w, node.value("trusted", false) && isLoopback(url));

    // wallet2 honours the proxy but never errors when it cannot reach the node — it just sits
    // disconnected (P0-verified). Under a REQUIRED proxy that is a failure, not a state.
    if (!proxy.empty()) {
        bool connected = false;
        for (int i = 0; i < 20 && !connected; ++i) {
            connected = MONERO_Wallet_connected(w) == 1;
            if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!connected && proxyRequired) {
            MONERO_WalletManager_closeWallet(m_wm, w, true);
            setState(State::Failed, "daemon unreachable through the required proxy");
            return err("daemon unreachable through the required proxy");
        }
    }

    MONERO_Wallet_setAutoRefreshInterval(w, 2000);
    MONERO_Wallet_startRefresh(w);
    MONERO_Wallet_refreshAsync(w);

    const std::string addr = take(MONERO_Wallet_address(w, 0, 0));
    {
        std::unique_lock<std::shared_mutex> h(m_handleMu);
        m_wallet = w;
    }
    {
        std::lock_guard<std::mutex> g(m_stateMu);
        m_walletName = name; m_network = network; m_primaryAddress = addr; m_nettype = nettype; m_lastError.clear();
    }
    // Restrict the files wallet2 just wrote; it uses the default umask.
    std::error_code ec;
    for (const auto& f : {path, path + ".keys", path + ".address.txt"})
        if (fs::exists(f, ec)) fs::permissions(f, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);

    setState(State::Syncing);
    return json{{"ok", true}, {"result", json{{"name", name}, {"network", network}, {"address", addr},
                                              {"watchOnly", MONERO_Wallet_watchOnly(w)}}}};
}

json WalletRuntime::doClose() {
    std::unique_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    setState(State::Closing);
    void* w = m_wallet;
    m_wallet = nullptr;
    m_pendingTx.clear();
    h.unlock();
    MONERO_Wallet_store(w, "");
    const bool ok = MONERO_WalletManager_closeWallet(m_wm, w, true);
    {
        std::lock_guard<std::mutex> g(m_stateMu);
        m_walletName.clear(); m_primaryAddress.clear(); m_lastSynced = false;
    }
    setState(State::NoWallet);
    return ok ? json{{"ok", true}, {"result", json::object()}} : err("close failed");
}

json WalletRuntime::doRescan() {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    MONERO_Wallet_rescanBlockchainAsync(m_wallet);
    h.unlock();
    setState(State::Syncing);
    return json{{"ok", true}, {"result", json::object()}};
}

json WalletRuntime::doCreateTransaction(const json& p) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    if (MONERO_Wallet_watchOnly(m_wallet)) return err("view-only wallet cannot spend");
    uint64_t amount = 0;
    if (!parseAtomic(p.value("amount", ""), amount) || amount == 0) return err("amount must be a positive decimal string of atomic units");
    const std::string dst = p.value("address", "");
    std::string net; { std::lock_guard<std::mutex> g(m_stateMu); net = m_network; }
    if (!monero_addr::valid(dst, net)) return err("invalid destination address for this network");
    const int priority = p.value("priority", 0);
    const uint32_t account = p.value("accountIndex", 0u);

    void* pt = MONERO_Wallet_createTransaction(m_wallet, dst.c_str(), p.value("paymentId", "").c_str(), amount,
                                               0, priority, account, "", ",");
    if (!pt) return err("createTransaction returned null");
    if (MONERO_PendingTransaction_status(pt) != 0) {
        const std::string e = take(MONERO_PendingTransaction_errorString(pt));
        return err(e.empty() ? "createTransaction failed" : e);
    }
    const std::string handle = "tx" + std::to_string(m_nextTx++);
    m_pendingTx[handle] = pt;
    return json{{"ok", true}, {"result", json{
        {"txHandle", handle},
        {"amount", std::to_string(MONERO_PendingTransaction_amount(pt))},
        {"fee", std::to_string(MONERO_PendingTransaction_fee(pt))},
        {"dust", std::to_string(MONERO_PendingTransaction_dust(pt))},
        {"txCount", MONERO_PendingTransaction_txCount(pt)},
        {"txids", take(MONERO_PendingTransaction_txid(pt, ","))},
        {"destination", dst}}}};
}

json WalletRuntime::doCommit(const json& p) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    auto it = m_pendingTx.find(p.value("txHandle", ""));
    if (it == m_pendingTx.end()) return err("unknown txHandle");
    void* pt = it->second;
    const bool ok = MONERO_PendingTransaction_commit(pt, "", false);
    if (!ok) return err(take(MONERO_PendingTransaction_errorString(pt)));
    const std::string txids = take(MONERO_PendingTransaction_txid(pt, ","));
    m_pendingTx.erase(it);   // monero_c exposes no disposeTransaction; the handle is dropped
    MONERO_Wallet_store(m_wallet, "");
    return json{{"ok", true}, {"result", json{{"txids", txids}}}};
}

json WalletRuntime::doDispose(const json& p) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    auto it = m_pendingTx.find(p.value("txHandle", ""));
    if (it == m_pendingTx.end()) return err("unknown txHandle");
    m_pendingTx.erase(it);
    return json{{"ok", true}, {"result", json::object()}};
}

json WalletRuntime::doChangePassword(const json& p) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    if (!verifyPassword(p.value("oldPassword", ""))) return err("wrong password");
    if (!MONERO_Wallet_setPassword(m_wallet, p.value("newPassword", "").c_str()))
        return err(take(MONERO_Wallet_errorString(m_wallet)));
    MONERO_Wallet_store(m_wallet, "");
    return json{{"ok", true}, {"result", json::object()}};
}

bool WalletRuntime::verifyPassword(const std::string& password) {
    std::string keys;
    { std::lock_guard<std::mutex> g(m_stateMu); keys = walletPath(m_walletName) + ".keys"; }
    return MONERO_WalletManager_verifyWalletPassword(m_wm, keys.c_str(), password.c_str(), false, 1);
}

// ── Reads ────────────────────────────────────────────────────────────────

json WalletRuntime::statusLocked() {
    json j{{"state", stateName(m_state)}, {"wallet", m_walletName}, {"network", m_network},
           {"address", m_primaryAddress}, {"lastError", m_lastError},
           {"libraryVersion", MONERO_C_VERSION}};
    return j;
}

json WalletRuntime::status() {
    json j;
    { std::lock_guard<std::mutex> g(m_stateMu); j = statusLocked(); }
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (m_wallet) {
        j["connected"]    = MONERO_Wallet_connected(m_wallet) == 1;
        j["synchronized"] = MONERO_Wallet_synchronized(m_wallet);
        j["walletHeight"] = MONERO_Wallet_blockChainHeight(m_wallet);
        j["daemonHeight"] = MONERO_Wallet_daemonBlockChainHeight(m_wallet);
        j["watchOnly"]    = MONERO_Wallet_watchOnly(m_wallet);
    } else {
        j["connected"] = false; j["synchronized"] = false; j["walletHeight"] = 0; j["daemonHeight"] = 0; j["watchOnly"] = false;
    }
    return j;
}

std::string WalletRuntime::balance(uint32_t account) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    return m_wallet ? std::to_string(MONERO_Wallet_balance(m_wallet, account)) : "0";
}

std::string WalletRuntime::unlockedBalance(uint32_t account) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    return m_wallet ? std::to_string(MONERO_Wallet_unlockedBalance(m_wallet, account)) : "0";
}

std::string WalletRuntime::address(uint64_t account, uint64_t index) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    return m_wallet ? take(MONERO_Wallet_address(m_wallet, account, index)) : "";
}

json WalletRuntime::subaddresses(uint32_t account) {
    json out = json::array();
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return out;
    const size_t n = MONERO_Wallet_numSubaddresses(m_wallet, account);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(json{{"index", i},
                           {"address", take(MONERO_Wallet_address(m_wallet, account, i))},
                           {"label", take(MONERO_Wallet_getSubaddressLabel(m_wallet, account, static_cast<uint32_t>(i)))}});
    }
    return out;
}

json WalletRuntime::createSubaddress(uint32_t account, const std::string& label) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    MONERO_Wallet_addSubaddress(m_wallet, account, label.c_str());
    const size_t n = MONERO_Wallet_numSubaddresses(m_wallet, account);
    if (n == 0) return err("addSubaddress failed");
    const size_t idx = n - 1;
    MONERO_Wallet_store(m_wallet, "");
    return json{{"ok", true}, {"result", json{{"index", idx}, {"address", take(MONERO_Wallet_address(m_wallet, account, idx))}}}};
}

// Rename or clear a subaddress label. Index 0 is the account's primary address, which
// wallet2 also labels ("Primary account"), so it is editable like any other.
json WalletRuntime::setSubaddressLabel(uint32_t account, uint32_t index, const std::string& label) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    if (index >= MONERO_Wallet_numSubaddresses(m_wallet, account)) return err("no such subaddress");
    MONERO_Wallet_setSubaddressLabel(m_wallet, account, index, label.c_str());
    MONERO_Wallet_store(m_wallet, "");
    return json{{"ok", true}, {"result", json{{"index", index},
                {"label", take(MONERO_Wallet_getSubaddressLabel(m_wallet, account, index))}}}};
}

json WalletRuntime::history() {
    json out = json::array();
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return out;
    void* hist = MONERO_Wallet_history(m_wallet);
    if (!hist) return out;
    MONERO_TransactionHistory_refresh(hist);
    const int n = MONERO_TransactionHistory_count(hist);
    for (int i = 0; i < n; ++i) {
        void* t = MONERO_TransactionHistory_transaction(hist, i);
        if (!t) continue;
        // Where an outgoing payment went. wallet2 records destinations only for transfers this
        // wallet made, so an incoming row carries none — the UI must not present that as "unknown".
        json dests = json::array();
        const int dn = MONERO_TransactionInfo_transfers_count(t);
        for (int d = 0; d < dn; ++d) {
            dests.push_back(json{{"address", take(MONERO_TransactionInfo_transfers_address(t, d))},
                                 {"amount", std::to_string(MONERO_TransactionInfo_transfers_amount(t, d))}});
        }
        out.push_back(json{
            {"txid", take(MONERO_TransactionInfo_hash(t))},
            {"direction", MONERO_TransactionInfo_direction(t) == 0 ? "in" : "out"},
            {"amount", std::to_string(MONERO_TransactionInfo_amount(t))},
            {"fee", std::to_string(MONERO_TransactionInfo_fee(t))},
            {"height", MONERO_TransactionInfo_blockHeight(t)},
            {"confirmations", MONERO_TransactionInfo_confirmations(t)},
            {"timestamp", MONERO_TransactionInfo_timestamp(t)},
            {"pending", MONERO_TransactionInfo_isPending(t)},
            {"failed", MONERO_TransactionInfo_isFailed(t)},
            {"coinbase", MONERO_TransactionInfo_isCoinbase(t)},
            {"unlockTime", MONERO_TransactionInfo_unlockTime(t)},
            {"paymentId", take(MONERO_TransactionInfo_paymentId(t))},
            {"description", take(MONERO_TransactionInfo_description(t))},
            {"subaddrIndex", take(MONERO_TransactionInfo_subaddrIndex(t, ","))},
            {"destinations", dests},
            {"account", MONERO_TransactionInfo_subaddrAccount(t)}});
    }
    return out;
}

bool WalletRuntime::addressValid(const std::string& addr, const std::string& network) {
    return monero_addr::valid(addr, network);
}

json WalletRuntime::revealSeed(const std::string& password) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    if (!verifyPassword(password)) return err("wrong password");
    const std::string seed = take(MONERO_Wallet_seed(m_wallet, ""));
    if (seed.empty()) return err("this wallet has no seed (restored from keys?)");
    return json{{"ok", true}, {"result", json{{"seed", seed}}}};
}

json WalletRuntime::revealViewKey(const std::string& password) {
    std::shared_lock<std::shared_mutex> h(m_handleMu);
    if (!m_wallet) return err("no wallet open");
    if (!verifyPassword(password)) return err("wrong password");
    return json{{"ok", true}, {"result", json{{"viewKey", take(MONERO_Wallet_secretViewKey(m_wallet))},
                                              {"address", take(MONERO_Wallet_address(m_wallet, 0, 0))}}}};
}

json WalletRuntime::listWallets() {
    json out = json::array();
    std::string dir;
    { std::lock_guard<std::mutex> g(m_stateMu); dir = m_walletsDir; }
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".keys") == 0) out.push_back(fn.substr(0, fn.size() - 5));
    }
    return out;
}

bool WalletRuntime::walletExists(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) return false;
    return MONERO_WalletManager_walletExists(m_wm, walletPath(name).c_str());
}

void WalletRuntime::shutdown() {
    {
        std::lock_guard<std::mutex> g(m_qmu);
        if (m_stop) return;
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::unique_lock<std::shared_mutex> h(m_handleMu);
    if (m_wallet) {
        MONERO_Wallet_store(m_wallet, "");
        MONERO_WalletManager_closeWallet(m_wm, m_wallet, true);
        m_wallet = nullptr;
    }
}
