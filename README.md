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

One hook, on the release routine (ID 36459, `Release.cpp`). When the game asks it to release a
reference whose formID is already 0 but that still points at a cell, the hook first does what the
game skips: removes the reference from the cell if the caller asked for that, and clears its
`parentCell`. Then the original runs as before. A null `parentCell` is a state the game already
handles. Nothing else is patched.

Each fix is logged as "fixed released reference 0x... (base ..., flags ..., position (...)) still
pointing at cell ...: cleared its parentCell", and the first one in a session shows a HUD message
(`[General] bNotifications` in `StaleParentCellFix.ini`). The hook checks the function's first
bytes before patching; on a mismatch (another mod or game version) it logs an error and does
nothing.

The investigation build (diagnostics, crash guards and a destroy-time backstop) is in git history
up to commit `a09749b`; see [FINDINGS.md](FINDINGS.md).

## Avoiding the leftovers: `BaseObjectSwapperFixer`

`BaseObjectSwapperFixer/fix_alphatest.py` rewrites Base Object Swapper `<form>|AlphaTest` swaps
as BOS's usual disable: a `[Transforms]` entry (with the same conditions) that keeps the base,
sets the initially-disabled flag and moves the reference 30000 units down. Put the `_SWAP.ini`
files in `BaseObjectSwapperFixer/SWAPS_IN/`, run `python3 BaseObjectSwapperFixer/fix_alphatest.py`,
and install the files from `SWAPS_OUT/` over the originals. With the rewritten No More Mist file
the leftovers no longer appear; the DLL still covers any other source.

## Build (Linux / SteamOS)

```sh
python3 tools/fetch_toolchain.py   # once; links ../SkiingInSkyrim's toolchain and CommonLib (read only)
python3 tools/build.py             # build/mod/SKSE/Plugins: DLL, PDB, INI
```

Analysis helpers: `tools/steamstub.py` writes a decrypted `build/SkyrimSE.unpacked.exe`;
`tools/addrlib.py offset|id|dis` maps Address Library IDs and disassembles them with objdump.

## Testing

1. Copy `build/mod` into the game's Data folder (or a mod manager folder). 1.6.629+ only.
2. Check `StaleParentCellFix.log` in the SKSE log folder: the settings line, "ReleaseReference ...
   hooked" and "completing the release of already released references ...".
3. Play, or follow the Tundra Homestead loop in [REPRODUCING.md](REPRODUCING.md), which triggers it
   within minutes on the affected load order.
4. "StaleParentCellFix: fixed a stale cell reference" on the HUD means the fix just prevented the
   crash's setup; the log says which reference it was.

## About the name

The name describes the defect, a `parentCell` left pointing at a freed cell, which is also what a
crash log shows. The root cause is more specific (the release routine skipping already released
references), but the fix still works by clearing that stale pointer, and the name matches what users search
for.
