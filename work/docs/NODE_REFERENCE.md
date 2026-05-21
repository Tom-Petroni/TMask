# TMask Node Reference

## Input

- `Input`: stream contenant l'AOV de position.

## Top Controls

- `channels`: channels de sortie du mask.
- `P channels`: channels de position utilises pour les pickers (comme TNoise).
- `Keep Alpha`: applique l'alpha des `P channels` comme masque de validite.
- `Pickers`: nombre de pickers actifs.
- `+ / -`: ajoute ou retire des pickers.
- `combine`: union en `Max` ou accumulation en `Add`.
- `invert`: inverse le masque final.

## Domain Warp

- `warp`: active/desactive le domain warp.
- `warp amount`: intensite du warp dans l'espace position.
- `warp type`: `Perlin`, `fBm`, `Turbulence`, `Ridged`, `Cell`.
- `warp scale`: frequence du noise de warp.
- `warp seed`: seed global du warp.

## Mirror

- `mirror pivot x`: axe X utilise par le bouton `Mirror` de chaque picker.

## Picker (x12 max)

Chaque picker contient:
- `enable`
- `Pick`: sample du center depuis le Viewer (sample bbox center).
- `Mirror`: copie vers le picker suivant en miroir horizontal.
- `center` (`x,y,z`)
- `size`
- `falloff`
- `noise`
- `noise type`
- `noise scale`
- `noise amount`
- `noise seed`

## Output

- Masque monochrome ecrit sur `RGBA` (meme valeur dans `R`, `G`, `B`, `A`).
