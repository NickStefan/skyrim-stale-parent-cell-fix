# Stale Parent Cell Fix

Fixes a use-after-free crash in Skyrim AE (1.6.629+, tested on 1.6.640): when an exterior cell is
purged from the cell buffer, the worldspace's persistent references that were in it keep a stale
`parentCell` pointer to the freed cell. Returning to that cell, or a radiant quest checking one of
those references, then crashes. Covering ground fast (skiing in Skiing in Skyrim, high speed)
makes it far more likely. Formerly FastLoadCrashFix. SKSE plugin built on CommonLibSSE-NG, with the
same toolchain and layout as SkiingInSkyrim.

### About the name

A more precise name would be "Stale Persistent Reference Cell Fix": the stale pointers that crash
have so far only been seen on persistent references, the ones a worldspace keeps in its
`mobilePersistentRefs` and `fixedPersistentRefMap` lists, and those lists are all the fix scans.
The shorter name was kept because it describes the defect, a `parentCell` left
pointing at a freed cell, and the fix doesn't depend on which object or mod produced the
reference. The crashes so far happened at Embershard, Loreius Farm, Dawnstar, near Whiterun and
at Tundra Homestead, so it isn't tied to one object.

## The crash

Four CrashLogger logs share one root cause:

| log | faults at | via |
| --- | --- | --- |
| crash-2026-11-07-21-21-55 | `20539+0x73` `mov rdx,[rbx+0x128]`, rbx `FFFFFFFF3F800000` | story manager alias fill, condition 21656 -> 18365 -> 19813 |
| crash-2026-11-07-22-50-52 | `20539+0x73`, rbx `3F8000000000040B` | same |
| crash-2026-11-13-00-50-13 | `20498+0x6` `mov rbx,[rcx+0x228]`, rcx `FFFFFFFF3F800000` | condition 21654 -> 18365 -> 19812 |
| crash-2026-11-07-22-09-19 | `18988+0x15` `mov rax,[rax+0x160]`, rax = UTF-16 text | CellLoaderTask: 19411 -> 19032 -> 20202 |

What the disassembly says (decrypted with `tools/steamstub.py`, IDs via `tools/addrlib.py`):

- 18365 is `BGSLocation::IsChild(ref, useCurrent)`: it gets the reference's current (vfunc, 19813)
  or editor (19812) location and walks up `parentLoc`. The condition functions 21656 / 21654
  (GetInCurrentLocAlias / GetIsEditorLocAlias style) call it on candidate references while the
  story manager fills quest aliases.
- 19812 / 19813 read the reference's `parentCell` (+0x60) and that cell's `worldSpace` (+0x128), and
  pass the worldspace to `TESWorldSpace::GetLocationFromCoordinates` (20539) or
  `TESWorldSpace::GetLocation` (20498), which fault on it.
- 19411 is `TESObjectCELL::AddReference`; it removes the reference from its old `parentCell`
  (19032), which asks `TESObjectCELL::GetEncounterZone` (18988) and faults.

In every log the reference has formID 0 and its `parentCell` is garbage (formID 0, type None,
nonsense editor ID; `0x3F800000` is `1.0f`, the UTF-16 is a string that took the memory over).
The reference is still reachable from another thread (story manager, cell loader) after the cell
it points to was freed. Fast movement means more cells attached/detached per second, hence the
crash being easy but not deterministic to hit.

Static findings on the mechanism:

- The cell destructor (18877) releases its references in 19410: it drops the cell's reference
  count on each one and frees the arrays, but never clears their `parentCell` (the per-entry call
  is an empty function). Any reference someone else still holds a count on outlives the cell with
  a dangling `parentCell`.
- The story manager node in all three alias-fill crashes is `0x0001320E`, the CLOC (Change
  Location) event. Every location boundary crossed starts radiant quest alias fills (MoreToSay's
  Embershard Bandits, Bounty: Bandit Boss, Rare Gifts in the logs) that test candidate references
  while cells behind the player are being torn down.
- Confirmed in play (2026-11-15): the worldspace's persistent reference lists hold the reference
  across the cell's destruction, and the cell loader (20532) re-adds it when the cell comes back.
  The formID was not cleared through `TESForm::SetFormID`; how it reaches 0 is still unknown.

## What the plugin does

The fix (`[Fix] bUnlinkPersistentReferences`): a worldspace holds counted pointers to its
persistent references (`fixedPersistentRefMap`, `mobilePersistentRefs`), but destroying an exterior
cell never clears their `parentCell`. Before a cell is destroyed, the plugin takes the lock the game
uses for those lists (ID 401119, bounded so it can never deadlock) and sets `parentCell` to null on
any of them still pointing at the cell that has formID 0 or the deleted flag. A purged exterior cell has usually lost its worldSpace pointer
by then, so in that case every worldspace's lists are checked; the slowest check is logged.

Null is a state the cell loader (ID 20532) already handles. For each reference in the loading
cell's fixed-map entry it calls `AddReference` when `parentCell` is null; when it isn't, it first
reads a flag from that cell, which with a stale pointer is a read of freed memory. The mobile list
is re-added the same way. Each cleared reference is logged as "cell X destroyed: cleared stale
parentCell of persistent reference ..." with its formID, base, flags and which list it was in.

