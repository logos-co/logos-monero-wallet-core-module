// The engine sending through a public node over Tor, and back to the wallet's own node. One
// private stagenet node (chaind) stands behind two logging fronts (tools/tor-modes/fronts.py): http plays the
// wallet's own node, socks plays Tor to a public node. The resolver stands in for the node
// module, so each step sets `send` as it needs. Prints one line per check; exits 1 on any failure.
//
// driver WALLETS_DIR RPC_PORT OWN_PORT OWN_LOG SOCKS_PORT SOCKS_LOG DEAD_PORT
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#define private public   // the checks read wallet2's trusted and offline flags off the handle
#include "wallet_runtime.h"
#undef private

extern "C" {
#include "monero_wallet2_api_c.h"
}

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

const char* const kSeed = "velvet lymph giddy number token physics poetry unquoted nibs useful sabotage limits benches "
                          "lifestyle eden nitrogen anvil fewest avoid batch vials washing fences goat unquoted";
const char* const kOnion = "abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion";

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

// POST to the node itself, past both fronts, so neither logs it.
json rpc(int port, const std::string& path, const json& body) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(fd); return json(); }
    const std::string b = body.dump();
    const std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                            "Content-Length: " + std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b;
    send(fd, req.data(), req.size(), 0);
    std::string got;
    char buf[65536];
    for (ssize_t n; (n = recv(fd, buf, sizeof buf, 0)) > 0;) {
        got.append(buf, static_cast<size_t>(n));
        const auto head = got.find("\r\n\r\n");
        const auto cl = got.find("Content-Length: ");
        if (head != std::string::npos && cl != std::string::npos && cl < head
            && got.size() >= head + 4 + std::stoul(got.substr(cl + 16))) break;
    }
    close(fd);
    const auto head = got.find("\r\n\r\n");
    return head == std::string::npos ? json() : json::parse(got.substr(head + 4), nullptr, false);
}

std::vector<std::string> lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);) out.push_back(l);
    return out;
}

std::vector<std::string> since(const std::string& path, size_t from) {
    auto all = lines(path);
    return from < all.size() ? std::vector<std::string>(all.begin() + static_cast<long>(from), all.end())
                             : std::vector<std::string>();
}

bool any(const std::vector<std::string>& ls, const std::string& needle) {
    for (const auto& l : ls) if (l.find(needle) != std::string::npos) return true;
    return false;
}

std::string joined(const std::vector<std::string>& ls) {
    std::string s;
    for (const auto& l : ls) s += (s.empty() ? "" : " | ") + l;
    return s;
}

json job(WalletRuntime& rt, const std::string& kind, const json& params) {
    const json s = rt.startJob(kind, params);
    if (!s.value("ok", false)) return s;
    const std::string id = s["jobId"], rc = s["receipt"];
    for (int i = 0; i < 1200; ++i) {
        const std::string st = rt.jobStatus(id, rc).value("state", "");
        if (st == "done" || st == "failed") break;
        std::this_thread::sleep_for(100ms);
    }
    json r = rt.jobResult(id, rc);
    rt.ackJob(id, rc);
    return r;
}

bool inPool(int rpcPort, const std::string& txid) {
    const json p = rpc(rpcPort, "/get_transaction_pool_hashes", json::object());
    for (const auto& h : p.value("tx_hashes", json::array())) if (h == txid) return true;
    return false;
}

