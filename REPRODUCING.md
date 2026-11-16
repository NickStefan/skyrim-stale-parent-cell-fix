# Reproducing the crash

## What has to happen

The full mechanism is in [FINDINGS.md](FINDINGS.md). In play terms:

1. **Visit an area with a leftover.** The game releases some references (formID 0, deleted) but
   leaves them in the worldspace's mobile persistent list. On the affected load order these are
   the mist objects No More Mist swaps to `AlphaTest`; they are released as you move away from
   them.
2. **Come back, so the cell loads again.** The loader puts the leftover back into the cell.
3. **Leave until the cell is destroyed, not just unloaded.** Unloaded exterior cells stay in the
   cell buffer (`uExterior Cell Buffer`, 36 by default) until enough other cells push them out, or
   until fast travel unloads everything. When the cell is unloaded and destroyed, the leftover is
   taken out of it but keeps pointing at it.
4. **Something touches the leftover.** Either:
   - **Come back to the same cell.** The loader re-adds the leftover and first removes it from the
     freed cell: crash in `TESObjectCELL::AddReference` / `GetEncounterZone` on a cell-loading
     worker thread.
   - **Cross a location boundary.** A Change Location story event starts radiant quests whose alias
     conditions test nearby references; reading the freed cell's worldspace or location crashes in
     `BGSLocation::IsChild` / `TESWorldSpace::GetLocationFromCoordinates` on a job thread.

Step 4 only crashes if the freed memory has been reused by then, so most stale pointers go
unnoticed; the plugin's log shows them regardless.

## Why it is hard to hit

Steps 2 and 3 need a return trip and real distance (or a fast travel) in between. Moving fast
covers distance quickly, but speed itself is not needed. A character with many radiant quests
ready to start (bounties picked up, Missives, quest mods like MoreToSay) makes the quest variant of
step 4 much more likely than a fresh character does.

## Known trigger areas

### The mist near Tundra Homestead (cell `000095F7`)

- Record: vanilla reference `000DA6AD` (Skyrim.esm), base `00077773` `FXMistLow01HalfVis` (MSTT),
  at (39160, -6468, -4960) in Wilderness cell `000095F7`, grid (9, -2), next to Tundra Homestead
  east of Whiterun. No plugin overrides the reference.
- `_NoMoreMist_SWAP.ini` (No More Mist, Base Object Swapper) swaps its base to `AlphaTest`
  (`00058E92`, STAT). In game the stale object is a formID-0, deleted-flagged (flags `0x2B`)
  reference at that spot, base `00058E92`, in the mobile persistent list.
- It is released by the game a few seconds after you leave, revived when the cell loads again,
  and goes stale when the cell is destroyed, about 18 s after the revival in every capture.

1. Go to Tundra Homestead so cell (9, -2) loads, then leave.
2. Come back, then leave far enough (or fast travel) for the cell to be destroyed.
3. Repeat. On the affected load order this triggers within about five minutes.

Other mist references swapped the same way go through the same steps elsewhere (formIDs
`000E21F0`, `000FCA50`, `000C9C4D`-`4F`, `000FC8A4`-`A6`, `0004C1E6`-`E7`, `000EA462`, `000E99xx`,
`02014E7C`-`80` in the 2026-11-16 logs).

### Dawnstar (harmless leftovers)

These turned out not to be dangerous: they keep their formIDs and are persistent, so they stay in
the cell's reference list and the destructor unlinks them (2026-11-16 00:34 session). Kept here
because they were investigated at length and show other ways deleted references stay in the
persistent lists.

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

Every reference that actually crashed or tripped a guard had formID 0; these `FF......`
leftovers never did.

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

## Checking the fixes

With the defaults, the steps above should produce, for each leftover that would have gone stale:

- `release of an already released reference 0x... (formID 0, base 00058E92, flags 0000002B) still
  pointing at cell 000095F7 (...): cleared its parentCell, which the game skips`: the source fix
  (`[Fix] bCompleteRelease`).
