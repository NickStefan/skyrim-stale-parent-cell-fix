# Stale Parent Cell Fix

Fixes a use-after-free crash in Skyrim AE (1.6.629+, tested on 1.6.640): a reference the game has
already released is left pointing at an exterior cell after that cell is destroyed. Returning to
the cell, or a radiant quest checking that reference when you cross a location boundary, then
reads freed memory and crashes. Covering ground fast (skiing, fast travel, high speed) makes it
more likely. SKSE plugin built on CommonLibSSE-NG, with the same toolchain and layout as
SkiingInSkyrim. Formerly FastLoadCrashFix.

## The bug

In short (details, IDs and evidence in [FINDINGS.md](FINDINGS.md)):

1. The game releases a reference (formID set to 0, marked deleted, unlinked from its cell) but
   leaves it in the worldspace's mobile persistent list, which keeps it alive.
2. When its cell loads again, the cell loader puts this dead reference back into the cell.
3. When that cell unloads, the game takes its temporary references out of the cell's reference
   list and asks its release routine to unlink them. The release routine skips any reference whose
   formID is already 0, so this one keeps pointing at the cell.
4. The cell's destructor only unlinks references still in its list, so the dead reference is left
   pointing at freed memory.

Every reference seen in step 1 so far is a mist object swapped to `AlphaTest` by No More Mist
(Base Object Swapper), and a run with `_NoMoreMist_SWAP.ini` removed produced none, so that swap
appears to create the conditions for the bug. How it gets them into the persistent list is still
open. The crash itself does not depend on it: any reference that goes through these steps is
affected.

## What the plugin does

**Source fix** (`[Fix] bCompleteRelease`, `Release.cpp`): hooks the release routine (ID 36459).
When it is given a reference whose formID is already 0 but that still points at a cell, the hook
first does what the game skips: removes it from the cell if the caller asked for that, and clears
its `parentCell`. Then the original runs as before. Logged as "release of an already released
reference ... cleared its parentCell, which the game skips".

**Destroy-time fix** (`[Fix] bUnlinkPersistentReferences`, `Unlink.cpp`), the backstop: before an
exterior cell is destroyed, it takes the worldspace's persistent-list lock (ID 401119, bounded so
it can never deadlock) and clears `parentCell` on every persistent reference that points at the
cell but is not in the cell's own reference list: exactly the ones the destructor would leave
dangling. With the source fix on it should find nothing. Logged as "cell X destroyed: cleared
parentCell of persistent reference ...". A null `parentCell` is a state the loader already handles.

**Guards** (`[Guards] bEnabled`), in case something is still missed: entry detours (prologue bytes
verified first; a mismatch skips that guard and logs it) on the functions the crashes fault in.

| ID | function | when the object is dead |
| --- | --- | --- |
| 18365 | `BGSLocation::IsChild` | reference / parentCell / worldSpace not live: return false |
| 20539 | `TESWorldSpace::GetLocationFromCoordinates` | worldspace not live: return null |
| 20498 | `TESWorldSpace::GetLocation` | worldspace not live: return null |
| 18905 | `TESObjectCELL::GetLocation` | cell not live: return null |
| 18988 | `TESObjectCELL::GetEncounterZone` | cell not live: return null |
| 19411 | `TESObjectCELL::AddReference` | old parentCell not live: cut the link, then add |

"Live" means readable (SEH-guarded, non-canonical pointers refused) with the right vtable and form
type. Each prevented crash is logged as `PREVENTED` with the thread, both objects and the call
stack.

**Diagnostics** (`[Diagnostics]`), used to find the cause and kept for reports:

- Destroyed cells (with call stacks) and references, so a dangling pointer can be described as
  "FREED: ... into TESObjectCELL ..., destroyed N ms ago on ...; stack then: ...".
- History of references being deleted (`SetDelete`), set to formID 0 (`SetFormID`), given a cell
  while formID 0 (`SetParentCell`) and removed from a cell (`RemoveReference`), with call stacks,
  printed under every reference a fix touches.
- The first sighting of each formID-0 reference in the persistent lists, and a watch on each one
  put back into a cell that logs if it drops out of the cell's list.
- Call stacks of recent Change Location story events, added to reports.
- The first few access violations in SkyrimSE.exe code (logged, never handled).
- A HUD notification the first time in a session that anything worth sending happens.

Settings and their descriptions are in `StaleParentCellFix.ini`.

## Build (Linux / SteamOS)

```sh
python3 tools/fetch_toolchain.py   # once; links ../SkiingInSkyrim's toolchain and CommonLib (read only)
python3 tools/build.py             # build/mod/SKSE/Plugins: DLL, PDB, INI
python3 tools/build.py test        # src/core tests (SEH guard, RTTI names) under Wine
```

Analysis helpers: `tools/steamstub.py` writes a decrypted `build/SkyrimSE.unpacked.exe`;
`tools/addrlib.py offset|id|dis` maps Address Library IDs and disassembles them with objdump.

## Testing

1. Copy `build/mod` into the game's Data folder (or a mod manager folder). 1.6.629+ only.
2. Check `StaleParentCellFix.log` in the SKSE log folder: the settings line, "completing the release
   of already released references ...", "unlinking persistent references ...", and a "guarded" line
   per hook. A hook whose bytes do not match logs an error instead and is skipped.
3. Play, or follow a route from [REPRODUCING.md](REPRODUCING.md) (the Tundra Homestead loop
   triggers it within minutes on the affected load order).
4. The HUD says when the log has something worth sending. "Caught a formID 0 reference" and
   "fixed a reference at the source" are expected: a leftover was created, and the source fix
   handled it. "Prevented a stale cell pointer" (the backstop), "a reference was left pointing at
   a destroyed cell" or "a guard prevented a crash" mean something got past the source fix: send
   `StaleParentCellFix.log` and any new crash log.

## About the name

The name describes the defect, a `parentCell` left pointing at a freed cell, which is also what a
crash log shows. The root cause is more specific (the release routine skipping already released
references), but both fixes still clear that stale pointer, and the name matches what users search
for.
