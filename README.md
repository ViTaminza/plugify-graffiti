# graffiti (Plugify)

CS2 graffiti for everyone, sprayed by the server (no inventory item): the plugin creates a player_spray_decal with the chosen design (sticker kit spray_*) and colour, ported from the Metamod:Source plugin 'graffiti' by ezekutor. Designs, colours and their localized names come from the game files (scripts/items/items_game.txt, resource/csgo_<lang>.txt), so new CS2 graffiti appear in the menu by themselves. Draw with a double tap of E (per-player switch) or with the console command mm_spray (bind t mm_spray); !graffiti / !spray open the menu (design by category, colour, double E). Config configs/graffiti/graffiti.ini, phrases configs/graffiti/translations/graffiti.phrases.txt; the per-player selection lives in the official permissions cookies (Graffiti.Kit / Graffiti.Tint / Graffiti.DoubleE). No exported methods.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, permissions (optional), hudmenu (optional)

## Installation
Download `graffiti-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/graffiti`.

## License
GPL-3.0, see LICENSE.