- No `cell ... destroyed: cleared parentCell of persistent reference` lines: those are the
  destroy-time backstop (`bUnlinkPersistentReferences`) catching what the source fix missed.
- No `PREVENTED` entries (guards) and no access violations.

"formID 0 persistent reference ... first seen" lines are normal: they record leftovers being
created (step 1), which the plugin does not prevent. A `left the reference list of cell ...
without RemoveReference` warning (the watch) is expected too if a check lands between the game
taking the reference out of the list and the source fix running; it describes step 3, not a
failure.

To compare: with `bCompleteRelease=0` the backstop clears the same references when their cell is
destroyed; with both fixes and `[Guards] bEnabled=0` the crash can happen again.

## Sessions

All times are local. Logs are in
`Documents/My Games/Skyrim Special Edition/SKSE/` (`StaleParentCellFix.log`, `crash-*.log`).
Every session from 2026-11-16 11:18 on ran with stress mode off, around Tundra Homestead.

- **2026-11-16 12:24, source fix.** The mist was released at 12:31:06; at 12:32:10, as cell
  `000095F7` unloaded, the source fix cleared its `parentCell`. The backstop found nothing to clear;
  no watch warning, no `PREVENTED`, no access violation.
- **2026-11-16 12:12, set watch.** The mist was re-added at 12:18:00; at 12:18:18 the watch found it
  gone from the cell's list: in the 3 ms before the cell's destruction 51 of its 58 entries left
  (free count 454 to 505 of 512, same slot array, same cell object), with no `RemoveReference`. The
  destroy stack (13721 -> 20461) led to 19113 -> 19235 and the early return in 36459: the root
  cause. The backstop cleared the mist.
- **2026-11-16 11:55, `RemoveReference` hook.** Released 11:58:58, re-added 12:00:34, cleared by the
  backstop at 12:00:52. No `RemoveReference` on it in between.
- **2026-11-16 11:37, `SetParentCell` hook.** Released 11:42:21; at 11:42:49 the loader (20532 ->
  19411, worker thread) re-added it to cell `000095F7`, in the cell's list; cleared by the backstop
  at 11:43:07, no longer in the list.
- **2026-11-16 11:18, `SetFormID` hook.** About 30 `AlphaTest` references set to formID 0 by the
  release routine (36459) from the per-grid cleanup (18237), all left in the mobile list with a
  null `parentCell`; the same formIDs left a new dead copy on each cell reload. The mist was
  released at 11:24:47 and cleared by the backstop at 11:28:06, pointing at its cell again. Its
  release record had already been overwritten by then (the history rings turn over in minutes), so
  records are now kept for each leftover when it is first seen.

- **2026-11-16 00:34, fix off, checked right after each destructor, stress on.** Three bursts,
  about 120 persistent references pointing at destroyed cells: the destructor set every one to
  null, including the three Dawnstar leftovers (the corpse twice). None stayed stale, so the
  `FF......` deleted leftovers were never dangerous and clearing them was redundant. Cell
  `000095F7` (the mist's) was destroyed with 6 references and no formID-0 one among them. Only
  formID-0 references have been proven stale (the mist, 77 s and 163 s after destruction). The
  next build logs the first sighting of every formID-0 reference in the persistent lists. The
  stale-read bookkeeping was then removed; only a warning for references still stale after the
  destructor remains.
- **2026-11-16 00:24, fix off, stale-read bookkeeping, stress on.** Three unload bursts (00:27:03,
  00:28:55, 00:29:43). Every persistent reference recorded as pointing at a cell just before its
  destruction had been re-pointed by the time the next cell in the same burst was checked, a
  millisecond later, so the destructor itself updates live references. No stale reads. The next
  build checks right after each destructor returns, separately counting pointers left stale, set
  to null, or set to another cell.
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
  and objects). That was a snapshot taken before the destructor ran; the 00:24 session showed the
  destructor re-points live ones itself. It still led to narrowing the fix. Slowest check 1293 us.
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
