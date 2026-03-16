# The Gauntlet of Kal'manathas

This repo started from urob's config, but this README documents how this
specific repo works today.

## What This Repo Contains

- `config/`: keymap, behaviors, combos, and manifest (`west.yml`)
- `Justfile`: daily commands (`init`, `build`, `update`, `draw`, etc.)
- `build.yaml`: board targets (currently `glove80_lh` and `glove80_rh`)
- `firmware/`: generated output files (`.uf2`/`.bin`) after builds
- `draw/`: keymap-drawer config and generated diagrams
- `config/archive/`: commented archive of removed old snippets

Dependencies are pinned in `config/west.yml` (including a pinned `zmk` commit
and pinned module revisions).

## Recommended Setup (Nix + Just)

This repo is set up to work with `nix` and `direnv`. That is the easiest and
most reproducible path.

### 1. Clone

```bash
git clone https://github.com/kalmander/gauntlet-of-kalmanathas.git
cd gauntlet-of-kalmanathas
```

### 2. Enter the dev environment

Option A (`direnv`, recommended if you use it):

```bash
direnv allow
```

Option B (manual shell):

```bash
nix develop
```

### 3. Initialize workspace deps (first time only)

```bash
just init
```

This runs:

```bash
west init -l config
west update --fetch-opt=--filter=blob:none
west zephyr-export
```

## Daily Workflow

### Show available commands

```bash
just
```

### List build targets

```bash
just list
```

### Build firmware

Build left:

```bash
just build glove80_lh
```

Build right:

```bash
just build glove80_rh
```

Build both (all targets in `build.yaml`):

```bash
just build all
```

Pristine build:

```bash
just build glove80_lh -p
just build glove80_rh -p
```

Artifacts are written to:

- `firmware/glove80_lh.uf2`
- `firmware/glove80_rh.uf2`

## Flash Workflow

1. Put the half you want to flash into bootloader mode.
2. Build the matching artifact (`glove80_lh` or `glove80_rh`).
3. Copy the corresponding `.uf2` from `firmware/` to the mounted boot drive.
4. Repeat for the other half.

Example (replace mount path with your actual boot volume):

```bash
cp firmware/glove80_lh.uf2 /path/to/boot-drive/
```

## Updating Dependencies

### Pull updates for currently pinned manifest revisions

```bash
just update
```

### Change/pin dependency revisions

1. Edit `config/west.yml`.
2. Run `just update`.
3. Rebuild both halves.

Useful when pinning `zmk` to a commit:

```bash
git -C zmk rev-parse HEAD
```

### Update local Nix toolchain lockfile

```bash
just upgrade-sdk
```

Use this intentionally. It updates flake inputs and can change the local build
environment.

## Keymap Drawing

Generate a fresh keymap diagram:

```bash
just draw
```

Outputs:

- `draw/base.yaml`
- `draw/base.svg`

## Tests

The `test` recipe runs native ZMK module tests from a test folder that contains
`events.patterns` and `keycode_events.snapshot`.

Examples:

```bash
just test modules/zmk/adaptive-key/tests/basic
just test modules/zmk/tri-state/tests/swapper --verbose
```

## Cleanup / Recovery Commands

Clean build artifacts:

```bash
just clean
```

Hard reset workspace deps (forces re-init):

```bash
just clean-all
just init
```

## Notes

- This repo uses `west` for dependency management. Treat `config/west.yml` as
  the source of truth for dependency revisions.
- Archived removed snippets are in
  `config/archive/removed_nodes_2026-02-27.dtsi` (commented, not included in
  builds).
