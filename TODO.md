# TODO

The root cause is known and fixed at the source (FINDINGS.md). Open items:

- [ ] Play a few more sessions with the defaults, around Tundra Homestead and elsewhere. Expect
      only "release of an already released reference" lines; a backstop clear, `PREVENTED` entry
      or access violation means another path exists.
- [ ] One session with `_NoMoreMist_SWAP.ini` disabled, to see whether the formID-0 leftovers come
      from the swap.
- [ ] If they do, find which part of the swap (MSTT to STAT) puts these references in the mobile
      persistent list, and report it to the Base Object Swapper / No More Mist authors.
- [ ] Quieter diagnostics defaults for a release (see ROADMAP.md).
- [ ] Check the hooks' bytes on a second game build (see ROADMAP.md).
