# Findings

How the stale `parentCell` crash happens, from crash logs, disassembly of Skyrim 1.6.640 and
in-game captures. Function numbers are Address Library IDs (AE); `tools/addrlib.py dis <id>`
disassembles them from the decrypted exe (`tools/steamstub.py`). The hooks and diagnostics
mentioned below are in the investigation build, commit `a09749b`; the current plugin is only the
fix.

## The crashes

Four CrashLogger logs from before the plugin share one cause:

| log | faults at | via |
| --- | --- | --- |
| crash-2026-11-07-21-21-55 | `20539+0x73` `mov rdx,[rbx+0x128]`, rbx `FFFFFFFF3F800000` | story manager alias fill, condition 21656 -> 18365 -> 19813 |
| crash-2026-11-07-22-50-52 | `20539+0x73`, rbx `3F8000000000040B` | same |
| crash-2026-11-13-00-50-13 | `20498+0x6` `mov rbx,[rcx+0x228]`, rcx `FFFFFFFF3F800000` | condition 21654 -> 18365 -> 19812 |
| crash-2026-11-07-22-09-19 | `18988+0x15` `mov rax,[rax+0x160]`, rax = UTF-16 text | CellLoaderTask: 19411 -> 19032 -> 20202 |

In every one the reference has formID 0 and its `parentCell` (+0x60) points at freed memory
(`0x3F800000` is `1.0f`; the UTF-16 is a string that took the memory over).

- 18365 is `BGSLocation::IsChild`. The condition functions 21656 / 21654 call it on candidate
  references while the story manager fills quest aliases. It reads the reference's current (19813)
  or editor (19812) location, which follows `parentCell` to the cell's `worldSpace` (+0x128) and
  into `TESWorldSpace::GetLocationFromCoordinates` (20539) or `GetLocation` (20498).
- The story manager node in all three alias-fill crashes is `0x0001320E`, the Change Location
  (CLOC) event: crossing a location boundary starts radiant quests whose alias conditions test
  nearby references.
- 19411 is `TESObjectCELL::AddReference`. Before adding a reference to a cell, it removes it from
  its old `parentCell` (19032), which reads that cell's encounter zone (18988).

## Root cause

A released reference that the game revives, then releases again, ends up pointing at a destroyed
cell. Four steps, each captured in play with call stacks (2026-11-16, Tundra Homestead mist
`000DA6AD`):

1. **Released, but left in the mobile persistent list.** A main-thread cleanup (18235 or 18233 ->
   18237) releases references listed for a grid cell. For each it calls
   `TESObjectCELL::RemoveReference` (19032), then the game's reference release (36459), which marks
   the reference deleted (`SetDelete`), nulls `parentCell`, drops it from the form map (14627),
   releases its handle and sets its formID to 0 (`TESForm::SetFormID`, 14666). The worldspace's
   `mobilePersistentRefs` still holds a counted pointer to it, so the object stays alive with a
   valid vtable, formID 0 and the deleted flag (flags `0x2B`). Every reference seen in this state
   had base `AlphaTest` (`00058E92`): the mist objects No More Mist swaps through Base Object
   Swapper. The same mist formID leaves one more dead copy each time its cell reloads.
2. **Revived by the cell loader.** When the cell loads again, the loader (20532, on a cell-loading
   worker thread) adds the worldspace's persistent references in that cell back into it with
   `AddReference` (19411), dead ones included. The leftover gets `parentCell` = the cell and is in
   the cell's reference set, consistently.
3. **Erased from the cell without being unlinked.** When the cell is unloaded and destroyed
   (13721), 19113 first collects the cell's non-persistent references and erases them from its
   reference set inline (19235, filter in 19230; no `RemoveReference` call), then passes each to
   36459 with `removeFromCell` false, relying on it to null `parentCell`. 36459 opens with
   `if (reference->formID == 0) return;`, taking such a reference for already released. The
   revived leftover has formID 0, so it leaves the set but keeps `parentCell`.
4. **Skipped by the destructor.** The cell destructor (18877 -> 19410 -> 19458 -> 19468) nulls
   `parentCell` only on references still in the cell's reference set. The leftover is not, so it
   keeps pointing at the freed cell, and the persistent list keeps it reachable. The next time the
   loader re-adds it (step 2, now reading the freed cell: the `AddReference` crash) or a Change
   Location quest tests it (the `IsChild` crashes), the game reads freed memory.

Why only formID 0: step 3 is the only path that drops a reference from a cell without nulling
`parentCell`, and it only skips references whose formID is already 0. Deleted references that
keep their formID (the `FF......` leftovers at Dawnstar) are persistent, so 19235 leaves them in
the set, and the destructor nulls them (2026-11-16 00:34 session).

