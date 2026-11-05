# Reference-only rendering fragments

These files preserve historical Dawn of Dreams rules outside the shared renderer/transport:

- `DoDWidescreen.h`: reference framebuffer-bank classifier helper.
- `dod_cg_cadence.h`: reference packet/bank/cadence state machine.
- `field_offset_reference.inc`: old GIF XYOFFSET half-line rewrite.

They are not compiled or automatically installed. They are fragments, not a complete replacement integration.
The pre-change implementation remains in main commit `cc3b5e17f43254db8a06c829172b14b7014e9d52` for source comparison.
A port retaining any behavior must identify its own ELF/revision, supply a guarded adapter, reset all parser state
at session boundaries and validate native/upscaled, movie, menu, alpha/RT and both synchronous/asynchronous paths.
Other games must not inherit the bank, depth, atlas or cadence assumptions. The current shared `widescreen=1`
selects configured 16:9 output without guessing content type. See `docs/NEUTRAL_BASE_MIGRATION.md` at the repo root.
