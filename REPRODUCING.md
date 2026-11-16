# Reproducing the crash

## What has to happen

1. **Load an area.** An exterior cell is loaded with a reference in it that the worldspace also
   tracks as persistent: in `TESWorldSpace::mobilePersistentRefs` or, per cell, in
   `fixedPersistentRefMap`. Both hold counted pointers, so the worldspace keeps those references
   alive on its own.
2. **Leave until the cell is destroyed, not just unloaded.** Unloaded exterior cells stay in the
   cell buffer (`uExterior Cell Buffer`, 36 by default) until enough other cells push them out, or
   until fast travel unloads everything. When one is destroyed, its destructor drops its references
   but never clears their `parentCell`. Every persistent reference of that cell now points at freed
   memory; one fast travel left about 50 that way, mostly ordinary markers and NPCs.
3. **Something touches a bad one.** Either:
   - **Come back to the same cell.** The loader (ID 20532) puts the cell's persistent references
     back into it. With a non-null `parentCell` it first reads a flag from that cell and removes the
     reference from it: crash in `TESObjectCELL::AddReference` / `GetEncounterZone` on a
     cell-loading worker thread.
   - **Cross a location boundary.** A Change Location story event starts radiant quests whose alias
     conditions test nearby references; reading the freed cell's worldspace or location crashes in
     `BGSLocation::IsChild` / `TESWorldSpace::GetLocationFromCoordinates` on a job thread.

Every crash so far was on a **leftover**: a reference with formID 0, or a runtime-created one
(`FF......`), with the deleted flag (`0x20`). A script or mod deleted the object, but it was still
in the worldspace's persistent list. Live references are left stale just as often and have not
crashed, so the fix only clears leftovers.

## Why it is hard to hit

Step 2 needs real distance (enough new cells to push the old one out of the buffer) or a fast
travel, and step 3 needs a leftover in that cell to be used afterwards. Moving fast covers distance
quickly, but speed itself is not needed: returning to the cell after it was destroyed is. A
character with many radiant quests ready to start (bounties picked up, Missives, quest mods like
MoreToSay) makes the quest variant much more likely than a fresh character does.

## Known trigger areas

### The mist near Tundra Homestead (cell `000095F7`)

- Record: vanilla reference `000DA6AD` (Skyrim.esm), base `00077773` `FXMistLow01HalfVis` (MSTT),
  at (39160, -6468, -4960) in Wilderness cell `000095F7`, grid (9, -2), next to Tundra Homestead
  east of Whiterun. No plugin overrides the reference.
- `_NoMoreMist_SWAP.ini` (No More Mist, Base Object Swapper) swaps its base to `AlphaTest`
  (`00058E92`, STAT). In game the stale object is a formID-0, deleted-flagged (flags `0x2B`)
  reference at that spot, base `00058E92`, in the mobile persistent list.
- How the swapped reference becomes a formID-0 deleted leftover is not known yet.

1. Go to Tundra Homestead so cell (9, -2) loads.
2. Leave far enough for the cell to be destroyed; stress mode does it at the next location boundary.
3. Come back. The loader re-adds the leftover.

### Dawnstar

| cell | grid | location | seen |
| --- | --- | --- | --- |
| `00008E76` | (8, 27) | Dawnstar | 2026-11-15 22:59: two cleared 3DNPC leftovers |
| `00008ED7` | (4, 24) | Wilderness, southwest of Dawnstar | 2026-11-15 22:59: cleared Female Dragonpriest leftover |
| `00008EB6` | (6, 25) | Dawnstar | 2026-11-13 crash: on the stack, next to a Dawnstar Guard |
| `00008E98` | (5, 26) | Wilderness, west of Dawnstar | 2026-11-15 22:01 and 22:38: destroyed on fast travel |

The leftovers here are runtime-created references, deleted but still in the mobile persistent
list (formIDs as logged; plugins from the load order of the 22:01 crash log):

