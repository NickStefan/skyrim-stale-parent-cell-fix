# Roadmap

Rough order of work. Things move up or down as test reports show what matters.

## Done

- Crash family identified from four CrashLogger logs: a released reference (formID 0) still
  reachable from the story manager or the cell loader after its parent cell was freed.
- Guards on IDs 18365, 20539, 20498, 18905, 18988 and 19411 (entry detours, prologue bytes
  verified first).
- Destroy-time fix (now the backstop): unlink persistent references the cell's destructor would
  leave pointing at it (worldspace lock 401119, bounded).
- Root cause, traced in play and in the disassembly (2026-11-16, see FINDINGS.md): the release
  routine 36459 returns early on a reference whose formID is already 0, so a released reference
  that the loader put back into a cell keeps its `parentCell` when the cell unloads.
- Source fix: hook 36459 and finish that release (`bCompleteRelease`). Verified in the 12:24
  session: the backstop and the guards had nothing left to do.
- Diagnostics: cell and reference lifetime history, `SetDelete` / `SetFormID` / `SetParentCell` /
  `RemoveReference` history with stacks, a watch on revived references, the first-chance access
  violation probe, Change Location event stacks and HUD notifications.
- Renamed from FastLoadCrashFix.

## Next

- Confirm the source fix over more sessions and other areas: only "already released reference"
  lines, no backstop clears, no `PREVENTED`.
- Make the diagnostics quieter by default for a release: probe off, fewer detailed reports, the
  `SetParentCell` / `RemoveReference` history and the watch off unless asked for.
- Decide which guards to keep for a release now that the cause is fixed.
- Find out why the swapped mist references are in the mobile persistent list at all (they are not
  persistent-flagged). Test: a session with `_NoMoreMist_SWAP.ini` disabled; if no formID-0
  leftovers appear, report it to the Base Object Swapper and No More Mist authors.
- Optional, stricter: have the loader (20532) skip formID-0 references instead of reviving them,
  so a released reference never gets back into a cell. Riskier than the current fix, which only
  completes what the game already intended.

## Before release: game versions

Supported now: Skyrim SE 1.6.629 and later (AE), verified only on 1.6.640. The plugin refuses to
load on anything older. Every hook checks its bytes before patching; a mismatch skips that hook
and logs it. The hooks, by Address Library ID (AE):

| ID | function | bytes checked |
| --- | --- | --- |
| 36459 | release reference (source fix) | `4C 8B DC 55 56 57 41 54` |
| 14666 | `TESForm::SetFormID` (diagnostics) | `40 53 55 56 57 41 54` |
| 19818 | `TESObjectREFR::SetParentCell` (diagnostics) | `48 89 5C 24 10 57 48 83 EC 20` |
| 19032 | `TESObjectCELL::RemoveReference` (diagnostics; patched past its null check) | `48 85 D2 0F 84 9D 03 00 00` |
| 32359 | `BGSStoryEventManager::AddEvent` (diagnostics) | see `Events.cpp` |
| 18365, 20539, 20498, 18905, 18988, 19411 | guards | see `Guards.cpp` |

The source fix also calls 19032 and the `SetParentCell` vfunc (0x98) directly, and relies on the
early return at the start of 36459 (`cmp dword ptr [rdx+14h], 0`); check that on any new exe.

To support more:

- 1.6.629 to 1.6.1170: play-test at least one other build (e.g. 1.6.1170) to check the bytes above
  still match. The Address Library IDs and layouts should be the same.
- 1.6.317 to 1.6.353 (early AE): same IDs, but the cell still has the SE layout. Choose the cell
  offsets by runtime (`cellData` 0x60, `worldSpace` 0x120, `loadedData` 0x128, size 0x140 before
  1.6.629), check the bytes on one of those builds, and lower the version gate in `Plugin.cpp`.
- SE 1.5.97: find the SE Address Library IDs of the hooked functions (most are not named in
  CommonLibSSE-NG) by matching them in a 1.5.97 exe with its database (`tools/steamstub.py <exe>
  <output>`, then `tools/addrlib.py` with `SPCF_EXE` and `SPCF_VERSIONLIB`; the SE
  `version-1-5-97-0.bin` is format 1, so addrlib.py needs that format added). Switch to
  `RELOCATION_ID(se, ae)`, take the SE cell offsets above and SE vtable IDs (already in
  CommonLib), and check the bytes.
- VR: not planned. It has its own Address Library (CSV offsets) and layouts; it would need the same
  work as SE on a VR exe.
- The backstop (`Unlink`) on every version: the persistent-reference lock is AE ID 401119 (needs
  its SE ID, found the same way, through its users 20522/20532), the cell's worldspace offset
  follows the cell layout above, and the cell's reference set (`+0x88`, slots at `+0xB0`) and
  `TESWorldSpace` member offsets (0xA8 fixed map, 0xD8 mobile list) should be checked against the
  loader (20532) and the destructor (19458) on the target exe. The fixed map is walked by hand
  (capacity at 0xB4, 0x18-byte entries at 0xD0, each pointing to an array of references) because
  CommonLib's declaration of its value type is wrong; check that against the clear (20522) too.
  The backstop has no byte check of its own, so a wrong lock ID would not be caught at load time;
  verify it before lowering the gate.
- For every version added: check the call chains in the original crash logs still go through the
  guarded functions; the condition functions (21656/21654) and the cell-loader path
  (19411 -> 19032) may differ.
