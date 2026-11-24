# TODO

The root cause is known and fixed (FINDINGS.md). Open items:

- [x] One session with `_NoMoreMist_SWAP.ini` disabled: no formID-0 leftovers (12:37).
- [x] Fix-only build with the original `_NoMoreMist_SWAP.ini`: mist fixed, no crash (13:00).
- [x] `BaseObjectSwapperFixer/fix_alphatest.py`; with the rewritten files the swaps still hide
      their objects and the log stays empty (13:24).
- [x] Name what each fix cleared in the log (base type, name, plugin or runtime, leveled origin,
      cell and worldspace), without new hooks.
- [ ] Identify what spawned the runtime-created references at Northpoint (2026-11-22) from the
      next such log line.
- [ ] Play-test the rewritten Dark Dungeons for Draugr Crypts swap files in a Nordic ruin.
- [ ] Report the `AlphaTest` finding to the Base Object Swapper / No More Mist authors, and the
      engine bug (36459's early return) to people who know the engine (Engine Fixes, powerofthree).
- [ ] Check the hook's bytes on a second game build (see ROADMAP.md).
