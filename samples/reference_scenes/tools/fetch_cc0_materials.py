#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Downloads the CC0 scanned materials of the photoreal reference scene (ADR-0140) from ambientCG
and writes them at 512 x 512 into samples/reference_scenes/assets/textures:

    python3 samples/reference_scenes/tools/fetch_cc0_materials.py samples/reference_scenes

Per material: <name>_color.jpg (sRGB), <name>_normal.jpg (OpenGL +Y convention) and <name>_orm.jpg
(linear; R = ambient occlusion, G = roughness, B = metalness, the glTF/Unreal ORM packing that
`.vmat` occlusion_strength reads, ADR-0139). JPEG at quality 92 with 4:4:4 chroma, so the packed
channels do not bleed into each other. Needs Pillow. The committed files are the output of this
script; regenerating needs network access and is only required when the set changes.
"""
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

from PIL import Image

# ambientCG asset id -> short name. All ambientCG assets are CC0 1.0 (https://docs.ambientcg.com/license/).
MATERIALS = {
    "PavingStones138": "paving",
    "Bricks090": "bricks",
    "WoodFloor051": "wood",
    "Metal049A": "metal",
    "Rock030": "rock",
}
SIZE = 512


def channel(files, suffix, default):
    for name in files:
        if name.endswith(f"_{suffix}.jpg"):
            return Image.open(io.BytesIO(files[name])).convert("L").resize((SIZE, SIZE), Image.LANCZOS)
    return Image.new("L", (SIZE, SIZE), default)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) / "assets/textures"
    out.mkdir(parents=True, exist_ok=True)
    for asset, name in MATERIALS.items():
        url = f"https://ambientcg.com/get?file={asset}_1K-JPG.zip"
        req = urllib.request.Request(url, headers={"User-Agent": "victus-reference-scenes/1"})  # the CDN rejects urllib's default
        with urllib.request.urlopen(req) as r:
            z = zipfile.ZipFile(io.BytesIO(r.read()))
        files = {n: z.read(n) for n in z.namelist()}
        color = next(v for n, v in files.items() if n.endswith("_Color.jpg"))
        normal = next(v for n, v in files.items() if n.endswith("_NormalGL.jpg"))
        save = dict(quality=92, subsampling=0)
        Image.open(io.BytesIO(color)).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS).save(out / f"{name}_color.jpg", **save)
        Image.open(io.BytesIO(normal)).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS).save(out / f"{name}_normal.jpg", **save)
        orm = Image.merge("RGB", (channel(files, "AmbientOcclusion", 255), channel(files, "Roughness", 255),
                                  channel(files, "Metalness", 0)))
        orm.save(out / f"{name}_orm.jpg", **save)
        print(f"{asset} -> {name}")


if __name__ == "__main__":
    main()
