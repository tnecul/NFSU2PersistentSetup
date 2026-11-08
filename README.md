# NFSU2PersistentSetup

Makes the **last dyno-committed setup** stick per car: whenever the game would
auto-load a race-type-specific setup (race entry, free roam, dyno test), the
plugin swaps the persisted setup in before the game applies it. Persisted state
survives game restarts and save loads via `NFSU2PersistentSetup.ini` next to
`SPEED2.EXE`.

Compatibility: the mod targets the **US v1.2** `SPEED2.EXE`
(`sha256 bc34932b36bb19194000690ec26edf964176856c31fb0978e01e61ee814eee9d`);
other regional builds or patch versions are untested and may not work.

## Files

- `plugin.cpp` / `hooks.S` — full source
- `NFSU2PersistentSetup.asi` — distributed in the release zip (`dist\` is built
  locally; the `.asi` is not committed to the repo)

## Install

1. Get `NFSU2PersistentSetup.asi` from the [Releases](../../releases) page.
2. Drop it into the game's `scripts\` folder (needs an ASI loader; if your other
   `.asi` mods work, this one will too).

## Usage

- Enter the dyno and edit the car until the setup is what you want. Then
  choose the **Mode Select** item in the dyno menu, answer **Yes** to
  the store prompt, and **exit the dyno** — the plugin captures the setup at
  that commit point and writes it to `NFSU2PersistentSetup.ini`.
- Until that first store nothing is captured and the game behaves stock.
- The stored setup is then applied whenever the game would auto-load a
  race-type-specific setup (race entry, free roam, dyno reopen) — across
  restarts and save loads.
- To change it, edit and store again on the dyno (the latest store wins). To
  un-store a car, delete its `[Car_]` section from the ini while the game is
  closed.
- Applies in career gameplay only when `CareerOnly=1` (default).

## Config

`NFSU2PersistentSetup.ini` and `NFSU2PersistentSetup.log` live in the `scripts\`
folder next to the `.asi`. A missing ini is auto-generated:

```ini
[General]
CareerOnly=1     ; 1 = plugin only acts in career gameplay, 0 = everywhere
DebugLog=0       ; 1 = write NFSU2PersistentSetup.log (off by default)
```

Per-car sections are appended on every dyno commit:

```ini
[Car_319A8CA1]
Payload=...82 hex chars (41 bytes, byte-exact setup)...
```

Payload integrity is validated by hex decode; sections failing it are
ignored.

## How it works (verified against SPEED2.EXE, base 0x400000)

| Address | Role |
|---|---|
| `0x50FC20` | dyno commit: copies 41 bytes from editor buffer `[this+0x900]+0x14` into slots bank `this+0x904+slot*0x29`. **Hooked:** capture into INI (per current car id `0x863480`). |
| `0x516AC0` | race/dyno entry: copies 41 bytes from `this+0x348+mode*0x29` to `dstCar+0x398`. **Hooked:** before the copy, the source bank is replaced with the persisted payload (when conditions hold), so the copy and its follow-up call see our bytes. |
| `0x850078` | app mode global: `1` = career gameplay, `2` = main menu/quick race, `4` = 2P, `8`/`0x100` = online. Runtime-verified. |
| `0x863480` | active car id — verified stable across full restarts, changes when the selected car changes. |
| `0x83A63C` | setup selector; `0xFF` = no dyno editing session (dyno-inactive guard so the plugin never fights live sliders). |

Calls happen only in career when `CareerOnly=1`: capture only while the app
mode is `1`, force only while it is `1`, and never while a dyno editing
session is open.

Implementation: raw prolog patches (5/7 bytes, `E9` jumps) into naked stubs
(`hooks.S`) which run the plugin logic then jump to hand-built trampolines
(conditional re-encoded; original code preserved). Runtime-only, no game
files are modified on disk.

## License

Dedicated to the public domain via **CC0 1.0** (see `LICENSE`) — no rights
reserved; you can copy, modify, and redistribute freely, attribution optional.

## Approximation/limitations

- `FUN_00516AC0` is subject to any game logic that might assume a *specific*
  source bank content; if a race applies "compare for cheating" that is
  exposed to the swapped payload, it will simply reflect the persisted setup.
- The dyno commit capture uses the editor buffer contents at the moment
  `FUN_0050FC20` is called — which is exactly what gets saved.
