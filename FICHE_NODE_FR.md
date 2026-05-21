# Fiche Node - TMask

## Resume

TMask est un node Nuke natif (Rust/C++) avec package Python pour loader/menu et outils de picker.

## Prerequis

- Nuke installe (version cible)
- Rust/Cargo
- Toolchain C++ compatible Nuke (MSVC sur Windows)

## Compiler

Depuis la racine du repo:

```powershell
cd work
cargo xtask --compile --nuke-versions 16.0 --target-platform windows --output-to-package --limit-threads
```

Exemples plateforme:

- Linux: `--target-platform linux`
- macOS Intel: `--target-platform macos-x86-64`
- macOS Apple Silicon: `--target-platform macos-aarch64`

## Installer dans Nuke

1. Copier `publish/TMask` dans `C:/Users/<user>/.nuke/TMask`
2. Ajouter dans `C:/Users/<user>/.nuke/init.py`:

```python
import nuke
nuke.pluginAddPath(r"C:/Users/<user>/.nuke/TMask")
```

3. Relancer Nuke

## Verification

- Verifier la presence de `Nodes > TMask`
- Verifier que le binaire existe dans `TMask/bin/<nuke_version>/<os>/<arch>/`
