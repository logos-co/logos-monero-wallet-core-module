#pragma once
#include <string>

// Standalone Monero address validation: Monero base58 → varint prefix → keccak-256 checksum,
// with the prefix matched to the network. Needed because monero_c's MONERO_Wallet_addressValid
// hands its `int nettype` to wallet2_api's deprecated `addressValid(str, bool testnet)`
// overload, so a STAGENET address can never validate through it (P3 finding).
namespace monero_addr {
// network: mainnet | testnet | stagenet | regtest (regtest uses mainnet prefixes)
bool valid(const std::string& address, const std::string& network);
}
