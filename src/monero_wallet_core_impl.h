#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

class WalletRuntime;

/// In-process Monero wallet engine.
///
/// Wraps monero_c — the C ABI form of Monero's own `wallet2_api.h` — so the wallet family
/// inherits the reference wallet's semantics (seed, wallet file, subaddresses, sync,
/// transaction construction) with no subprocess and no bundled executable. The library is
/// LGPL-3.0 and is linked DYNAMICALLY as a separate shared object beside this plugin.
///
/// This is the ONLY module in which a wallet password or key exists. The password reaches
/// `openWallet` and is never persisted or cached; the seed leaves only as the explicit
/// return of revealSeed(), which re-checks the password first.
///
/// Every long wallet2 call (open, create, restore, close, createTransaction, commit) blocks
/// — an init against a remote node is ~2.6 s and a first scan is minutes — so they are
/// TICKETS served by a worker thread: startJob() → jobStatus() → jobResult() → ackJob().
/// Reads (balance, address, heights, history) answer directly.
///
/// `concurrency: "multi"`: methods may be dispatched concurrently; state is guarded inside.
class MoneroWalletCoreImpl : public LogosModuleContext {
public:
    MoneroWalletCoreImpl();
    ~MoneroWalletCoreImpl();

    // ── Tickets ──────────────────────────────────────────────────────────

    /// Queue a long operation. `kind` is one of: open_wallet, create_wallet,
    /// restore_from_seed, restore_from_keys, close_wallet, rescan, create_transaction,
    /// commit_transaction, dispose_transaction, change_password.
    ///
    /// `params` per kind (all strings unless noted):
    ///   open_wallet        { name, password, network }
    ///   create_wallet      { name, password, network, language? }
    ///   restore_from_seed  { name, password, network, seed, restoreHeight(number), seedOffset? }
    ///   restore_from_keys  { name, password, network, address, viewKey, spendKey?, restoreHeight(number) }
    ///   close_wallet       { }
    ///   rescan             { }
    ///   create_transaction { address, amount (decimal atomic units), priority(number 0-3)?, accountIndex(number)?, paymentId? }
    ///   commit_transaction { txHandle }
    ///   dispose_transaction{ txHandle }
    ///   change_password    { oldPassword, newPassword }
    ///
    /// Returns `{ jobId, receipt }`. The receipt is required by every follow-up call so a
    /// job id alone (which crosses the event plane) authorises nothing.
    StdLogosResult startJob(const std::string& kind, const LogosMap& params);

    /// `{ ok, state: "queued"|"running"|"done"|"failed", progress, error }`.
    LogosMap jobStatus(const std::string& jobId, const std::string& receipt);

    /// The job's result, once. Fails while the job is not done, and after it was acked.
    StdLogosResult jobResult(const std::string& jobId, const std::string& receipt);

    /// Forget a finished job. Returns false for an unknown id or a wrong receipt.
    bool ackJob(const std::string& jobId, const std::string& receipt);

    /// Cancel a queued job. A running wallet2 call cannot be interrupted; returns false then.
    bool cancelJob(const std::string& jobId, const std::string& receipt);

    // ── Reads — answer directly ──────────────────────────────────────────

    /// @code{.json}
    /// { "state": "no_wallet|opening|syncing|ready|closing|failed",
    ///   "wallet": string, "network": string, "address": string,
    ///   "connected": bool, "synchronized": bool,
    ///   "walletHeight": number, "daemonHeight": number,
    ///   "watchOnly": bool, "lastError": string, "libraryVersion": string }
    /// @endcode
    LogosMap status();

    /// Balances in ATOMIC UNITS as decimal strings (1 XMR = 1e12; a JSON number loses
    /// precision above 2^53).
    std::string balance(int64_t accountIndex);
    std::string unlockedBalance(int64_t accountIndex);

    /// The address at (account, index). (0, 0) is the primary address.
    std::string address(int64_t accountIndex, int64_t addressIndex);

    /// `[ { index, address } ]` for one account.
    LogosList subaddresses(int64_t accountIndex);

    /// Add a subaddress to an account. Returns `{ index, address }`.
    StdLogosResult createSubaddress(int64_t accountIndex, const std::string& label);

    /// `[ { txid, direction: "in"|"out", amount, fee, height, confirmations, timestamp,
    ///      pending, failed, unlockTime, paymentId } ]`, amounts as decimal strings.
    LogosList history();

    /// Whether `addr` is valid for `network` (mainnet|stagenet|testnet|regtest).
    bool addressValid(const std::string& addr, const std::string& network);

    /// The 25-word seed, after re-checking `password` against the open wallet's keys file.
    /// The only way key material leaves this module. Never logged.
    StdLogosResult revealSeed(const std::string& password);

    /// The secret view key, same discipline as revealSeed().
    StdLogosResult revealViewKey(const std::string& password);

    /// Wallet names present in this instance's wallets directory.
    LogosList listWallets();
    bool walletExists(const std::string& name);

    /// This module's version and the pinned monero_c / Monero version.
    LogosMap libraryVersion();

logos_events:
    /// Emitted on every state transition with the same shape as status().
    void walletStateChanged(const std::string& payloadJson);
    /// A ticket reached a terminal state: `state` is "done" or "failed".
    void jobFinished(const std::string& jobId, const std::string& state);

protected:
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;

private:
    std::unique_ptr<WalletRuntime> m_rt;
};
