Local modules live here when this repo needs firmware code that should not be patched into vendored upstream modules.

Why this exists:
- We added a custom two-shot num layer flow that needed an auto-layer variant with a max keypress limit.
- We also needed a tri-state variant that can ignore release-only rollovers, because the upstream behavior interrupted on both presses and releases and that caused repo-specific `Enter -> Base` fallthrough in fast `Nexus` rolls.
- The upstream `zmk-auto-layer` checkout is vendored from urob and should stay clean so pulling future upstream updates stays simple.
- The same rule applies to `zmk-tri-state`: if we need repo-specific behavior or experiments, they live here instead of turning upstream modules into patch piles.
- Keeping our custom behavior in a local module makes the ownership boundary explicit: upstream modules stay upstream, repo-specific firmware code stays here.

What changed relative to urob's build setup:
- Upstream modules are still brought in through `config/west.yml` as before.
- ZMK's app build expects local extra modules through `-DZMK_EXTRA_MODULES=...`, and then forwards that into Zephyr's module loader.
- This repo now injects `local-modules/gauntlet-behaviors` via `-DZMK_EXTRA_MODULES=...` in the `Justfile` build commands.
- `config/` is back to being config/keymap focused instead of pretending to be a Zephyr module.

If you build manually instead of using `just`, include:
- `-DZMK_EXTRA_MODULES=/absolute/path/to/local-modules/gauntlet-behaviors`

Current local module:
- `gauntlet-behaviors`: local copies of auto-layer and tri-state with repo-specific options.
