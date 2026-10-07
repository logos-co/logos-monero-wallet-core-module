#pragma once
#include <string>

// Standalone Monero address validation: Monero base58 → varint prefix → keccak-256 checksum,
// with the prefix matched to the network. Written because monero_c's MONERO_Wallet_addressValid
// handed its `int nettype` to wallet2_api's deprecated `addressValid(str, bool testnet)`
// overload, so a STAGENET address could not validate through it (fixed in v0.18.5.3-RC1).
namespace monero_addr {
// network: mainnet | testnet | stagenet | regtest (regtest uses mainnet prefixes)
bool valid(const std::string& address, const std::string& network);
}
