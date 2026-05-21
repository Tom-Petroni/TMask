# TMask

TMask est un node Nuke natif pour construire des masques proceduraux dans l'espace position, avec multi-pickers, bruit local et domain warp.

## Pourquoi TMask

- Jusqu'a 12 pickers avec gestion `+ / -`
- Controles par picker: center, size, falloff, noise
- Domain warp global (type, amount, scale, seed)
- Combine modes: `Max` ou `Add`
- Output masque monochrome sur RGBA

## Structure du repo

```text
TMask/
  publish/        # payload a copier dans .nuke
  work/           # source rust/c++ + tooling build
  node.json
  VERSION
  CHANGELOG.md
```

## Prerequis

- Nuke installe (version cible)
- Rust/Cargo
- Toolchain C++ compatible Nuke (MSVC sous Windows)

## Compiler

Depuis la racine du repo:

```powershell
cd work
cargo xtask --compile --nuke-versions 16.0 --target-platform windows --output-to-package --limit-threads
```

Exemples cibles:

- Linux: `--target-platform linux`
- macOS Intel: `--target-platform macos-x86-64`
- macOS Apple Silicon: `--target-platform macos-aarch64`

## Installer dans Nuke

1. Copier `publish/TMask` vers `C:/Users/<user>/.nuke/TMask`
2. Dans `C:/Users/<user>/.nuke/init.py`, ajouter:

```python
import nuke
nuke.pluginAddPath(r"C:/Users/<user>/.nuke/TMask")
```

3. Redemarrer Nuke

## Verification rapide

- Le menu `Nodes > TMask` apparait
- Le binaire est present dans:
  `TMask/bin/<nuke_version>/<os>/<arch>/`

## Docs techniques

- `work/docs/NODE_REFERENCE.md`

## Build CI GitHub

Le repo contient un workflow GitHub Actions (`.github/workflows/nuke-build.yml`) qui:

- build les versions Nuke 13.0 -> 17.0
- build Windows/Linux/macOS (x86_64 + arm64 quand disponible)
- genere un zip de release pret a copier dans `.nuke`

## Licence

Usage commercial soumis a la licence du repo (`LICENSE` + `EULA.md`).
