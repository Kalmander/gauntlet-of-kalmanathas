Local modules live here when this repo needs firmware code that should not be patched into vendored upstream modules.

Why this exists:
- We added a custom two-shot num layer flow that needed an auto-layer variant with a max keypress limit.
- We also needed a sticky-layer variant plus an explicit cancel helper, so repo-specific `NEXUS` one-shots can be consumed cleanly by `Enter` flows without flattening the whole layer stack.
- That same local sticky-layer variant now also treats chorded use as momentary: if another key is already held when the sticky-layer key is pressed, releasing it will not leave the layer latched on. This is specifically to make sloppy `NEXUS -> ENTER` rolls resolve cleanly back to `BASE`.
- We also needed a tri-state variant that can ignore release-only rollovers, because the upstream behavior interrupted on both presses and releases and that caused repo-specific `Enter -> Base` fallthrough in fast `Nexus` rolls.
- We also needed a stricter combo path for the num-layer home-row chords because stock combo timing was good in general but could still rare-misfire on fast same-hand rolls like `th`.
- The upstream `zmk-auto-layer` checkout is vendored from urob and should stay clean so pulling future upstream updates stays simple.
- The same rule applies to `zmk-tri-state`: if we need repo-specific behavior or experiments, they live here instead of turning upstream modules into patch piles.
- The same rule now applies to strict combos: the repo owns the special-case combo behavior here instead of carrying a patch in upstream `zmk/app/src/combo.c`.
- Keeping our custom behavior in a local module makes the ownership boundary explicit: upstream modules stay upstream, repo-specific firmware code stays here.

What changed relative to urob's build setup:
- Upstream modules are still brought in through `config/west.yml` as before.
- ZMK's app build expects local extra modules through `-DZMK_EXTRA_MODULES=...`, and then forwards that into Zephyr's module loader.
- This repo now injects `modules-local/gauntlet-behaviors` via `-DZMK_EXTRA_MODULES=...` in the `Justfile` build commands.
- `config/` is back to being config/keymap focused instead of pretending to be a Zephyr module.

If you build manually instead of using `just`, include:
- `-DZMK_EXTRA_MODULES=/absolute/path/to/modules-local/gauntlet-behaviors`

Current local module:
- `gauntlet-behaviors`: local copies/extensions for auto-layer, sticky-layer, tri-state, and strict combos.

## Sticky Layer Local

`gauntlet-behaviors` contains a local sticky-layer behavior used as `&sll`.

Why we made it:
- The original reason was explicit local cancel support, so repo-owned behaviors could consume one-shot `NEXUS` state directly.
- The current behavior also has one repo-specific semantic difference from upstream `&sl`: chorded presses are treated as momentary instead of sticky.

What it does beyond upstream `&sl`:
- Exposes a local cancel helper for repo-owned behavior code.
- Tracks whether another key position was already down when the sticky-layer key was pressed.
- If so, releasing that sticky-layer key releases the layer immediately instead of starting the sticky timeout.

Current use in this repo:
- `&sll NEXUS` is used on the NEXUS thumb paths and a few helper macros/adaptive keys.
- The overlap-aware release rule is there to keep fast or sloppy `LB3 -> LB2` Enter rolls from leaving `NEXUS` latched on.

## Strict Combos

`gauntlet-behaviors` now contains a local `gauntlet,strict-combos` listener.

Why we made it:
- The num-layer activation combo on `RM2 + RM3` was intentionally easy to hit, but that also meant a rare accidental trigger when typing fast `th`.
- A global `require-prior-idle-ms` was the wrong fix because it would make intentional mid-word num access worse.
- We wanted a repo-local solution that is easy to own and reason about, without patching vendored ZMK combo code.

What it does:
- It behaves like stock combos, but adds `require-overlap-ms`.
- A strict combo must still become a valid combo candidate within the normal `timeout-ms` window.
- After the combo is fully pressed, it only fires if the keys keep overlapping until `require-overlap-ms` has elapsed.
- If one key is released before that overlap window completes, the combo is cancelled and the original key events fall through.

Important difference from stock ZMK combos:
- Stock `zmk,combos` fires immediately when the combo becomes fully pressed and is the only remaining candidate.
- `gauntlet,strict-combos` waits until the overlap timer expires before firing.
- Because of that, `require-overlap-ms = 1` is not precisely identical to stock behavior. It is still slightly stricter because there is now a small extra window where an early release cancels the combo.
- The closest setting to stock behavior is `require-overlap-ms = 0`.

Current use in this repo:
- Both num-layer same-hand combos use the strict path now: `LM2 + LM3` and `RM2 + RM3`.
- The current overlap gate is `10 ms` for both the temporary and perma variants.

Important ownership rule:
- Do not define the same `key-positions` pair in both `zmk,combos` and `gauntlet,strict-combos`.
- The listeners are independent, so duplicated pairs across both containers are ambiguous and can misfire.
