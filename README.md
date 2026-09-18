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
reads it (`effective_node`) right before `init`. In local mode that is the node `monerod_module`
runs on this device: loopback, so trusted. A network marked `proxyRequired` with no proxy **refuses to
open** — and because wallet2 honours the proxy but raises no error when the node is unreachable,
a required proxy that cannot connect also fails the open rather than sitting silently
disconnected.

## The library

`libmonero_wallet2_api_c.{dylib,so,dll}` is built from source by
[logos-monero-nix](https://github.com/logos-co/logos-monero-nix) for every target (darwin arm64/x64, linux arm64/x64,
mingw x64): Monero at monero_c's pin with monero_c's patches, and monero_c's own wallet2 shim.
It exports the same `MONERO_*` API as the upstream prebuilt it replaced (357 on Linux and
Windows, 354 on darwin). It is **LGPL-3.0** and is linked **dynamically** as a separate shared
object beside the plugin, never statically — a user may substitute a modified copy by replacing
that file, and the exact source and build recipe are public.

Some antivirus products may flag Monero components. See Monero's
[antivirus FAQ](https://web.getmonero.org/get-started/faq/#antivirus) for more information.

The license texts travel with it: the library installs `LICENSE.monero_c` (LGPL-3.0) and
`LICENSE.monero` (Monero's BSD-3) into its `lib/` (the only place the builder's `include`
staging looks) and `metadata.json` names both in `include`, so they land beside the plugin in
the payload and inside the `.lgx`. `ci.yml` asserts all three — the library present as its own
file, `LICENSE.monero_c` byte-identical to this repo's copy, and `LICENSE.monero` present.
The headers in `lib/` are byte-identical to the library's own; the build uses the library's.

## Build

```bash
nix build              # the module; also exposes .#monero-c for the bare library
```
