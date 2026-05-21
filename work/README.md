# TMask - Source

Code source du node Nuke `TMask` en version NDK/native (C++), avec packaging Python et pipeline build `xtask`, dans le meme esprit que `TNoise`.

## Structure

- `crates/tmask-nuke` : node natif Nuke (`tmask_base.cpp`) et bridge Rust.
- `xtask` : fetch SDK Nuke, compile et packaging multi-version.
- `TMask/` : package Python deployable dans `.nuke` (loader, menu, picker helpers).
- `docs/` : documentation node.

## Features TMask (NDK)

- Multi-pickers (jusqu'a 12) avec `+ / -`.
- Par picker: `center`, `size`, `falloff`, noise local.
- Types de noise: `Perlin`, `fBm`, `Turbulence`, `Ridged`, `Cell`.
- Domain warp global (enable/amount/type/scale/seed).
- Mirror rapide vers le picker suivant (`mirror pivot x`).
- `Pick` viewer pour sampler le center directement depuis l'AOV de position.
- `P channels` comme dans TNoise (Channel knob RGBA/XYZA + Keep Alpha).

## Build rapide

Windows / Nuke 16.0:

```powershell
cargo xtask --compile --nuke-versions 16.0 --target-platform windows --output-to-package --limit-threads
```

Sortie package:

`TMask/bin/16.0/windows/x86_64/TMask.dll`

## Installation dans `.nuke`

1. Copier `TMask/` et `init.py` dans `.nuke`.
2. Relancer Nuke.
3. Creer le node via `Nodes > TMask > TMask`.
