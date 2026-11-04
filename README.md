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
open**. From a node module that predates "Use Tor for", a required proxy that cannot connect
within 10 s also fails the open, because wallet2 raises no error when the node is unreachable.

## Use Tor for

The node module's setting (`all`, `identifying` or `none`) decides where wallet2 connects.
`status()` reports it as `torUse` and `route`, with `onTor` and `waitingForTor`:

| route | when | wallet2 dials |
|---|---|---|
| `direct` | `none`, local mode, a node on this network | the node, no proxy |
| `catch_up` | `identifying`, from the open until synchronized | the direct node (`catchUp.url`), no proxy, untrusted |
| `waiting_for_tor` | a Tor route while the built-in Tor joins, or caught up before it is ready | nowhere: offline, its proxy an address that never connects |
| `tor` | `all` from the open; `identifying` after the catch-up | the node over Tor (onion), through the SOCKS port |

- **The switch** runs on the worker thread once wallet2 reports synchronized: pause the refresh,
  stop any in flight and wait it out offline, then `init` again on the onion node with the
  proxy, untrusted, and resume. Nothing more goes to the catch-up node after that; a route only
  ever moves toward Tor. A catch-up node that sends no new block for 90 s gives way to Tor for
  the catch-up too, whether it never answers or one batch takes it longer than that. That switch
  shuts the catch-up node's connection first: wallet2 cannot abandon a request, and a node that
  trickles its batch keeps each read inside wallet2's timeout. Reads keep answering through any
  switch, which waits for the request in flight under a shared lock.
- **On Tor, the node module is asked again every 60 s.** A seeded onion that stops answering is
  re-picked there (its health check marks it, the next `effective_node` measures the others);
  when the onion it hands back differs, the same switch moves wallet2 onto it.
- **`status()` never waits on the node.** wallet2 holds its daemon lock for a whole request, and
  `connected()` and the daemon's height take it, so a slow batch used to hold every `status()`
  past its IPC deadline. A probe thread now reads, every 3 s, `connected` off wallet2's own
  socket (a connection from this process to the node, or to the SOCKS port on a Tor route), and
  `daemonHeight` from wallet2 only while synchronized: mid catch-up it would wait behind the
  batch holding the wallet, and every job with it. `status()` reports that reading;
  `walletHeight` and `synchronized` need no lock. Until a height is read, the app shows the node
  module's.
- **Sends wait:** `create_transaction` and `commit_transaction` fail with an error starting
  `waiting_for_tor:` while the route is `catch_up` or `waiting_for_tor`, so decoys (`get_outs`)
  and the broadcast only ever go over Tor.
- **Lock:** an open reports `set_wallet_open(network, true)` to the node module and a close
  `false`, which locks "Use Tor for" and that network's node while the wallet is open. The
  first job after this module starts clears every network, as a lock left by an earlier
  instance would otherwise stay.
- With a node module from before this setting there is no `torUse` or `catchUp`: a proxy means
  `tor`, none means `direct`, as before.

`doctests/wallet-core-tor-modes.test.yaml` checks each mode against a mainnet node behind an
HTTP front (the direct node) and a SOCKS front (Tor, to a fake onion name), both of which log
every request: in `identifying`, a send fails `waiting_for_tor` while the catch-up is held, and
the direct node hears nothing after the switch; `stalled` holds the direct node's block requests
for good, and `status()` still answers at once and the engine finishes the catch-up over Tor; `all` never reaches the direct node, `none`
makes no SOCKS connection. Mainnet because its last checkpoint is near the tip, so a wallet
restored just below it is synchronized in a minute; nothing is broadcast. The wallet holds
nothing, so a funded send (decoys and the broadcast) is not exercised. The fronts and the script are in `tools/tor-modes/`; to run one
mode by hand, with `LOGOS` a logosctl bundle and `TREE` a modules directory holding tor_module,
the node module and this one:

```bash
LOGOS=./logos TREE=./tree/modules sh tools/tor-modes/modes.sh identifying
```

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
