// Which monero_c const char* returns are heap-allocated (must be freed) and which are not?
// Each candidate runs in a forked child, so an invalid free aborts only that child.
#include "monero_wallet2_api_c.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static void* g_wm = nullptr;
static void* g_w  = nullptr;

// Returns true if freeing the pointer survived.
static bool probe(const char* name, const char* (*get)()) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        const char* p = get();
        std::string copy = p ? p : "(null)";
        if (p) MONERO_free(const_cast<char*>(p));   // the thing under test
        _exit(0);
    }
    int st = 0; waitpid(pid, &st, 0);
    bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  %-46s free() -> %s\n", name, ok ? "SAFE" : (WIFSIGNALED(st) ? "ABORTED (must NOT be freed)" : "exit!=0"));
    return ok;
}

static void* g_pt = nullptr;

int main(int argc, char** argv) {
    const char* dir = argv[1]; const char* name = argv[2]; const char* pw = argv[3]; const char* node = argv[4];
    g_wm = MONERO_WalletManagerFactory_getWalletManager();
    std::string path = std::string(dir) + "/" + name;
    g_w = MONERO_WalletManager_openWallet(g_wm, path.c_str(), pw, 2 /*stagenet*/);
    if (!g_w) { printf("open failed\n"); return 1; }
    MONERO_Wallet_init(g_w, node, 0, "", "", false, false, "");
    printf("wallet open: status=%d\n", MONERO_Wallet_status(g_w));

    printf("\n-- control: functions we free everywhere and that have never crashed --\n");
    probe("MONERO_Wallet_address(0,0)", []{ return MONERO_Wallet_address(g_w, 0, 0); });
    probe("MONERO_Wallet_getSubaddressLabel(0,0)", []{ return MONERO_Wallet_getSubaddressLabel(g_w, 0, 0); });

    // A doomed transaction still yields a PendingTransaction object with a status/errorString.
    g_pt = MONERO_Wallet_createTransaction(g_w, MONERO_Wallet_address(g_w, 0, 0), "",
                                           1000000000000000ULL /* absurd */, 0, 0, 0, "", ",");
    printf("\npendingTx=%p status=%d\n", g_pt, g_pt ? MONERO_PendingTransaction_status(g_pt) : -1);
    if (g_pt) {
        printf("\n-- the two calls doCommit makes --\n");
        probe("MONERO_PendingTransaction_errorString", []{ return MONERO_PendingTransaction_errorString(g_pt); });
        probe("MONERO_PendingTransaction_txid(\",\")",  []{ return MONERO_PendingTransaction_txid(g_pt, ","); });
    }
    return 0;
}
