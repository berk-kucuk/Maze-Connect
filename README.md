# Maze Connect

The **desktop client** for Maze Connect — your phone and your
[Maze Linux](https://github.com/berk-kucuk/MazeLinux) computer, linked over
the LAN with verified, pinned mutual TLS.

The Android counterpart lives at
[Maze-Connect-Mobile](https://github.com/berk-kucuk/Maze-Connect-Mobile).

## What it does

**On the computer, about the phone**

- **Dashboard of your phones** — battery (level, charging and how, temperature,
  health), storage, memory, network type and Wi-Fi signal, ringer mode, Do Not
  Disturb, power saver, model, Android version and uptime. Refreshed every few
  seconds while you look, once a minute otherwise; a phone out of reach keeps
  its last reading, labelled with its age.
- **Find my phone** — rings it at full volume even on silent, from the
  dashboard, the Devices page or the tray menu; the button follows the phone,
  so it turns back when someone taps *Found it*.
- **Low battery / fully charged** desktop notifications, once per cycle.
- **Text and links from the phone** land on the clipboard with a
  notification; a link opens only if you click it.
- **Send files, and your clipboard,** to a particular phone.
- The tray tooltip lists linked phones and their battery.

**On the phone, about the computer**

- Its dashboard (CPU, memory, disk, temperatures, security services,
  hardening score), a live status-bar reading and home-screen widgets.
- Media control of every MPRIS player and the system volume, with
  lock-screen controls.
- Commands from an allow-list defined here — never a shell.
- maze-guard killswitches (camera, microphone, Wi-Fi, Bluetooth, USB).
- Chat with Maze AI through this computer's Ollama.
- Files both ways, from any app's Share menu.

Every feature is a **capability**, granted by pairing and revocable per
device; the phone's owner has their own switches for what the computer may
read from or do to the phone. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md),
[`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md) and
[`docs/HANDOFF.md`](docs/HANDOFF.md) for the design and where the work stands.

## Status

The transport is done and verified against the real Android client: LAN
discovery, SAS-verified pairing with a committed nonce exchange, mutual TLS
1.3 with public-key pinning, automatic reconnect, heartbeats, and file
transfer with path confinement.

15 test suites run in `ctest` (and in the package's `check()`), including
end-to-end TLS tests that an unpaired peer is refused in both directions,
golden vectors shared with the mobile client, and `tst_phonelink` — a phone
that is paired but misbehaves: answers nobody asked for, clipboard floods,
text crafted to disguise a link, ring results with no ring.

The dashboard does not probe this machine itself for the phone's benefit:
maze-tools' `maze_status.py` does, through a small helper
(`/usr/lib/mazeconnect/maze-connect-status`). Without maze-tools the phone's
dashboard says so and nothing else changes.

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

## Installation

### From the Maze repository

**On Maze Linux** the repository is already configured:

```bash
sudo pacman -S maze-connect
```

**On Arch Linux and Arch-based distributions**, add the repository once:

1. Import and trust the Maze signing key:

   ```bash
   curl -O https://mazerepo.berkkucukk.com.tr/packages/mazelinux.gpg
   gpg --show-keys --with-fingerprint mazelinux.gpg
   sudo pacman-key --add mazelinux.gpg
   sudo pacman-key --lsign-key 7C4D515A6B930CB04794CEF6147C8159B3E2EE5F
   ```

   The fingerprint `gpg` prints must be `7C4D 515A 6B93 0CB0 4794  CEF6 147C 8159 B3E2 EE5F`.

2. Add the repository to the end of `/etc/pacman.conf`:

   ```ini
   [mazelinux]
   SigLevel = Required DatabaseOptional
   Server = https://mazerepo.berkkucukk.com.tr/packages
   ```

3. Sync and install:

   ```bash
   sudo pacman -Syu maze-connect
   ```

Optionally install `mazelinux-keyring` as well; it keeps the signing key up to date through pacman.

Remove with `sudo pacman -Rns maze-connect`.

### Build from source

```bash
sudo pacman -S --needed base-devel git
git clone https://github.com/berk-kucuk/Maze-Connect.git
cd Maze-Connect
sudo pacman -S --needed $(bash -c 'source packaging/PKGBUILD; echo "${depends[@]}" "${makedepends[@]}"')
./build.sh --install
```

Without `--install` the package is only built, into `dist/`.

## Packaging

`./build.sh` produces an Arch package in `dist/`:

```sh
./build.sh                              # -> dist/maze-connect-1.3.0-1-x86_64.pkg.tar.zst
./build.sh --install                    # build, then pacman -U it
./build.sh --repo ../MazeLinux/localrepo   # build, then add to the ISO's local repo
```

The package runs the test suite in `check()`, so a regression in pairing,
pinning or path confinement stops the build instead of shipping. `dist/` is
gitignored — publish the artifact through the Maze repository rather than committing it.

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
