#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile VICTUS built-in GLSL shaders into an embedded C++ shader library.

For every *.vert / *.frag / *.comp in engine/render/shaders this produces:
  * SPIR-V (glslangValidator -V)            -> Vulkan
  * MSL 2.1 (spirv-cross --msl, compiled with -DVICTUS_MSL=1) -> Metal
  * resource counts (spirv-cross --reflect)  -> SDL_GPU shader create info
  * GLSL ES 3.00 (spirv-cross --es, graphics shaders only) -> OpenGL ES backend (ADR-0144), or
    an empty string when the shader needs more than ES 3.00 (validated with glslangValidator)

Generated files are committed so engine builds do not require shader tools.
`--check` verifies (without any shader tools) that the generated file was produced
from the current sources by comparing embedded SHA-256 hashes.

Shaders whose source hash is unchanged keep their previously generated SPIR-V/MSL, so a
different local glslang/SPIRV-Cross build only affects the shaders that were edited
(`--all` recompiles everything).
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "engine" / "render" / "shaders"
OUT_DIR = ROOT / "engine" / "render" / "generated"
OUT_CPP = OUT_DIR / "builtin_shaders.cpp"
STAGES = {".vert": "Vertex", ".frag": "Fragment", ".comp": "Compute"}


def sources():
    return sorted(p for p in SRC.iterdir() if p.suffix in STAGES)


# Shaders that contain this marker are also compiled with VICTUS_FP16 defined, as
# "<name>#fp16" (explicit half-precision variant for GPUs with shaderFloat16, ADR-0137).
FP16_MARKER = "VICTUS_FP16_VARIANT"
# Shaders that contain this marker are also compiled with VICTUS_REACTIVE defined, as
# "<name>#reactive": transparent passes that also write the TAA reactive mask as a second colour
# target (ADR-0143).
REACTIVE_MARKER = "VICTUS_REACTIVE_VARIANT"


class Unit:
    def __init__(self, path, name, defines):
        self.path, self.name, self.defines, self.suffix = path, name, defines, path.suffix


def units():
    out = []
    for p in sources():
        out.append(Unit(p, p.name, []))
        text = p.read_text()
        if FP16_MARKER in text:
            out.append(Unit(p, p.name + "#fp16", ["VICTUS_FP16=1"]))
        if REACTIVE_MARKER in text:
            out.append(Unit(p, p.name + "#reactive", ["VICTUS_REACTIVE=1"]))
    return out


def digest(unit):
    path = unit.path if isinstance(unit, Unit) else unit
    h = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n"))
    if isinstance(unit, Unit) and unit.defines:
        h.update(("\n" + " ".join(unit.defines)).encode())
    return h.hexdigest()


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"command failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
        sys.exit(1)
    return r.stdout


def compile_one(unit, tmp):
    path = unit.path
    defs = [f"-D{d}" for d in unit.defines]
    stem = unit.name.replace("#", "_")
    spv = tmp / (stem + ".spv")
    run(["glslangValidator", "-V", "--quiet", "--target-env", "vulkan1.0", *defs, str(path), "-o", str(spv)])
    # MSL is generated from a second compile with VICTUS_MSL defined so shaders can pick
    # Metal-specific binding numbers (SDL_GPU orders Metal buffers as uniforms, then storage).
    spv_msl = tmp / (stem + ".msl.spv")
    run(["glslangValidator", "-V", "--quiet", "--target-env", "vulkan1.0", "-DVICTUS_MSL=1", *defs, str(path), "-o",
         str(spv_msl)])
    msl = run(["spirv-cross", str(spv_msl), "--msl", "--msl-version", "20100", "--msl-decoration-binding"])
    refl = json.loads(run(["spirv-cross", str(spv), "--reflect"]))
    images = refl.get("images", [])
    ssbos = refl.get("ssbos", [])
    counts = {
        "samplers": len(refl.get("textures", [])) + len(refl.get("separate_images", [])),
        "storage_textures": len(images),
        "storage_buffers": len(ssbos),
        "uniform_buffers": len(refl.get("ubos", [])),
    }
    if path.suffix == ".comp":
        # Compute shaders split read-only and read-write storage (SDL_GPU binds them separately).
        counts["storage_textures"] = sum(1 for i in images if i.get("readonly"))
        counts["storage_buffers"] = sum(1 for b in ssbos if b.get("readonly"))
        counts["rw_storage_textures"] = len(images) - counts["storage_textures"]
        counts["rw_storage_buffers"] = len(ssbos) - counts["storage_buffers"]
        counts["threads"] = refl["entryPoints"][0]["workgroup_size"]
    return spv.read_bytes(), msl, counts


