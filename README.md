# CSGC — Counter-Strike: Game Coordinator

![Status](https://img.shields.io/badge/status-minimal%20working-yellow) ![License](https://img.shields.io/badge/license-GPL--3.0-green) ![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20x86-blue)

A standalone client-side replacement for the CS:GO Legacy Game
Coordinator. Redirects all GC traffic from the game to a
community-hosted TCP server.

> [!WARNING]
> **This project is in a minimal working state.**
> The GC hook works and the client receives `CMsgClientWelcome`,
> but matchmaking and inventory are not fully functional yet.
> Expect bugs, crashes, and missing features.

## What is this?

CSGC is a **client** — a `csgc.dll` plus a launcher that hooks into
CS:GO Legacy and replaces the official `ISteamGameCoordinator`
interface. Instead of talking to Valve's CS2 GC servers, the game
talks to **your** server.

**This repo does NOT include the server.** The server is a separate
project:

-> **[CSGO-GC-Replacement](https://github.com/aka3257/CSGO-GC-Replacement)** — Node.js server that implements the GC protocol.

## Installation

1. Download the latest prebuilt release from Releases.
2. Extract zip into your CS:GO Legacy folder with **replace**.
3. Start the [CSGO-GC-Replacement](https://github.com/aka3257/CSGO-GC-Replacement) server.
4. Launch CS:GO through Steam.

## Building from source

Requirements:
- Visual Studio 2022 (Desktop C++ workload)
- CMake 3.20+
- Windows SDK

Run these commands:
1. ``mkdir build && cd build``
2. ``cmake .. -A Win32 -DCMAKE_BUILD_TYPE=Release``
3. ``cmake --build . --config Release``

## Credits

Inspired by [mikkokko/csgo_gc](https://github.com/mikkokko/csgo_gc).

## License

GNU General Public License v3.0 (GPL-3.0)

## Third-party licenses

### funchook (GPL-2.0-or-later with linking exception)

This project uses funchook, which is licensed under GPL v2 or later
with a linking exception.

- Source code: https://github.com/kubo/funchook
- License: https://github.com/kubo/funchook/blob/master/LICENSE
