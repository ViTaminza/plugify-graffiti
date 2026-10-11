# graffiti — CS2 graffiti for everyone, sprayed by the server (Plugify port)

Port of the Metamod:Source plugin cs2-graffiti (by ezekutor). The server creates a
`player_spray_decal` with a design (sticker kit `spray_*`) and a colour, so no inventory item is
needed; designs, colours and their localized names come from the game files, so new CS2 graffiti
appear in the menu by themselves.

| | |
|---|---|
| original | cs2-graffiti (1013 + 537 LOC of C++) |
| config | `configs/graffiti/graffiti.ini` — the production file |
| phrases | `configs/graffiti/translations/graffiti.phrases.txt` |
| commands | chat `!graffiti`, `!spray` (menu); console `mm_spray` (the `bind t mm_spray` key); `mm_graffiti_reload`, `mm_graffiti_clear`, `mm_graffiti_schema` (server console) |
| cookies | official permissions plugin, names of the original: `Graffiti.Kit`, `Graffiti.Tint`, `Graffiti.DoubleE` |
| workshop addons | none — the catalog comes from the game install |
| tests | `test/graffiti_core_tests.cpp` (build/tests/graffiti_tests): the production config, catalog parser on a format-faithful `items_game.txt` sample, localization parser, decal geometry, double-E detector |

## What is ported

* the whole config (menu/spray commands, `double_e` + window, cooldown, `max_distance`,
  `one_per_player`, `lifetime`, `sound`, `default_kit`, `default_tint`);
* the catalog: every `sticker_kits` block with `spray_*` entries, `graffiti_tints`, the
  `SprayKit_*` / `Attrib_SprayTintValue_*` localization of `resource/csgo_<lang>.txt` with the
  English fallback, the category grouping (`std`, `std2`, `std3`, `vcap1`, `illuminate1`, `other`)
  and the alphabetical order of the menu — all parsed from `scripts/items/items_game.txt`
  (`s2sdk::ReadFileVPK`);
* the menus (main, design by category with the `►` marker, colours, help), the `!`-commands and
  the console commands of the original;
* spraying on the world only: the aim trace is `s2sdk::TraceCollideable` (eyes → view direction ×
  `max_distance`, the pawn ignored) which also returns the surface normal the decal orientation
  needs, plus a `s2sdk::TraceLine` to keep the original's "no players, no moving props" rule;
* the decal's schema fields (`m_vecEndPos`, `m_vecStart`, `m_vecLeft`, `m_vecNormal`, `m_nPlayer`,
  `m_nTintID`, `m_unAccountID`, `m_nUniqueID`, `m_rtGcTime`, `m_nEntity`, `m_nHitbox`,
  `m_flCreationTime`) written through the s2sdk schema setters (with the network update the
  original sent with `SetStateChanged`);
* the double-E spray with the defusing/hostage exclusion, the per-player sound on spray, the
  cooldown/lifetime bookkeeping and the `mm_graffiti_schema` diagnostics (field offsets of this
  build instead of the original's schema dump walk).

## Deliberate differences

1. **Cookies** through the official permissions plugin (same names, loaded by a once-per-second
   `permissions::LoadUser` reconcile).
2. **The utils settings-menu toggle** (`utils_settings`, "Graffiti with double E" inside the
   cs2-menus settings menu) is not ported — the Plugify menu layer has no settings-menu extension
   point. The switch inside the `!graffiti` menu is unchanged.

## Not ported

* nothing else — every key of the production config is honoured.

## Needs a check on a real server

* the decal geometry: the engine's `TraceCollideable` returns the surface normal, but the decal's
  `m_vecNormal`/`m_vecLeft` convention can only be confirmed by looking at a sprayed wall, floor
  and ceiling in game;
* `mm_graffiti_schema` prints the offsets of the fields this port writes — if a future CS2 update
  renames or removes one, the log lines make it obvious (the writes themselves degrade to no-ops).
