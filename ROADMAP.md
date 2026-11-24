# Roadmap

Rough order of work. Things move up or down as test reports show what matters.

## Done

- Crash family identified from four CrashLogger logs: a released reference (formID 0) still
  reachable from the story manager or the cell loader after its parent cell was freed.
- Root cause, traced in play and in the disassembly (2026-11-16, see FINDINGS.md): the release
  routine 36459 returns early on a reference whose formID is already 0, so a released reference
  that the loader put back into a cell keeps its `parentCell` when the cell unloads.
- The fix: hook 36459 and finish that release. Verified in the 12:24 session, when the
  destroy-time backstop and the crash guards of the investigation build had nothing left to do.
- No More Mist's swap of mist objects to `AlphaTest` identified as what creates the leftovers
  (12:37 session, one run).
- Reduced to the fix alone. The investigation build (crash guards, destroy-time backstop,
  lifetime and call-stack diagnostics, stress mode) is commit `a09749b`.

- Fix-only build play-tested with the original swap file (mist fixed, no crash) and with the swap
  files rewritten by `BaseObjectSwapperFixer/fix_alphatest.py` (nothing to fix).

- A second source of leftovers seen: objects created at runtime, deleted, in Northpoint
  (2026-11-22). The fix handled them; the log line now names the base, its plugin or runtime
  origin, the leveled actor it was made from and the cell, gathered only when the fix runs.

## Next

- Identify the Northpoint spawner from the next detailed log line.
- Report to the No More Mist / Base Object Swapper authors, and the engine bug to people who know
  the engine (Engine Fixes, powerofthree), for an independent check.

## Before release: game versions

Supported now: Skyrim SE 1.6.629 and later (AE), verified only on 1.6.640. The plugin refuses to
load on anything older. The hook checks its bytes before patching; a mismatch skips it and logs
it.

| ID (AE) | function | bytes checked |
| --- | --- | --- |
| 36459 | reference release | `4C 8B DC 55 56 57 41 54` |

The fix also calls `TESObjectCELL::RemoveReference` (19032) and the `SetParentCell` vfunc (0x98)
directly, reads `parentCell` at +0x60, and relies on the early return at the start of 36459
(`cmp dword ptr [rdx+14h], 0`); check all of that on any new exe.

- 1.6.629 to 1.6.1170: play-test at least one other build (e.g. 1.6.1170) to check the bytes still
  match. The Address Library IDs and layouts should be the same.
- 1.6.317 to 1.6.353 (early AE): same IDs; check the bytes and `parentCell` offset on one of those
  builds, and lower the version gate in `Plugin.cpp`.
- SE 1.5.97: find the SE Address Library IDs of 36459 and 19032 by matching them in a 1.5.97 exe
  with its database (`tools/steamstub.py <exe> <output>`, then `tools/addrlib.py` with `SPCF_EXE`
  and `SPCF_VERSIONLIB`; the SE `version-1-5-97-0.bin` is format 1, so addrlib.py needs that format
  added). Switch to `RELOCATION_ID(se, ae)` and check the bytes.
- VR: not planned. It has its own Address Library (CSV offsets) and layouts.
- For every version added: check the call chain of the root cause (13721 -> 19113 -> 19235 /
  36459) is the same.
