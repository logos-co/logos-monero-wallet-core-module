// A private stagenet chain for send-via.sh: an offline monerod at difficulty 1, from the same
// Monero tree as the wallet (logos-monero-nix's monerod-c). Not regtest: wallet2 checks each
// block's fork version against its network's schedule, and regtest jumps to the last fork at
// height 1. chaind DATA_DIR RPC_PORT; runs until SIGTERM or SIGINT.
#include <csignal>
#include <cstdio>
#include <string>

#include "monerod_c.h"

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: chaind DATA_DIR RPC_PORT\n"); return 2; }
    const std::string dir = argv[1], port = argv[2];
    const std::string args = "[\"--stagenet\",\"--offline\",\"--fixed-difficulty\",\"1\",\"--no-zmq\","
                             "\"--data-dir\",\"" + dir + "\",\"--rpc-bind-ip\",\"127.0.0.1\","
                             "\"--rpc-bind-port\",\"" + port + "\",\"--log-level\",\"0\"]";
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);   // before the daemon's threads, which inherit it
    if (MONEROD_start(args.c_str()) != 0) {
        const char* e = MONEROD_last_error();
        std::fprintf(stderr, "chaind: %s\n", e ? e : "start failed");
        return 1;
    }
    int sig = 0;
    sigwait(&set, &sig);
    MONEROD_stop();
    return 0;
}
