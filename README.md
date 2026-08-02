# Maze Connect

The **desktop client** for Maze Connect — the app that lets you manage
[Maze Linux](https://github.com/berk-kucuk/MazeLinux) from your phone.

Not a KDE Connect clone. KDE Connect already does clipboard, notifications
and find-my-device well, and those were removed from here rather than
reimplemented. What is left is what only this app can do: read the machine's
own dashboard, run commands it has been told about in advance, toggle
maze-guard's killswitches, and talk to Maze AI. File transfer stayed.

The wire protocol is custom (not KDE-Connect-compatible) and built around
mutual TLS 1.3, EC P-256 device identity and SAS-verified pairing. See
[`docs/PROTOCOL.md`](docs/PROTOCOL.md),
[`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md) and
[`docs/HANDOFF.md`](docs/HANDOFF.md) for the full design and where the work
currently stands.

The Android counterpart lives at
[Maze-Connect-Mobile](https://github.com/berk-kucuk/Maze-Connect-Mobile).

## Status

The transport is done and verified against the real Android client: LAN
discovery, SAS-verified pairing, mutual-TLS 1.3 with public-key pinning,
automatic reconnect to paired devices, and file transfer. Covered by 10 test
suites (`ctest`), including end-to-end TLS tests asserting that an unpaired
peer is refused in both directions, and golden vectors shared with the mobile
client.

**Verified end to end:** a real pairing between this client and
Maze-Connect-Mobile on an Android emulator produced the same six-digit code
on both screens, and both sides pinned the other's public key.

The management features are landing one at a time:

| | |
| --- | --- |
| `systemStatus` — dashboard | code complete, real-device pass pending |
| `commands` — desktop-defined allow-list | not started |
| `guardControl` — killswitch on/off | not started |
| `ai` — Maze AI chat | not started |

Every one of them is a **capability that is off by default and enabled per
device**. Pairing settles who a device is; it never settles what that device
may do. That holds even for the dashboard, which only reads — "only reads"
still covers the hostname, the local IP, the kernel, the hardware and which
security services are running.

The dashboard does not reimplement any probe. maze-tools already ships
`maze_status.py`, which maze-control-center uses, so a small helper
(`/usr/lib/mazeconnect/maze-connect-status`) prints one JSON snapshot and this
client forwards it. Without maze-tools installed the helper is absent, the
dashboard says so, and nothing else changes.

## Layout

| Path | Contents |
| --- | --- |
| `core/` | `mazeconnect-core` — protocol, crypto, pairing, transport, storage. No UI dependency, unit-testable. |
| `daemon/` | `mazeconnectd` — headless variant for `systemd --user` (`daemon/systemd/mazeconnectd.service`). Hosts the same `DeviceManager` as the GUI. Run one or the other, not both: they contend for the same port. |
| `app/` | `maze-connect` — the GUI. Hosts `DeviceManager` in-process. Frameless window with its own title bar, following the same glass/sidebar shell as `maze-control-center`. |
| `packaging/` | Arch `PKGBUILD`, the `maze-connect-status` helper, and `icons/` (the app icon and the ground-free mark used inside the app). |

## Design

The UI follows the Maze desktop suite, not the Maze website. The tokens come
from maze-tools' shared `maze_ui.py` — true-black glass panel with a 22px
radius, a 210px sidebar, soft-white pills, and colour reserved for status
(`#56C271` / `#E06666`). The window is frameless, as
`maze-control-center` and `maze-welcome` are, so the rounded panel is the
window edge rather than sitting inside a squared-off Plasma decoration.

The one deliberate exception is the pairing code, which is set as a
large mono readout: it is the only thing in the app whose correctness
depends on a person comparing it against another screen.

## Packaging

`./build.sh` produces an Arch package in `dist/`:

```sh
./build.sh                              # -> dist/maze-connect-0.4.0-1-x86_64.pkg.tar.zst
./build.sh --install                    # build, then pacman -U it
./build.sh --repo ../MazeLinux/localrepo   # build, then add to the ISO's local repo
```

The package runs the test suite in `check()`, so a regression in pairing,
pinning or path confinement stops the build instead of shipping. `dist/` is
gitignored — publish the artifact through releases rather than committing it.

## Building

Requires Qt 6.5+ (Core, Gui, Qml, Quick, QuickControls2, Test) and CMake 3.25+.

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

Run the UI directly during development:

```sh
./build/app/maze-connect
```
