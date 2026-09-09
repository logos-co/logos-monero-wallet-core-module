#include "monero_wallet_core_impl.h"

#include <filesystem>

#include "logos_sdk.h"
#include "wallet_runtime.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
StdLogosResult fromJson(const json& j) {
    if (j.value("ok", false)) {
        json v = j; v.erase("ok");
        return {true, v.contains("result") ? v["result"] : v};
    }
    return {false, {}, j.value("error", std::string("unknown error"))};
}
}

MoneroWalletCoreImpl::MoneroWalletCoreImpl() {
    // Events are routed onto the generated typed emitters. Safe from the worker thread:
    // emitEventImpl_ is marshalled by the host and is a no-op outside a framework context.
    auto emit = [this](const std::string& name, const json& payload) {
        if (name == "walletStateChanged") walletStateChanged(payload.dump());
        else if (name == "jobFinished")   jobFinished(payload.value("jobId", ""), payload.value("state", ""));
    };
    // The node module owns endpoint + proxy policy; this module only reads it, right before
    // init, so a switch in the settings app takes effect on the next open.
    auto resolve = [this](const std::string& network) -> json {
        try {
            const std::string raw = modules().monero_node_module.get_node_config(network);
            const json j = json::parse(raw);
            if (!j.value("ok", false)) return json{{"error", j.value("error", "node not configured")}};
            return j.value("result", json::object());
        } catch (const std::exception& e) {
            return json{{"error", std::string("node module unreachable: ") + e.what()}};
        }
    };
    m_rt = std::make_unique<WalletRuntime>(emit, resolve);
}

MoneroWalletCoreImpl::~MoneroWalletCoreImpl() = default;

void MoneroWalletCoreImpl::onContextReady() {
    if (instancePersistencePath().empty()) return;
    const fs::path dir = fs::path(instancePersistencePath()) / "wallets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    m_rt->setWalletsDir(dir.string());
}

LogosShutdown MoneroWalletCoreImpl::aboutToUnload() {
    // Store + close are ~0.5 s against a remote node (P0-measured); a wallet left unstored
    // loses cache progress, so this is done before returning rather than deferred.
    m_rt->shutdown();
    return LogosShutdown::Synchronous;
}

StdLogosResult MoneroWalletCoreImpl::startJob(const std::string& kind, const LogosMap& params) {
    return fromJson(m_rt->startJob(kind, params));
}
LogosMap MoneroWalletCoreImpl::jobStatus(const std::string& jobId, const std::string& receipt) {
    return m_rt->jobStatus(jobId, receipt);
}
StdLogosResult MoneroWalletCoreImpl::jobResult(const std::string& jobId, const std::string& receipt) {
    return fromJson(m_rt->jobResult(jobId, receipt));
}
bool MoneroWalletCoreImpl::ackJob(const std::string& jobId, const std::string& receipt) {
    return m_rt->ackJob(jobId, receipt);
}
bool MoneroWalletCoreImpl::cancelJob(const std::string& jobId, const std::string& receipt) {
    return m_rt->cancelJob(jobId, receipt);
}

LogosMap MoneroWalletCoreImpl::status() { return m_rt->status(); }
std::string MoneroWalletCoreImpl::balance(int64_t a) { return m_rt->balance(static_cast<uint32_t>(a < 0 ? 0 : a)); }
std::string MoneroWalletCoreImpl::unlockedBalance(int64_t a) { return m_rt->unlockedBalance(static_cast<uint32_t>(a < 0 ? 0 : a)); }
std::string MoneroWalletCoreImpl::address(int64_t a, int64_t i) {
    return m_rt->address(static_cast<uint64_t>(a < 0 ? 0 : a), static_cast<uint64_t>(i < 0 ? 0 : i));
}
LogosList MoneroWalletCoreImpl::subaddresses(int64_t a) { return m_rt->subaddresses(static_cast<uint32_t>(a < 0 ? 0 : a)); }
StdLogosResult MoneroWalletCoreImpl::createSubaddress(int64_t a, const std::string& label) {
    return fromJson(m_rt->createSubaddress(static_cast<uint32_t>(a < 0 ? 0 : a), label));
}
LogosList MoneroWalletCoreImpl::history() { return m_rt->history(); }
bool MoneroWalletCoreImpl::addressValid(const std::string& addr, const std::string& network) {
    return m_rt->addressValid(addr, network);
}
StdLogosResult MoneroWalletCoreImpl::revealSeed(const std::string& password) { return fromJson(m_rt->revealSeed(password)); }
StdLogosResult MoneroWalletCoreImpl::revealViewKey(const std::string& password) { return fromJson(m_rt->revealViewKey(password)); }
LogosList MoneroWalletCoreImpl::listWallets() { return m_rt->listWallets(); }
bool MoneroWalletCoreImpl::walletExists(const std::string& name) { return m_rt->walletExists(name); }

LogosMap MoneroWalletCoreImpl::libraryVersion() {
    return json{{"module", MONERO_WALLET_CORE_VERSION}, {"monero_c", MONERO_C_VERSION},
                {"license", "LGPL-3.0 (monero_c, dynamically linked)"}};
}
