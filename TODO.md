# TODO: telemetry to solve the formID-0 crash from one capture

The plugin catches the crash cleanly and records what the bad reference looks like afterwards.
It does not yet record the moment the reference went bad, so one more capture would probably
not be enough to solve the bug.

## What we'd learn from a capture today

- Which reference it is: base, flags, position, and which persistent list it is in (the
  "first seen" line).
- Where its `parentCell` points: null, a live cell, or a destroyed cell, plus when and by which
  call stack that cell was destroyed (free tracking).
- Who marked it deleted, if that happened this session (deletion history).
- Whether the fix cleared it, or a guard caught it. For a guard, also the full call stack at the
  crash site and the recent Change Location events.

## The two open questions

### 1. Who sets its formID to 0?

None of our hooks see this, and it is the bigger gap.

- The deletion hook records the deleted flag (`0x20`), which is a different thing. A deleted
  reference keeps its formID; the Dawnstar corpse kept `FF002527`.
- Something later zeroes the formID while the reference is still alive. The mist, vanilla
  `000DA6AD`, was seen as formID 0 with flags `0x2B`. It stays alive because the worldspace's
  persistent list holds a counted pointer to it.
- Guess: the game drops the form from its form map and clears the ID, without knowing the
  persistent list still holds it.
- "First seen" is coarse. It only fires when some cell is destroyed, so it says "already 0 by
  then", not when or how it got that way.

### 2. Why does the cell destructor miss it, when it nulls everything else?

The 2026-11-16 00:34 session showed the destructor nulls about 120 references, deleted `FF...`
leftovers included. Only formID-0 references have been proven to stay stale (the mist, 77 s and
163 s after its cell was destroyed).

- Guess: the destructor walks the cell's own reference list, not the worldspace's persistent
  lists. A reference already removed from the cell's list would then never be nulled.
- Not measured yet.

## What would make one capture enough

1. **A hook on the formID-zeroing path**, with call stacks in a small ring.
   - Find the game's form-ID setter and the remove-from-form-map function with
     `tools/addrlib.py`, hook both, and record any reference whose formID goes to 0.
   - This is a rare event, so it is cheap.
   - The "first seen" line would then include "formID zeroed N s ago by this stack". That stack
     is the root cause.
2. **An "is it still in the cell's own reference list?" check** when a formID-0 reference
   pointing at a cell about to be destroyed is seen. A single lookup, and it confirms or kills
   the guess about the destructor in one capture.
3. **A look at the destructor's code** (IDs 18877, 19410, 20508) to see which list it walks. That
   needs no play session. If it confirms the guess, item 2 just verifies it in play.

With all three, one capture should say who zeroed the formID, from what call path, and why the
destructor skipped it: enough to fix it at the source, or to state precisely what the real fix is.

Order: item 3 first (costs no play time), then items 1 and 2 before the next session.

## Leads from the web (2026-11-16)

- [xOBSE issue #252](https://github.com/llde/xOBSE/issues/252) (Oblivion, same engine lineage): a
  `PlaceAtMe` reference removed with `DeleteReference` is properly deleted, but a dangling pointer
  stays in the cell's object list; scanning the cell finds it with formID 0, and it passes the
  validity check. Open question there: why the reference destructor did not remove it from the
  list. Same signature as ours: formID 0 + deleted, still in a list the engine should have
  cleaned.
- Crash Logger logs from other users show the same object state, so it is not specific to this
  load order: formID 0 with `kDeleted`, for example flags `0x0002012A` on `ActorCharacter`
  references ([pastebin 16MCqbZy](https://pastebin.com/16MCqbZy),
  [pastebin S0Ktuh5i](https://pastebin.com/S0Ktuh5i),
  [reddit](https://www.reddit.com/r/skyrimmods/comments/10xdmq7/getting_weird_random_crashes_and_i_would_love/)).
  Ours: flags `0x2B` on a `TESObjectREFR`. Nobody there identified a cause.
- Working theory this supports: once the engine finishes deleting a reference, it releases the
  formID (sets it to 0) and drops it from the form map and the cell's list. A counted pointer in
  the worldspace's persistent list keeps it alive as a zombie, with a valid vtable, that no
  cleanup path knows about. That would answer both open questions above. Items 1 to 3 test it.
- Installed suspect: Disabled Reference Integrity Fix 1.5.0 is in every crash's module list. It
  hooks `InitItemImpl` and `Load3D` on references and moves references with base `00058E92`
  (`AlphaTest`, the mist's swapped base) to Z -30000. `include_deleted_refs` is off, and the mist
  itself is not in its log. A test: `early_fix_on_load3d = false` for a session.
- Similar but not this bug: Shadow Scene Node Crash Fix (deleted lights still in light lists after
  a cell change), the same "list outlives its entry" family.
