# TODO

The root cause is known and fixed at the source (FINDINGS.md). Open items:

- [ ] Play a few more sessions with the defaults, around Tundra Homestead and elsewhere. Expect
      only "release of an already released reference" lines; a backstop clear, `PREVENTED` entry
      or access violation means another path exists.
- [x] One session with `_NoMoreMist_SWAP.ini` disabled: no formID-0 leftovers (12:37). Repeat once
      or twice to be sure.
- [ ] Write a small tool that rewrites BOS swap files to avoid the swaps that create leftovers
      (mist to `AlphaTest`), and test it with the DLL.
- [ ] Report it to the Base Object Swapper / No More Mist authors.
- [ ] Quieter diagnostics defaults for a release (see ROADMAP.md).
- [ ] Check the hooks' bytes on a second game build (see ROADMAP.md).