| reference | base | base plugin | flags | cell |
| --- | --- | --- | --- | --- |
| `FF002527` | `FE64B800` | Female Dragonpriest.esp | `00010428` | `00008ED7` |
| `FF0025A6` | `2114033D` | 3DNPC.esp | `00000C28` | `00008E76` |
| `FF0025A7` | `2113FDAB` | 3DNPC.esp | `00000C28` | `00008E76` |

No Base Object Swapper is involved, so swaps are not the only source of leftovers.

`FF002527` is a dead, persistent, runtime-spawned actor. The deletion history (2026-11-15 23:26
and 23:28) shows the **engine** deleting it, on the main thread, every time cell `00008ED7`
loads: the cell's per-reference load step (19084 -> 19286) calls the actor's dead-body cleanup
(37293: `IsDead`, not initially disabled, and a few more checks), which disables and deletes it
(19806: `SetDelete(true)`, `SetAltered(true)`). Because the actor is persistent, the worldspace's
mobile list keeps it alive, so it is never freed. It is re-added to the cell and deleted again on
every visit, and left stale whenever the cell is destroyed. No script deletes it, and no quest
holds it: the holder log (2026-11-15 23:56) shows life state dead, not forced persistent, no quest
aliases. It carries the persistent flag itself, so it was created persistent, or a quest held it
earlier and released it.

`FF0025A6` and `FF0025A7` are books held by 3DNPC's running quest `21025759`
`DialogueJasparGaerston`, aliases "Book2" and "Book1". They are deleted and initially disabled,
but the filled aliases keep them in the persistent list.

So the sources differ (a base swap, a spawned corpse, quest-created items), but the engine does
the same thing with all of them: it keeps a deleted but persistent object in the worldspace's
lists, and leaves its `parentCell` dangling when the cell is destroyed. Every reference that
actually crashed or tripped a guard had formID 0; these `FF......` leftovers have not crashed yet.

