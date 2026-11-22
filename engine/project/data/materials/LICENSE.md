# Starter materials and sky

These files ship inside the engine so new projects start with real surfaces instead of flat
colours (ADR-0197). All of them are public domain (CC0 1.0); no attribution is required, but the
sources are recorded here for provenance.

| Folder / file | Source asset | Author | License |
|---|---|---|---|
| `grass/` | [Grass004](https://ambientcg.com/view?id=Grass004) | ambientCG (Lennart Demes) | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| `rock/` | [Rock030](https://ambientcg.com/view?id=Rock030) | ambientCG | CC0 1.0 |
| `planks/` | [Planks037B](https://ambientcg.com/view?id=Planks037B) | ambientCG | CC0 1.0 |
| `metal/` | [Metal032](https://ambientcg.com/view?id=Metal032) | ambientCG | CC0 1.0 |
| `paving/` | [PavingStones131](https://ambientcg.com/view?id=PavingStones131) | ambientCG | CC0 1.0 |
| `../sky/partly_cloudy.hdr` | [Kloofendal 48d Partly Cloudy (Pure Sky)](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky), 1K | Poly Haven (Greg Zaal, Jarod Guest) | [CC0 1.0](https://polyhaven.com/license) |

Each material folder holds the 1K JPG set repacked by hand: `color.jpg` (sRGB albedo),
`normal.jpg` (OpenGL convention, +Y up) and `orm.jpg` (R ambient occlusion, G roughness,
B metalness; the glTF ORM layout the PBR shader reads).

Modification: the grass set's roughness (ORM green) is remapped to 0.75 + 0.2 x source, because
the source map averaged 0.26 and rendered grass as wet plastic.

Modification: the metal set's colour is rebalanced to a neutral steel F0 (linear mean 0.56, 0.56, 0.57)
because the source averaged sRGB 0.49 / 0.54 / 0.58 and rendered navy blue under the sky.
