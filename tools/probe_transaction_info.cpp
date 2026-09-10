// Settle ownership for the TransactionInfo getters, which need a wallet WITH history.
// Each candidate is called in a forked child that copies the string then frees it: a pointer
// we do not own aborts only the child.
#include "monero_wallet2_api_c.h"
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static void* g_w = nullptr;
static void* g_t = nullptr;

static void probe(const char* name, const char* (*get)()) {
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        const char* p = get();
        std::string copy = p ? p : "";
        if (p) MONERO_free(const_cast<char*>(p));
        _exit(0);
    }
    int st = 0; waitpid(pid, &st, 0);
    bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  %-46s free() -> %s\n", name, ok ? "SAFE (we own it: take())" : "ABORTS (engine owns it: borrow())");
}

int main(int argc, char** argv) {
    const char* dir = argv[1]; const char* pw = argv[2]; const char* node = argv[3];
    void* wm = MONERO_WalletManagerFactory_getWalletManager();
    std::string path = std::string(dir) + "/basecamp";
    g_w = MONERO_WalletManager_openWallet(wm, path.c_str(), pw, 2);
    if (!g_w) { printf("open failed\n"); return 1; }
    MONERO_Wallet_init(g_w, node, 0, "", "", false, false, "");
    printf("connected=%d\n", MONERO_Wallet_connected(g_w));
    // Same sequence the engine uses; refresh() alone does not advance.
    MONERO_Wallet_setAutoRefreshInterval(g_w, 2000);
    MONERO_Wallet_startRefresh(g_w);
    MONERO_Wallet_refreshAsync(g_w);
    printf("refreshing (until synchronized) ...\n"); fflush(stdout);
    for (int i = 0; i < 60; ++i) {
        const unsigned long long h = (unsigned long long)MONERO_Wallet_blockChainHeight(g_w);
        const unsigned long long d = (unsigned long long)MONERO_Wallet_daemonBlockChainHeight(g_w);
        printf("  wallet=%llu daemon=%llu synced=%d\n", h, d, MONERO_Wallet_synchronized(g_w));
        fflush(stdout);
        if (MONERO_Wallet_synchronized(g_w) && d > 0 && h + 1 >= d) break;
        sleep(3);
    }
    // Stop wallet2's own thread before forking: only the forking thread survives in a child.
    MONERO_Wallet_pauseRefresh(g_w);
    printf("height=%llu synchronized=%d\n",
           (unsigned long long)MONERO_Wallet_blockChainHeight(g_w), MONERO_Wallet_synchronized(g_w));
    printf("balance=%llu\n", (unsigned long long)MONERO_Wallet_balance(g_w, 0));

    void* hist = MONERO_Wallet_history(g_w);
    if (!hist) { printf("no history object\n"); return 1; }
    MONERO_TransactionHistory_refresh(hist);
    int n = MONERO_TransactionHistory_count(hist);
    printf("transactions in history: %d\n", n);
    if (n == 0) { printf("NO TRANSACTIONS — cannot probe TransactionInfo\n"); return 2; }
    g_t = MONERO_TransactionHistory_transaction(hist, 0);
    printf("\nprobing against transaction 0:\n");
    // read-only sanity first (no free), so we know the values are real
    const char* h = MONERO_TransactionInfo_hash(g_t);
    printf("  hash         = %s\n", h ? h : "(null)");
    const char* si = MONERO_TransactionInfo_subaddrIndex(g_t, ",");
    printf("  subaddrIndex = '%s'   (empty/garbage would mean the pointer is dead)\n", si ? si : "(null)");
    printf("  direction    = %d\n", MONERO_TransactionInfo_direction(g_t));
    printf("  amount       = %llu\n\n", (unsigned long long)MONERO_TransactionInfo_amount(g_t));

    probe("MONERO_TransactionInfo_hash",              []{ return MONERO_TransactionInfo_hash(g_t); });
    probe("MONERO_TransactionInfo_paymentId",         []{ return MONERO_TransactionInfo_paymentId(g_t); });
    probe("MONERO_TransactionInfo_description",       []{ return MONERO_TransactionInfo_description(g_t); });
    probe("MONERO_TransactionInfo_subaddrIndex(\",\")", []{ return MONERO_TransactionInfo_subaddrIndex(g_t, ","); });
    if (MONERO_TransactionInfo_transfers_count(g_t) > 0)
        probe("MONERO_TransactionInfo_transfers_address(0)", []{ return MONERO_TransactionInfo_transfers_address(g_t, 0); });
    else
        printf("  %-46s (no transfers recorded on this row — incoming)\n", "MONERO_TransactionInfo_transfers_address");
    return 0;
}