template <class F> bool within(std::chrono::seconds limit, F f) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(250ms);
    }
    return f();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 8) {
        std::fprintf(stderr, "usage: driver WALLETS_DIR RPC_PORT OWN_PORT OWN_LOG SOCKS_PORT SOCKS_LOG DEAD_PORT\n");
        return 2;
    }
    const std::string dir = argv[1], ownLog = argv[4], socksLog = argv[6];
    const int rpcPort = std::stoi(argv[2]);
    const std::string ownUrl = std::string("http://127.0.0.1:") + argv[3];
    const std::string socks = std::string("socks5h://127.0.0.1:") + argv[5];
    const std::string dead = std::string("socks5h://127.0.0.1:") + argv[7];
    const std::string publicUrl = std::string("http://") + kOnion + ":18089";
    const std::string publicHost = std::string(kOnion) + ":18089";

    std::mutex mu;
    json node{{"url", ownUrl}, {"trusted", true}, {"torUse", "none"}, {"route", "direct"}};
    auto setSend = [&](const json& send) {
        std::lock_guard<std::mutex> g(mu);
        if (send.is_null()) node.erase("send"); else node["send"] = send;
    };
    WalletRuntime rt([](const std::string&, const json&) {},
                     [&](const std::string&) { std::lock_guard<std::mutex> g(mu); return node; });
    rt.setWalletsDir(dir);
    MONERO_WalletManagerFactory_setLogCategories("*:FATAL");   // wallet2 logs to stdout, among the checks

    // ── A funded wallet on its own node ──
    const json opened = job(rt, "restore_from_seed", {{"name", "sv"}, {"password", "pw"}, {"network", "stagenet"},
                                                      {"seed", kSeed}, {"restoreHeight", 0}});
    check(opened.value("ok", false), "open on your node: " + opened.dump());
    if (!opened.value("ok", false)) return 1;
    const std::string addr = opened["result"]["address"];
    // Coinbase unlocks after 60 blocks; 90 leaves 30 spendable.
    rpc(rpcPort, "/start_mining", {{"miner_address", addr}, {"threads_count", 1}, {"do_background_mining", false},
                                   {"ignore_battery", true}});
    check(within(300s, [&] { return rpc(rpcPort, "/get_height", json::object()).value("height", 0) >= 90; }), "mined 90 blocks");
    rpc(rpcPort, "/stop_mining", json::object());
    auto unlocked = [&] { const std::string s = rt.unlockedBalance(0); return s.empty() ? 0ULL : std::stoull(s); };
    check(within(180s, [&] { return rt.status().value("synchronized", false) && unlocked() > 10000000000000ULL; }),
          "funded: unlocked " + rt.unlockedBalance(0));
    check(rt.status().value("sendsVia", "") == "node", "status before: sendsVia node");

    auto build = [&](const std::string& label) {
        const json t = job(rt, "create_transaction", {{"address", addr}, {"amount", "1000000000000"}});
        check(t.value("ok", false), label + " build: " + (t.value("ok", false) ? t["result"].value("txids", "") : t.dump()));
        return t.value("ok", false) ? t["result"] : json::object();
    };
    auto flagsRestored = [&](const std::string& label) {
        std::shared_lock<std::shared_timed_mutex> h(rt.m_handleMu);
        check(MONERO_Wallet_trustedDaemon(rt.m_wallet) && !MONERO_Wallet_isOffline(rt.m_wallet),
              label + ": back on your node, trusted and online");
    };
    // The wallet keeps syncing from its own node, and the public node hears nothing more.
    auto syncsFromOwn = [&](const std::string& label) {
        const size_t own = lines(ownLog).size(), pub = lines(socksLog).size();
        check(within(20s, [&] { return lines(ownLog).size() > own; }), label + ": your node hears the refresh again");
        std::this_thread::sleep_for(5s);   // two more refresh rounds
        check(lines(socksLog).size() == pub, label + ": the public node hears nothing after the send");
    };

    // ── 1. Through the public node over Tor, three times ──
    setSend({{"url", publicUrl}, {"proxy", socks}});
    json t, c;
    size_t own = 0, pub = 0;
    for (int i = 1; i <= 3; ++i) {
        t = build("public");
        own = lines(ownLog).size();
        pub = lines(socksLog).size();
        c = job(rt, "commit_transaction", {{"txHandle", t.value("txHandle", "")}});
        std::printf("commit: %s\n", c.dump().c_str());
        check(c.value("ok", false) && c["result"].value("via", "") == "public"
              && c["result"].value("sendHost", "") == publicHost, "via public, sendHost " + publicHost);
        const auto heard = since(socksLog, pub);
        bool onlyBroadcast = any(heard, "/sendrawtransaction");
        for (const auto& l : heard)
            onlyBroadcast &= l == "connect " + publicHost || l == "tls refused" || l == "/sendrawtransaction";
        check(onlyBroadcast, "the public node heard the broadcast alone: " + joined(heard));
        check(!any(since(ownLog, own), "sendrawtransaction"), "your node was not asked to broadcast");
        check(inPool(rpcPort, t.value("txids", "-")), "in the pool");
        check(rt.status().value("sendsVia", "") == "public" && rt.status().value("sendHost", "") == publicHost,
              "status: sendsVia public, sendHost");
        flagsRestored("after the public send");
        syncsFromOwn("after the public send");
    }

    // ── 2. No SOCKS port yet: refused, never dialled in the clear ──
    for (const json& send : {json{{"url", publicUrl}, {"proxy", "tor"}}, json{{"url", publicUrl}}}) {
        setSend(send);
        t = build("no proxy");
        pub = lines(socksLog).size();
        c = job(rt, "commit_transaction", {{"txHandle", t.value("txHandle", "")}});
        check(!c.value("ok", true) && c.value("error", "").rfind("waiting_for_tor:", 0) == 0,
              "proxy " + send.value("proxy", std::string("absent")) + ": " + c.value("error", ""));
        check(lines(socksLog).size() == pub && !inPool(rpcPort, t.value("txids", "-")), "nothing sent");
        job(rt, "dispose_transaction", {{"txHandle", t.value("txHandle", "")}});
    }

    // ── 3. The public node fails: its error, nothing sent, no fallback ──
    setSend({{"url", publicUrl}, {"proxy", dead}});
    t = build("unreachable");
    own = lines(ownLog).size();
    c = job(rt, "commit_transaction", {{"txHandle", t.value("txHandle", "")}});
    check(!c.value("ok", true) && c.value("error", "").rfind("public node " + publicHost + ":", 0) == 0,
          "unreachable: " + c.value("error", ""));
    check(!inPool(rpcPort, t.value("txids", "-")), "nothing sent");
    check(!any(since(ownLog, own), "sendrawtransaction"), "no fallback to your node");
    flagsRestored("after the failed send");
    syncsFromOwn("after the failed send");
    job(rt, "dispose_transaction", {{"txHandle", t.value("txHandle", "")}});

    // ── 4. Binding to the public node fails (a port wallet2 cannot parse) ──
    setSend({{"url", std::string("http://") + kOnion + ":99999"}, {"proxy", socks}});
    t = build("bad node");
    pub = lines(socksLog).size();
    c = job(rt, "commit_transaction", {{"txHandle", t.value("txHandle", "")}});
    check(!c.value("ok", true) && c.value("error", "").rfind("public node ", 0) == 0, "bad node: " + c.value("error", ""));
    check(lines(socksLog).size() == pub && !inPool(rpcPort, t.value("txids", "-")), "nothing sent");
    flagsRestored("after the failed bind");
    syncsFromOwn("after the failed bind");
    job(rt, "dispose_transaction", {{"txHandle", t.value("txHandle", "")}});

    // ── 5. No `send` (an older node module): through your node, as before ──
    setSend(json());
    t = build("your node");
    own = lines(ownLog).size();
    pub = lines(socksLog).size();
    c = job(rt, "commit_transaction", {{"txHandle", t.value("txHandle", "")}});
    std::printf("commit: %s\n", c.dump().c_str());
    check(c.value("ok", false) && c["result"].value("via", "") == "node"
          && c["result"].value("sendHost", "") == ownUrl.substr(7), "via node, sendHost your node");
    check(any(since(ownLog, own), "/sendrawtransaction") && lines(socksLog).size() == pub, "your node broadcast it");
    check(inPool(rpcPort, t.value("txids", "-")), "in the pool");
    check(rt.status().value("sendsVia", "") == "node" && !rt.status().contains("sendHost"), "status: sendsVia node");

    job(rt, "close_wallet", json::object());
    rt.shutdown();
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
