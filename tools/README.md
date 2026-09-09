# probe_ownership.cpp

Which `const char*` returns from monero_c are malloc'd copies the caller must free, and which
are interior pointers that must not be? The header documents neither, and getting it wrong is
not a leak — it is `___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED`, an
abort that takes the module down with whatever wallet was open. It killed a real send *after*
the transaction had been relayed, so the surface could not even say what had happened.

Each candidate is called in a forked child that copies the string and then frees it. A child
that exits cleanly means the pointer is owned by us (`take()` in wallet_runtime.cpp); a child
killed by a signal means it is the engine's (`borrow()`).

```bash
clang++ -std=c++17 -I../lib probe_ownership.cpp -o probe \
  /path/to/libmonero_wallet2_api_c.dylib -Wl,-rpath,/path/to
./probe <wallets-dir> <wallet-name> <password> http://node.monerodevs.org:38089
```

Measured against 0.18.4.6-RC2:

| call | free() |
|---|---|
| `MONERO_Wallet_address` | SAFE |
| `MONERO_Wallet_getSubaddressLabel` | SAFE |
| `MONERO_PendingTransaction_errorString` | SAFE |
| `MONERO_PendingTransaction_txid(",")` | **ABORTS — must not be freed** |

`MONERO_TransactionInfo_subaddrIndex(",")` is the other call with a separator argument and is
treated as borrowed; proving it needs a wallet with transaction history.
