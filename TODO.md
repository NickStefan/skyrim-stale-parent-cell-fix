# TODO

The root cause is known and fixed (FINDINGS.md). Open items:

- [x] One session with `_NoMoreMist_SWAP.ini` disabled: no formID-0 leftovers (12:37).
- [ ] Test the fix-only build with `_NoMoreMist_SWAP.ini` installed: expect "fixed released
      reference" lines and no crash.
- [ ] Write a small tool that rewrites BOS swap files to avoid the swaps that create leftovers
      (mist to `AlphaTest`), and test it with the DLL: expect an empty log.
- [ ] Report it to the Base Object Swapper / No More Mist authors.
- [ ] Check the hook's bytes on a second game build (see ROADMAP.md).