ESSL_PREFIX = {"Vertex": "vs_", "Fragment": "fs_"}


def essl_from_spirv(spv, stage, tmp, stem):
    """GLSL ES 3.00 with GL clip space and "//!" slot comments, or (None, reason).

    Conventions (engine/rhi/src/rhi_gles.hpp): vertex shaders get --fixup-clipspace and
    --flip-vert-y; vertex outputs and fragment inputs are renamed vx_io_<location> so the stages
    link by name; uniform blocks get a per-stage prefix so same-named blocks in the two stages do
    not have to match; SDL_GPU's sets (vertex samplers 0 / uniforms 1, fragment samplers 2 /
    uniforms 3) become "//! sampler <binding> <name>" and "//! ubo <binding> <block>" lines.
    """
    refl = json.loads(run(["spirv-cross", str(spv), "--reflect"]))
    if refl.get("ssbos") or refl.get("images") or refl.get("separate_images") or refl.get("separate_samplers"):
        return None, "storage buffers/images or separate samplers"
    cmd = ["spirv-cross", str(spv), "--es", "--version", "300"]
    if stage == "Vertex":
        cmd += ["--fixup-clipspace", "--flip-vert-y"]
        for o in refl.get("outputs", []):
            cmd += ["--rename-interface-variable", "out", str(o["location"]), f"vx_io_{o['location']}"]
    else:
        for i in refl.get("inputs", []):
            cmd += ["--rename-interface-variable", "in", str(i["location"]), f"vx_io_{i['location']}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, "spirv-cross: " + (r.stderr.strip().splitlines() or ["failed"])[0]
    text = r.stdout
    prefix = ESSL_PREFIX[stage]
    header = []
    for t in refl.get("textures", []):
        header.append(f"//! sampler {t['binding']} {t['name']}")
    types = refl.get("types", {})
    for u in refl.get("ubos", []):
        block = types.get(u["type"], {}).get("name", u["name"])
        new = prefix + block
        text, n = re.subn(rf"(\buniform\s+){re.escape(block)}(\s*\{{)", rf"\g<1>{new}\g<2>", text)
        if n != 1:
            return None, f"uniform block {block} not found in output"
        header.append(f"//! ubo {u['binding']} {new}")
    first, _, rest = text.partition("\n")
    text = "\n".join([first, *header, rest])
    out = tmp / (stem + (".vert" if stage == "Vertex" else ".frag"))
    out.write_text(text)
    v = subprocess.run(["glslangValidator", str(out)], capture_output=True, text=True)
    if v.returncode != 0:
        errs = [ln for ln in v.stdout.splitlines() if ln.startswith("ERROR")]
        return None, (errs[0] if errs else "invalid ESSL 3.00")
    return text, None


def spirv_from_block(block):
    m = re.search(r"k_spirv_\d+\[\] = \{\n(.*?)\n\};", block, re.S)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(1)))


def essl_decl(i, text):
    return f"const char k_essl_{i}[] =\n{c_string(text)};" if text else f'const char k_essl_{i}[] = "";'


def c_bytes(data):
    lines = []
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    return "\n".join(lines)


def c_string(text):
    # Raw string literal chunks keep MSVC under its string-literal length limit.
    chunks = [text[i:i + 12000] for i in range(0, len(text), 12000)] or [""]
    return "\n".join(f'R"VICTUSMSL({c})VICTUSMSL"' for c in chunks)


def previous_outputs():
    """name -> (sha, body text, entry line) from the existing generated file."""
    if not OUT_CPP.exists():
        return {}
    text = OUT_CPP.read_text()
    out = {}
    start = text.find("namespace {\n")
    table = text.find("const BuiltinShader k_shaders[] = {")
    if start < 0 or table < 0:
        return {}
    blocks = re.split(r"(?m)^(?=// \S+ sha256=)", text[start + len("namespace {\n"):table])
    entries = {m.group(1): m.group(0) for m in re.finditer(r'(?m)^    \{"([^"]+)",.*\},$', text[table:])}
    for b in blocks:
        m = re.match(r"// (\S+) sha256=([0-9a-f]+)", b)
        if m and m.group(1) in entries:
            out[m.group(1)] = (m.group(2), b.rstrip("\n"), entries[m.group(1)])
    return out


def renumber(text, old, new):
    return re.sub(rf"\bk_(spirv|msl|essl)_{old}\b", lambda m: f"k_{m.group(1)}_{new}", text)