Why only formID 0 or deleted ones: just before destruction many live persistent references still
point at the cell (a build that cleared them all cleared about 50 on one fast travel), but the
destructor itself re-points those: checked a millisecond later, at the next destruction in the
same burst (2026-11-16 00:27), every one of them pointed elsewhere, and checked right after it
returns (00:46), all were null, `FF......` deleted leftovers included. Only formID-0 references
have been proven to stay stale; the fix also clears deleted ones, redundantly but harmlessly. Clearing live ones early is
unnecessary, and what reads a live reference's cell in between (quest markers, Papyrus
`GetParentCell`, saving) is unknown. Every crash so far was on a leftover
with formID 0, the guarded ones also with the deleted flag (the `AlphaTest` swap of the Tundra
Homestead mist), so the fix is limited to those and live references keep the game's behavior. A
live reference hitting the same race would be caught by the guards and show up as `PREVENTED`.

Guards, the backstop if something is missed (entry detours, prologue bytes verified first; a mismatch skips that guard and logs it):

| ID | function | when the object is dead |
| --- | --- | --- |
| 18365 | `BGSLocation::IsChild` | reference / parentCell / worldSpace not live: return false |
| 20539 | `TESWorldSpace::GetLocationFromCoordinates` | worldspace not live: return null |
| 20498 | `TESWorldSpace::GetLocation` | worldspace not live: return null |
| 18905 | `TESObjectCELL::GetLocation` | cell not live: return null |
| 18988 | `TESObjectCELL::GetEncounterZone` | cell not live: return null |
| 19411 | `TESObjectCELL::AddReference` | old parentCell not live: cut the link, then add |

"Live" means readable (SEH-guarded, non-canonical pointers refused) with the right vtable and form
type. Every prevented crash is written to `StaleParentCellFix.log` with the thread, both objects
(header, RTTI class, base object, position, cell grid, raw bytes) and the call stack with Address
Library IDs.

Diagnostics:

- Lifetime history: the deleting destructors of `TESObjectCELL` (with call stacks) and
  `TESObjectREFR` are hooked. Reports say, for any pointer, "FREED: +0x128 into TESObjectCELL ...
  destroyed N ms ago on main thread; stack then: ...", i.e. what it was and who destroyed it.
- Deletion history (`[Diagnostics] bTrackDeletes`): `SetDelete` (vfunc 0x23) is hooked in the
  `TESObjectREFR`, `Actor` and `Character` vtables (IDs 19565 and 37564, both ending in
  `TESForm::SetDelete`, 14636), and the last 8192 deletions are kept with call stacks. Under each
  reference the fix clears, the log says who deleted it, when, and what cell it was in then; or
  that no deletion was recorded this session (for example, it was already deleted in the save).
- After each cell destructor returns, the references the fix left alone are checked. Any still
  pointing at the destroyed cell gets a "still points at it afterwards" warning and a HUD notice;
  that would mean the fix's scope is too narrow.
- formID-0 sightings: the first time the destroy hook sees each formID-0 persistent reference, it
  logs its base, flags, position, where its `parentCell` points (null, a live cell, or a destroyed
  one and who destroyed it) and its deletion history, with a HUD notice.
- Holders: under each cleared reference the fix also logs whether it is forced persistent (a quest
  alias or script holding it), an actor's life state, and the quest aliases filled with it (quest
  formID, editor ID, alias name, whether the quest is running).
- Change Location history: the queueing of the last story manager CLOC events is recorded with
  call stacks (32 frames, like the lifetime history) and added to every report. The quests these
  events start run later on a worker thread, so whatever caused the event (the engine, a mod, fast
  movement) is otherwise missing from the crash stack.
- A HUD notification the first time in a session that each guard fires (the session is then better
  reloaded than saved), a formID-0 persistent reference is seen, or the fix clears one, so you know
  the log has something worth sending.
- A vectored handler logs the first 8 access violations in SkyrimSE.exe code (never handles them):
  registers and stack slots that point into destroyed objects, and an unwound call stack. That
  covers the crash still happening somewhere unguarded.

Settings are in `StaleParentCellFix.ini` (guards, free tracking, probe, report limits).

## Build (Linux / SteamOS)

```sh
python3 tools/fetch_toolchain.py   # once; links ../SkiingInSkyrim's toolchain and CommonLib (read only)
python3 tools/build.py             # build/mod/SKSE/Plugins: DLL, PDB, INI
python3 tools/build.py test        # src/core tests (SEH guard, RTTI names) under Wine
```

Analysis helpers: `tools/steamstub.py` writes a decrypted `build/SkyrimSE.unpacked.exe`;
`tools/addrlib.py offset|id|dis` maps Address Library IDs and disassembles them with objdump.

## Testing

What has to happen for the crash, and routes and settings that trigger it reliably:
[REPRODUCING.md](REPRODUCING.md).

1. Copy `build/mod` into the game's Data folder (or a mod manager folder). 1.6.629+ only.
2. Check `StaleParentCellFix.log` in the SKSE log folder: the settings line, "unlinking persistent
   references ...", seven "guarded" lines (six guards plus the AddEvent history hook), the lifetime
   tracker and "tracking who queues Change Location story events".
3. Cross many location boundaries fast: Riverwood - Embershard - Helgen, Whiterun - Loreius Farm -
   Whitewatch Tower - Dawnstar, Nightcaller Temple - Dawnstar. Skiing or `player.setav speedmult 400`
   (then `player.modav carryweight 1` so it applies); trying without SkiingInSkyrim tells whether it
   is involved. To make the race far more likely, set `bPurgeCellsOnLocationChange=1` under
   `[Stress]` in the INI (a test session only; the log says "STRESS MODE" when it is on).
4. "cell ... destroyed: cleared stale parentCell of persistent reference" lines are the fix working. If the "a guard prevented a crash
   the fix missed" notification shows, send back `StaleParentCellFix.log` plus any new crash log, and
   don't overwrite a good save from that session. `PREVENTED` entries are a crash the fix did not
   stop; their `FREED:` lines and stacks show which cell was freed and who freed it.
