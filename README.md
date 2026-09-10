# logos-monero-wallet-core-module

`monero_wallet_core_module` — the Logos Monero wallet family's **in-process engine**. It wraps
[monero_c](https://github.com/MrCyjaneK/monero_c) (the C ABI form of Monero's own `wallet2_api.h`),
so the wallet inherits the reference implementation's semantics — seed, wallet file, subaddresses,
sync, transaction construction — with **no subprocess and no bundled executable**.

This is the only module in which a wallet password or key exists. The password reaches
`openWallet` and is never persisted or cached; the seed leaves only as the explicit return of
`revealSeed()`, which re-checks the password first.

## Contract

Long wallet2 calls block (an init against a remote node is ~2.6 s, a first scan is minutes), so
they are **tickets** served by a worker thread: `startJob(kind, params)` → `jobStatus` →
`jobResult` → `ackJob`. Kinds: `open_wallet`, `create_wallet`, `restore_from_seed`,
`restore_from_keys`, `close_wallet`, `rescan`, `create_transaction`, `commit_transaction`,
`dispose_transaction`, `change_password`. Reads (`status`, `balance`, `address`, `subaddresses`,
`history`, `addressValid`, …) answer directly. Amounts are **decimal strings of atomic units**.

The node module (`monero_node_module`) owns the daemon endpoint and proxy policy; this module
reads it right before `init`. A network marked `proxyRequired` with no proxy **refuses to
open** — and because wallet2 honours the proxy but raises no error when the node is unreachable,
a required proxy that cannot connect also fails the open rather than sitting silently
disconnected.

## The library

`nix/monero-c.nix` fetches the pinned upstream release bundle (sha256-pinned), extracts one
`libmonero_wallet2_api_c.{dylib,so,dll}` per target (darwin arm64/x64, linux arm64/x64,
mingw x64), and lays it out as `lib/` + `include/` + the license. It is **LGPL-3.0** and is
linked **dynamically** as a separate shared object beside the plugin, never statically —
a user may substitute a modified copy by replacing that file.

The license text travels with it: the derivation installs `LICENSE.monero_c` into its `lib/`
(the only place the builder's `include` staging looks) and `metadata.json` names it in
`include`, so it lands beside the plugin in the payload and inside the `.lgx`. `ci.yml`
asserts both — the library present as its own file, and the license byte-identical to this
repo's `LICENSE.monero_c`.

## Build

```bash
nix build              # the module; also exposes .#monero-c for the bare library
```