def generate(recompile_all=False):
    previous = {} if recompile_all else previous_outputs()
    stale = [s.name for s in units() if s.name not in previous or previous[s.name][0] != digest(s)]
    if stale:
        for tool in ("glslangValidator", "spirv-cross"):
            if not shutil.which(tool):
                sys.exit(f"{tool} not found (install glslang-tools and spirv-cross)")
    missing_essl = [s.name for s in units() if s.name in previous and "k_essl_" not in previous[s.name][1]]
    if missing_essl and not shutil.which("spirv-cross"):
        sys.exit("spirv-cross not found (needed to add GLSL ES output)")
    entries = []
    body = []
    no_essl = []
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        for i, src in enumerate(units()):
            stage = STAGES[src.suffix]
            stem = src.name.replace("#", "_")
            if src.name not in stale:
                _, block, entry = previous[src.name]
                old = re.search(r"\bk_spirv_(\d+)\b", block).group(1)
                block, entry = renumber(block, old, i), renumber(entry, old, i)
                if "k_essl_" not in block:
                    # Older generated file: derive GLSL ES from the embedded SPIR-V, leaving the
                    # SPIR-V and MSL untouched.
                    text = None
                    if stage != "Compute":
                        spv = tmp / (stem + ".cached.spv")
                        spv.write_bytes(spirv_from_block(block))
                        text, why = essl_from_spirv(spv, stage, tmp, stem)
                        if why:
                            no_essl.append(f"{src.name}: {why}")
                    block += "\n" + essl_decl(i, text)
                    entry = entry.replace(f"sizeof(k_msl_{i}) - 1}}, ",
                                          f"sizeof(k_msl_{i}) - 1}}, {{k_essl_{i}, sizeof(k_essl_{i}) - 1}}, ", 1)
                body.append(block)
                entries.append(entry)
                continue
            print(f"compiling {src.name}")
            spv, msl, c = compile_one(src, tmp)
            text = None
            if stage != "Compute":
                spv_path = tmp / (stem + ".essl.spv")
                spv_path.write_bytes(spv)
                text, why = essl_from_spirv(spv_path, stage, tmp, stem)
                if why:
                    no_essl.append(f"{src.name}: {why}")
            body.append(f"// {src.name} sha256={digest(src)}")
            body.append(f"const std::uint8_t k_spirv_{i}[] = {{\n{c_bytes(spv)}\n}};")
            body.append(f"const char k_msl_{i}[] =\n{c_string(msl)};")
            body.append(essl_decl(i, text))
            extra = ""
            if "threads" in c:
                tx, ty, tz = c["threads"]
                extra = f', {c["rw_storage_textures"]}, {c["rw_storage_buffers"]}, {{{tx}, {ty}, {tz}}}'
            entries.append(
                f'    {{"{src.name}", rhi::ShaderStage::{STAGES[src.suffix]}, {{k_spirv_{i}, sizeof(k_spirv_{i})}}, '
                f'{{k_msl_{i}, sizeof(k_msl_{i}) - 1}}, {{k_essl_{i}, sizeof(k_essl_{i}) - 1}}, {c["samplers"]}, {c["storage_textures"]}, '
                f'{c["storage_buffers"]}, {c["uniform_buffers"]}{extra}}},')
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    text = "\n".join([
        "// SPDX-License-Identifier: Apache-2.0",
        "// GENERATED by tools/shaders/compile_shaders.py from engine/render/shaders. Do not edit.",
        '#include "victus/render/builtin_shaders.hpp"',
        "",
        "namespace victus::render {",
        "namespace {",
        *body,
        "const BuiltinShader k_shaders[] = {",
        *entries,
        "};",
        "} // namespace",
        "",
        "std::span<const BuiltinShader> builtin_shaders() noexcept { return k_shaders; }",
        "} // namespace victus::render",
        "",
    ])
    OUT_CPP.write_text(text, newline="\n")
    print(f"wrote {OUT_CPP.relative_to(ROOT)} ({len(entries)} shaders)")
    for line in no_essl:
        print(f"  no GLSL ES 3.00: {line}")


def check():
    if not OUT_CPP.exists():
        sys.exit("generated shader library missing; run tools/shaders/compile_shaders.py")
    text = OUT_CPP.read_text()
    stale = [s.name for s in units() if f"// {s.name} sha256={digest(s)}" not in text]
    count = text.count(" sha256=")
    if stale or count != len(units()):
        sys.exit(f"generated shaders are stale ({', '.join(stale) or 'shader set changed'}); "
                 "run tools/shaders/compile_shaders.py")
    print(f"builtin shaders up to date ({count})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--all", action="store_true", help="recompile every shader, not only changed ones")
    a = ap.parse_args()
    check() if a.check else generate(a.all)
