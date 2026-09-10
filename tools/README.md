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

Measured against 0.18.4.6-RC2, on a wallet with a real incoming transaction:

| call | `MONERO_free` | in our code |
|---|---|---|
| `MONERO_Wallet_address` | SAFE | `take()` |
| `MONERO_Wallet_getSubaddressLabel` | SAFE | `take()` |
| `MONERO_PendingTransaction_errorString` | SAFE | `take()` |
| `MONERO_TransactionInfo_hash` | SAFE | `take()` |
| `MONERO_TransactionInfo_paymentId` | SAFE | `take()` |
| `MONERO_TransactionInfo_description` | SAFE | `take()` |
| `MONERO_TransactionInfo_subaddrIndex(",")` | SAFE | `take()` |
| `MONERO_PendingTransaction_txid(",")` | **ABORTS** | `borrow()` |

**There is no rule to infer from a signature.** The tempting heuristic — that a `separator`
argument means the function joins a list into a temporary and hands back a dangling `.c_str()` —
was tested and is false: `TransactionInfo_subaddrIndex` takes a separator and is an ordinary
owned copy, and reads back correctly (`'0'`). `PendingTransaction_txid` is so far the only
exception, and it is not merely unfreeable: the value it returns is unusable, which is why
`doCommit` derives the txid from the wallet's own history instead.

Not yet measured under this probe: `MONERO_TransactionInfo_transfers_address`, which needs a row
with recorded destinations (an outgoing transfer). It is `take()`n today, and the history loop
calls it for every outgoing row on every refresh — exercised continuously in a live session with
several outgoing transactions and zero aborts — but that is observation, not a controlled test.