1. Spend some time in Dawnstar.
2. Travel away (fast travel destroys the area's cells) or cross location boundaries with stress mode.
3. Come back, or keep crossing boundaries nearby.

## Steps

1. Use a save with plenty of quest progress, not a fresh character.
2. In `StaleParentCellFix.ini`:
   - `[Stress] bPurgeCellsOnLocationChange=1` purges the cell buffer after every Change Location
     event, so step 2 happens every time you cross a boundary. Test sessions only. Reinstalling the
     mod resets it to 0.
   - `[Guards] bEnabled=1` to catch it as a `PREVENTED` entry (with a HUD notification), or `0` to
     let it crash; the crash probe still logs the details first.
3. Visit a trigger area above, or move on a route that crosses many locations **and doubles back**:
   - Riverwood - Embershard - Alpha Wolf Den - Riverwood - Whiterun, then back again.
   - Whiterun - Battle-Born Farm - Loreius Farm - Whitewatch Tower - Dawnstar, and back.
   - Nightcaller Temple - Dawnstar.
4. Send back `StaleParentCellFix.log` and any new crash log from the SKSE log folder.

## Checking the fix

With `[Fix] bUnlinkPersistentReferences=1` (the default), the same steps should produce
`cell ... destroyed: cleared stale parentCell of persistent reference ...` lines instead of
`PREVENTED` entries or crashes. Each line gives the reference's formID, base, flags and list.
Healthy references are never cleared, so fast travel alone usually logs nothing. A `PREVENTED`
entry on a reference with a real formID and no deleted flag would mean the fix's scope is too
narrow. To see the crash again for comparison, set the fix to 0 (and `[Guards] bEnabled=0`).

## Sessions

All times are local. Logs are in
`Documents/My Games/Skyrim Special Edition/SKSE/` (`StaleParentCellFix.log`, `crash-*.log`).

- **2026-11-16 00:03, fix off, guards on, stress on.** One Dawnstar - Tundra Homestead cycle: no
  crash, no `PREVENTED`, no access violation. Expected for a use-after-free that only crashes when
  the freed memory has been reused by the time it is read; it does not show the leftovers are
  safe. Led to the stale-read reports, which log every such read regardless of luck.
- **2026-11-15 23:53, holder logging on, stress on.** Dawnstar - Tundra Homestead. 23:56:37: the
  three Dawnstar leftovers cleared again. The corpse `FF002527`: dead, not forced persistent, no
  quest aliases. The books `FF0025A6` / `FF0025A7`: quest `21025759` `DialogueJasparGaerston`
  (running), aliases "Book2" / "Book1". All three already deleted in the save.
- **2026-11-15 23:19, deletion history on, stress on.** Fast travel Dawnstar - Tundra Homestead
  and back, several times. 23:24:58: the same three Dawnstar leftovers cleared, all "no deletion
  recorded" (already deleted in the save). 23:26:35 and 23:28:14: `FF002527` cleared again, each
  time deleted about 21 s / 121 s earlier by the engine's dead-actor cleanup as `00008ED7` loaded
  (stack 36564 -> 40438 -> 40445 -> 40745 -> 40744 -> 13317 -> 13288 -> 13428 -> 13430 -> 13427 ->
  20460 -> 19084 -> 19286 -> 37293 -> 19806 -> SetDelete). One check took 9046 us because the
  stack was formatted while holding the lock; logging now happens after the lock is released.

- **2026-11-15 22:54, narrowed fix (leftovers only), stress on.** Left Dawnstar. At 22:59:03 the
  fix cleared the three leftovers in the Dawnstar table. No crash, no `PREVENTED`. Slowest check
  1108 us.
- **2026-11-15 22:33, fix clearing every reference, stress on.** One fast travel at 22:38:15
  destroyed cells `00008E98`, `00008ED7`, `00008E77` and `00008E76` and cleared 50 references:
  37 fixed, 13 mobile, nearly all live (XMarkers `00000034`, XMarkerHeadings `0000003B`, mod NPCs
  and objects). This showed the engine leaves every persistent reference stale, which led to
  narrowing the fix. Slowest check 1293 us.
- **2026-11-15 22:01, crash in the plugin (`crash-2026-11-15-22-01-52.log`).** Fast travel to
  Tundra Homestead destroyed cell `00008E98`, and the fix crashed in
  `StaleParentCellFix.dll+0028488` reading address `0x80` while walking `fixedPersistentRefMap`.
  CommonLib declares that map's values as inline arrays; the game stores pointers to arrays (see
  its clear, ID 20522, and array destructor, ID 19249). Fixed by walking the map with the game's
  layout.
- **2026-11-15 21:45, first fix, guards on, stress off.** About 15 minutes of running back and
  forth: the Tundra Homestead mist again, cell `000095F7`. The fix missed it because the purged
  cell's worldSpace pointer was already null when it was destroyed; fixed by checking all
  worldspaces in that case. `PREVENTED #1 at TESObjectCELL::AddReference`.
- **2026-11-15 15:05, guards on, stress on.** Riverwood - Alpha Wolf Den - Riverwood - Whiterun.
  Cell `000095F7` was purged at about 15:02:27 during normal movement; on the way back the loader
  re-added a deleted, formID-0 `AlphaTest` reference (base `00058E92`) that still pointed at the
  destroyed cell. `PREVENTED #1 at TESObjectCELL::AddReference`.

### Crashes before this plugin

The crash logs do not print the bad reference's base or position, only that it has formID 0.

| crash log | where | path | cells named |
| --- | --- | --- | --- |
| `crash-2026-11-07-21-21-55.log` | Embershard (location on the stack) | story manager | `000096A3` (23, -8), Lost Knife Hideout, on the stack |
| `crash-2026-11-07-22-09-19.log` | plains west of Whiterun | cell loader | `000095DF` (2, -1), the cell being loaded |
| `crash-2026-11-07-22-50-52.log` | Loreius Farm (location on the stack) | story manager | none |
| `crash-2026-11-13-00-50-13.log` | Dawnstar (`BGSLocation` in RDI) | story manager | `00008EB6` (6, 25) Dawnstar, `000038CD` (7, 38) sea, on the stack |

## Looking up a cell

Grid and location for a cell formID come from its `XCLC` and `XLCN` subrecords in the plugin that
defines it (Skyrim.esm for all the cells above).