What is still unknown: why released `AlphaTest` mist references are in the mobile persistent list
at all (ordinary mist objects are not persistent; their flags lack `0x400`). Base Object Swapper
swaps the base of these references from a movable static (MSTT) to a static (STAT) when they
initialize. Base Object Swapper's own code never deletes references, changes formIDs or touches
cells or persistent lists (source read 2026-11-16; its 3.4.1 "engine bug with Skyrim.esm refs" fix
only changes the seed of random-chance swaps), so the suspect is the swap's result, not the code.

Strong lead (2026-11-16 12:37 session): with `_NoMoreMist_SWAP.ini` removed and nothing else
changed, a run around Tundra Homestead produced no formID-0 leftovers at all: no sightings, nothing
for either fix to do. With the file, every session today had its first leftover within minutes. So
the swap of these mist objects to `AlphaTest` appears to be what creates the conditions for the bug
(step 1). One session so far. The engine bug in steps 3 and 4 is independent of it: any released
reference left in the persistent list would go stale the same way.

## Sources of the leftovers

What the fix acts on: the release routine (36459) is handed a reference whose formID is already 0
and that still points at a cell. A formID of 0 means the reference was released before; the first
release clears `parentCell`, so something linked it to a cell again afterwards (for the mist: the
loader, re-adding it from the worldspace's persistent list). A log line without "removed it from
the cell" means the caller passed `removeFromCell` false: the cell-unload path (19113), right after
19235 took the reference out of the cell's list. Each fix is therefore a reference the destructor
would have left pointing at the freed cell. The Dawnstar `FF......` leftovers of 2026-11-15/16
(false alarms of the early, broader fix) never meet this condition: they kept their formIDs and
stayed in the cell's list.

A stale link only crashes if something follows it after the cell's memory is reused (a return to
the cell, or a Change Location quest testing the reference). The mist did, four times; that
cannot be shown for each fix.

### Mist swapped to `AlphaTest` (Base Object Swapper)

