# DQ9Recompiled

Static recompilation of **Dragon Quest IX: Sentinels of the Starry Skies**
(Europe, `YDQP`, SHA-1 `ff761d349709f329c8ac4bd0023fdce21861e8a1`), built on the
[ndsrecomp](https://github.com/birabittoh/ndsrecomp) framework.

**You must own the game.** This project does **not** ship any copyrighted code,
data, or assets: no ROM, BIOS, firmware or generated code. You provide your own
dump of the USA, PAL or JP (`YDQJ`, experimental) copy of the game.

The game runs as compiled code from its first instruction, with no interpreter,
on a recompiled FreeBIOS and a generated firmware.

Feel free to visit DQ9Recompiled's [official Discord server](https://discord.gg/DJe2pXMH7S) if you need any help.

|   |   |
|:---:|:---:|
| <img width="3840" height="2160" alt="battle" src="https://github.com/user-attachments/assets/44ba7ca2-5543-4fe5-8efb-a62c6e5a469e" /><br>Single-screen battle UI | <img width="3840" height="2160" alt="battle2" src="https://github.com/user-attachments/assets/e44a6eb4-1882-4b3e-ba49-b50d544cb549" /><br>Minimal when not issuing orders |
| <img width="1470" height="2112" alt="vertical" src="https://github.com/user-attachments/assets/5730ddef-47ae-4e60-bc19-40c610ed4046" /><br>Variable resolution support (including ultrawide) | <img width="1441" height="2160" alt="qol" src="https://github.com/user-attachments/assets/d89164d5-cb75-4a4f-b6b4-61799045a7f0" /><br>Quality of life options |

## Features

* **Skip intros**: boots past the logos and the opening movie.
* **Camera zoom**: Manual (mouse wheel, right stick, pinch to zoom) or Auto.
* **Unlocked camera rotation**: you can now rotate it freely.
* **UI size**: optimized text boxes and menus for big windows.
* **Language**: switch between English, French, German, Italian and Spanish
  without a restart. Japanese is available only with a JP ROM.
* **Any resolution**: 3D renders at any resolution, including ultrawide.
* **2nd screen position**: pieces of the secondary screen are drawn on top of
  the primary one during exploration and battles.
* **Mouse-friendly menus**: hover to highlight, one click to choose.
* **60 FPS mode**: Off, Fast or Accurate.
* **Analog movement**: the left stick walks at any angle.
* **Skip text**: press B, the right mouse button or double tap to print instantly.
* **Remappable keyboard**: every keyboard button can be remapped.
* **Touch buttons**: additional touch buttons with adjustable opacity.
* **Virtual stylus**: use the touch screen from a controller.
* **Local DLC**: Sellma's shop and DLC quests work without any external service.
* **Options**: every option is displayed natively in-game.
* **Multiplayer**: local wireless over the LAN, and online lobbies with Netplay,
  through the relay or, with "Online (alt)", direct STUN connections.

## Play

```bash
./DQ9Recompiled
```

On first launch it asks for your ROM and unpacks it into `assets/<code>` in the
data folder; your `.nds` can then be moved or deleted. You can also drag a ROM
over the executable to extract it.

Settings, saves and the unpacked ROM live in a per-user folder, or
next to the executable if a `portable.txt` file is there:

| Platform | Folder |
| --- | --- |
| Linux | `~/.local/share/DQ9Recompiled` |
| Windows | `Documents\DQ9Recompiled` |
| macOS | `~/Library/Application Support/DQ9Recompiled` |

Logs go to `logs/` next to the executable. On macOS, or if that folder
cannot be written, they go to `logs/` in the folder above.

At startup the game checks GitHub for a newer release, offered on the title
screen. To skip that, set `check_for_updates = false` under `[system]` in
`settings.toml`, or launch with `--no-update-check`.

### Multiplayer

On first launch Windows asks whether to let the game through its firewall;
allow it on private networks. On Linux, open the ports yourself if a
firewall is on (`sudo ufw allow 26710:26725/udp`, or
`sudo firewall-cmd --add-port=26710-26725/udp`). If the
others still don't show up (networks that block broadcast, or players
reached through a VPN such as Tailscale), add `--local-wireless-peer <IP>`
for each of the other PCs.

**Local wireless**: games running on the same network find each other. One player
opens the Rapportal and the others join from the inn in Stornway. Allow the
runner through your firewall (UDP ports 26710-26725). If peers do not show up,
pass `--local-wireless-peer <IP>`.

Two copies can play on one PC: `--instance 0` and `--instance 1` (separate saves).

## Building from scratch

You need Python 3, CMake, Ninja, clang and SDL3.

```bash
git clone --recursive https://github.com/birabittoh/DQ9Recompiled
cd DQ9Recompiled
```

Put your ROM in the repository root as `dq9.nds`, then:

```bash
python3 scripts/build.py
```

The first build unpacks `dq9.nds` into `assets/YDQP/` and builds from that
folder from then on (`dq9.nds` is no longer needed). A runner started from
the repository finds `assets/YDQP/` on its own.

`python3 scripts/build.py --package` then packs the build as the release asset
the updater looks for (`DQ9Recompiled-<tag>-<platform>.zip`, `.tar.gz` on Linux,
`.dmg` on macOS). The version a build compares releases against is the newest
`v*` tag (or `--version X.Y.Z`, or `NDS_VERSION_NAME`); with none, the updater
is off.

If you already cloned without `--recursive`, run
`git submodule update --init --recursive` first.

This sets up a virtual environment, generates the banks from the ROM, builds the
runner and copies it to `DQ9Recompiled[.exe]`. The bank compile is large (about 1 GB
of C), so keep `--jobs` at 6 or below on a small machine. `--help` lists the
other options (`--release`, `--no-online`, `--no-hooks`, ...).

## Credits

* [ndsrecomp](https://github.com/mstan/ndsrecomp)
* [recomp-net](https://github.com/RetroPortingToolKit/recomp-net)
* [melonDS](https://melonds.kuribo64.net/)
* [dqix-decomp](https://github.com/dqix/dqix-decomp)

## License

The source in this repository is available under the MIT License; see
[`LICENSE`](LICENSE).

A built `nds_runner` links GPL-3.0-or-later code from melonDS, so it can only be
distributed under **GPL-3.0-or-later**; see [`LICENSE-GPL`](LICENSE-GPL). If you
distribute a build, ship `LICENSE-GPL` with it and make the corresponding source
available. The submodules carry their own licenses.
