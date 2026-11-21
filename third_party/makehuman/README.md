# MakeHuman assets (CC0 1.0)

Source assets for the VICTUS character pipeline (`tools/characters`, docs/CHARACTERS.md,
ADR-0179). Only MakeHuman **assets** are vendored; no MakeHuman program code (AGPL-3.0) is
included or used — the pipeline reads these files with its own Apache-2.0 loaders
(`tools/characters/mh.py`).

- Upstream: https://github.com/makehumancommunity/makehuman
- Commit: `a8bc2d54ff0ac92e78ff71431b1023eda42bf482` (2024-06-26)
- Licence: CC0 1.0 Universal for the assets (upstream `LICENSE.md` section C: "The base mesh and
  proxies, Targets and modifiers, Textures, Clothes, Poses and expressions ... have been released
  under CC0 1.0 Universal"); full text in `LICENSE.ASSETS.md`, copied unmodified.
- Modifications: none to the content. Text files are gzip-compressed (`.gz`) to keep the
  repository small; `gzip -d` restores the upstream bytes.

| File here | Upstream path | Use |
|---|---|---|
| `base.obj.gz` | `makehuman/data/3dobjs/base.obj` | hm08 base mesh (body, helper eyelash strips, teeth and tongue groups) |
| `default.mhskel.gz` | `makehuman/data/rigs/default.mhskel` | 163-bone reference skeleton (joint positions from vertex groups) |
| `default_weights.mhw.gz` | `makehuman/data/rigs/default_weights.mhw` | reference skin weights, collapsed onto the VICTUS 60-joint layout |
| `face-poseunits.bvh.gz`, `face-poseunits.json` | `makehuman/data/poseunits/` | facial pose units, baked into the 44 blend shapes |
| `high-poly.obj.gz`, `high-poly.mhclo.gz` | `makehuman/data/eyes/high-poly/` | eyeball proxy and its fitting data |
| `targets/*.target.gz` (24 files) | `makehuman/data/targets/macrodetails/` | macro targets: `universal-{male,female}-young-{min,average,max}muscle-{min,average,max}weight` and `{african,asian,caucasian}-{male,female}-young` |

To refresh: check out a newer upstream commit, copy the same paths, `gzip -9n` the text files,
update the commit above and in `docs/THIRD_PARTY_AND_LICENSES.md`, rebuild the characters and
review the renders.