See "What is still unknown" above. The No More Mist swaps rewritten as disables by
`BaseObjectSwapperFixer/fix_alphatest.py` (`[Transforms]` with the same conditions,
`posR(0.0,0.0,-30000.0),flags(0x00000800)`) still hide the objects and produce no leftovers
(2026-11-16 13:24). The same tool converted the other installed swap files that use `AlphaTest`,
including Dark Dungeons for Draugr Crypts (420 light and torch swaps, light to static, like the
mist's movable static to static); those are not play-tested for leftovers yet.

Seasons of Skyrim swap files (`ZZZ_Unfrozen_Northpoint_SUM.ini` and others) also swap to
`AlphaTest` (by formID, `0x58E92~Skyrim.esm`), but cannot be rewritten: Seasons' format is only
`base|swap` per record type (no properties, flags or conditions), applied when a reference's model
is queued (12910) and undone when the season changes, and it skips disabled references. BOS
conditions cover location, region, keyword, cell and worldspace, not season, and BOS applies once
at initialization, so it cannot hide by season either. Those Northpoint swaps are static to static
and have not been seen to leave leftovers.

### Objects created at runtime (Northpoint, 2026-11-22)

Three references fixed together at 00:02:31, in cell `2DC27287`, which is `Northpoint.esp`'s
exterior cell `04C27287` of its own worldspace `NorthpointWorld` at grid (-45, -26), where all
three positions fall (Z about -16100 to -16300 there is street level):

| base | flags | position |
| --- | --- | --- |
| `FF003D27` | `0x29` | (-182908, -105948, -16110) |
| `FF003D2F` | `0x29` | (-183574, -105337, -16333) |
| `FF003D44` | `0x29` | (-183379, -105881, -16187) |

- The bases are forms created at runtime (`FF......`). The usual way a base is created at runtime
  is placing a leveled actor, which makes a temporary actor base. Three such references within
  about 700 units of each other, deleted (`0x20`) and released together, fit a small group of
  spawned actors cleaned up later.
- Not the Seasons swap: it was winter and the Northpoint swap files cover summer, autumn and spring
  only; the bases are not `AlphaTest`.
- Not Northpoint itself: `Northpoint.esp` ships only two quest fragment scripts.
- Not Dynamic Underwater Fish (spawns vanilla critter spawners and CC fish around a player whose
  head is under water): there was no swimming; the player was walking in the city.
- No SKSE log of the session names the three formIDs, so the spawning mod is unknown. The log line
  now names the base, its type and the leveled actor it was made from, to identify it next time.

## Engine details

| ID | what | notes |
| --- | --- | --- |
| 36459 | reference release (manager, reference, removeFromCell) | returns at once if formID is 0; else `SetDelete(true)`, optional 19032, `SetParentCell(nullptr)` (vfunc 0x98), 14627, handle release, `SetFormID(0)` |
| 18237 | per-grid-cell release | called from 18235 (main loop, 13288) and 18233 (fast travel path) |
| 20532 | cell loader: re-adds persistent references | via 19411; worker thread |
| 13721 | cell unload and destroy | 19113, then 20461 (removes it from the grid, 20508, and deletes it) |
| 19113 | releases a cell's temporary references | erases them with 19235, then 36459 with `removeFromCell` false |
| 19235 / 19230 | inline erase from the cell's set / its filter | skips persistent references (19593, flag `0x400`) |
| 19032 | `TESObjectCELL::RemoveReference` | the only caller of the set's erase (19373) |
| 19818 | `TESObjectREFR::SetParentCell` | vfunc 0x98; actors use 37304 |
| 18877 / 19410 | cell destructor / its reference release | the only code that releases a whole set |

Cell layout (1.6.629+): reference set (`BSTScatterTable`) at +0x88: capacity +0x94, free count
+0x98, sentinel +0xA0, slots +0xB0 (0x10 bytes each: reference, next; a slot is used when next is
non-null). Lock at +0x120, `worldSpace` at +0x128. A reference's `parentCell` is at +0x60.

`TESWorldSpace::fixedPersistentRefMap` (+0xA8) stores a pointer to an array per entry, not the
inline array CommonLibSSE-NG declares (see its clear, 20522, and the array destructor, 19249). The
lists are guarded by the lock with ID 401119.

## How each step was established

- **Step 4** by disassembly of the destructor (19410 walks only the cell's set), then in play: right
  after each destructor returns, every reference that was in the set is null, about 120 per burst
  including the Dawnstar leftovers (2026-11-16 00:34); the mist, not in the set, was still stale
  77 s and 163 s after its cell was destroyed (2026-11-15).
- **Step 1** by a `SetFormID` entry hook (14666): every formID-0 reference in the persistent lists
  was set to 0 by 36459 from 18237, on the main thread (2026-11-16 11:21-11:28, about 30
  references). An earlier hook, before the plugin was in git, never saw it.
- **Step 2** by a `SetParentCell` hook: the leftover was given the cell by the loader through
  `AddReference`, and was in the cell's set right after (11:42 and 12:00 captures).
- **Step 3** by elimination, a watch, then disassembly. A `RemoveReference` hook saw no call on the
  leftover (12:00). A watch that checked the set every ~20 ms caught 51 of the cell's 58 entries
  leaving within 3 ms of its destruction, with the same slot array, the same cell object and no
  `RemoveReference` (12:18). The destroy stack led to 13721 -> 19113 -> 19235 and the early return
  in 36459. Every capture showed about 18 s between the loader re-adding the leftover and the
  cell's destruction.
- **The fix** (completing 36459 for formID-0 references): in the next session the mist was released,
  revived, then caught at step 3 and unlinked; the destroy-time backstop found nothing to clear,
  and no guard fired (12:32).
- **The swap as the source of step 1**: the same route with `_NoMoreMist_SWAP.ini` removed gave no
  formID-0 leftovers (12:37-12:44). With the swaps rewritten as disables (`[Transforms]`
  `posR(0.0,0.0,-30000.0),flags(0x00000800)`, by `BaseObjectSwapperFixer/fix_alphatest.py`), the
  objects stay hidden and the fix had nothing to do either (13:24).

## Related reports

- [xOBSE issue #252](https://github.com/llde/xOBSE/issues/252) (Oblivion, same engine lineage): a
  deleted `PlaceAtMe` reference left in a cell's object list with formID 0.
- Crash Logger logs from other users show references with formID 0 and the deleted flag in crashes
  ([pastebin 16MCqbZy](https://pastebin.com/16MCqbZy), [pastebin S0Ktuh5i](https://pastebin.com/S0Ktuh5i),
  [reddit](https://www.reddit.com/r/skyrimmods/comments/10xdmq7/getting_weird_random_crashes_and_i_would_love/)),
  so the state is not specific to this load order.
- Ruled out: Disabled Reference Integrity Fix 1.5.0 (source read): it never deletes references,
  changes formIDs, moves them between cells or touches persistent lists, and only acts on
  initially disabled or very low references. It reads `parentCell` in its hooks, so it can be a
  victim of a stale pointer, not the cause.
