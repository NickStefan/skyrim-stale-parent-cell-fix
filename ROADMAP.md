# Roadmap

Rough order of work. Things move up or down as test reports show what matters.

## Done

- Root cause of the stale parentCell crash family pinned down from four CrashLogger logs: a reference (formID 0) still
  reachable from the story manager or cell loader thread after its parent cell was freed.
- Guards on IDs 18365, 20539, 20498, 18905, 18988 and 19411 (entry detours, prologue bytes verified first).
- Diagnostics: lifetime history (cell frees with stacks, reference frees), detailed incident reports, first-chance access violation probe, HUD notification per guard, call stacks
  of queued Change Location story events.
- Renamed from FastLoadCrashFix; removed the `SetFormID` history hook (never matched) and the per-event
  Change Location log lines.
- Fix: unlink persistent references from exterior cells before they are destroyed (worldspace lock
  401119, bounded).
- Static: cell teardown (18877 -> 19410) drops its references without clearing their parentCell; the story
  manager crashes all come from the CLOC (Change Location) event.

## Next

- Playtest on 1.6.640: confirm the guards install, see how often each one fires, and read the `FREED:` lines
  to learn which thread frees the cell, and how long before the stale reference is used.
- Confirm the fix in play: `cleared stale parentCell` lines, and no `PREVENTED` entries with stress
  mode on. The fix only clears formID 0 or deleted references; a `PREVENTED` entry on a live
  reference would mean the scope needs widening (clearing all of them was tried: about 50 per fast
  travel, reload-safe, effect on quest markers and saving unknown).
- Find what leaves deleted references in the persistent lists; not needed for the fix, useful for
  the mods' authors. Known so far (details in REPRODUCING.md): the Tundra Homestead mist `000DA6AD`,
  base swapped to `AlphaTest` by No More Mist, ends up formID 0; runtime-created references with
  3DNPC.esp and Female Dragonpriest.esp bases around Dawnstar end up deleted. The deletion history
  (`bTrackDeletes`) now prints who deleted each cleared leftover; if the stack shows a Papyrus
  native (ObjectReference.Delete and the like), the script itself is the next thing to identify.
- Add-reference guard (19411): skip references that look half-dead (formID 0) instead of cutting their link
  and adding them, so a zombie reference can't end up in a live cell and the save.
- Decide which guards to keep for a release, and make the diagnostics quieter by default (probe off, fewer
  detailed reports).

## Before release: game versions

Supported now: Skyrim SE 1.6.629 and later (AE), verified only on 1.6.640. The plugin refuses to load on
anything older. To support more:

- 1.6.629 to 1.6.1170: play-test at least one other build (e.g. 1.6.1170) to check the six guards' prologue bytes
  still match. The Address Library IDs and layouts should be the same; a mismatch only skips a guard and logs it.
- 1.6.317 to 1.6.353 (early AE): same IDs, but the cell still has the SE layout. Choose the cell offsets
  by runtime (`cellData` 0x60, `worldSpace` 0x120, `loadedData` 0x128, size 0x140 before 1.6.629), check the
  prologue bytes on one of those builds, and lower the version gate in `Plugin.cpp`.
- SE 1.5.97: find the SE Address Library IDs of the six guarded functions (none are named in CommonLibSSE-NG)
  by matching them in a 1.5.97 exe with its database (`tools/steamstub.py <exe> <output>`, then
  `tools/addrlib.py` with `SPCF_EXE` and `SPCF_VERSIONLIB`; the SE `version-1-5-97-0.bin` is format 1, so
  addrlib.py needs that format added). Switch to `RELOCATION_ID(se, ae)`, take the SE cell
  offsets above and SE vtable IDs (already in CommonLib), and check the prologue bytes.
- VR: not planned. It has its own Address Library (CSV offsets) and layouts; it would need the same work as
  SE on a VR exe.
- The fix (`Unlink`) on every version: the persistent-reference lock is AE ID 401119 (needs its SE ID,
  found the same way, through its users 20522/20532), the cell's worldspace offset follows the cell layout
  above, and `TESWorldSpace` member offsets (0xA8 fixed map, 0xD8 mobile list) should be checked against
  the loader's disassembly (ID 20532) on the target exe. The fixed map is walked by hand (capacity at
  0xB4, 0x18-byte entries at 0xD0, each pointing to an array of references) because CommonLib's
  declaration of its value type is wrong; check that against the clear (20522) too. The fix has no byte check of its own, so a wrong
  lock ID would not be caught at load time; verify it before lowering the gate.
- For every version added: check the call chains in the original crash logs still go through the guarded
  functions; the condition functions (21656/21654) and the cell-loader path (19411 -> 19032) may differ.
