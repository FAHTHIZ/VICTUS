#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile VICTUS built-in GLSL shaders into an embedded C++ shader library.

For every *.vert / *.frag / *.comp in engine/render/shaders this produces:
  * SPIR-V (glslangValidator -V)            -> Vulkan
  * MSL 2.1 (spirv-cross --msl, compiled with -DVICTUS_MSL=1) -> Metal
  * resource counts (spirv-cross --reflect)  -> SDL_GPU shader create info
  * GLSL ES 3.00 (spirv-cross --es from a compile with -DVICTUS_ESSL=1, graphics shaders only)
    -> OpenGL ES backend (ADR-0144), or an empty string when the shader needs more than ES 3.00
    (validated with glslangValidator)

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
# Bump when the GLSL ES conversion changes so cached shaders regenerate their ESSL only.
ESSL_REV = 4


def essl_for(unit, tmp):
    """(text, reason): GLSL ES for a graphics unit, compiled from source with VICTUS_ESSL=1."""
    stage = STAGES[unit.suffix]
    if stage == "Compute":
        return None, None
    stem = unit.name.replace("#", "_")
    spv = tmp / (stem + ".essl.spv")
    defs = [f"-D{d}" for d in unit.defines]
    run(["glslangValidator", "-V", "--quiet", "--target-env", "vulkan1.0", "-DVICTUS_ESSL=1", *defs, str(unit.path),
         "-o", str(spv)])
    return essl_from_spirv(spv, stage, tmp, stem)


def essl_from_spirv(spv, stage, tmp, stem):
    """GLSL ES 3.00 with GL clip space and "//!" slot comments, or (None, reason).

    Conventions (engine/rhi/src/rhi_gles.hpp): vertex shaders get --fixup-clipspace and
    --flip-vert-y; vertex outputs and fragment inputs are renamed vx_io_<location> so the stages
    link by name; uniform blocks and samplers get a per-stage prefix so same-named ones in the two
    stages do not have to match or share a unit; SDL_GPU's sets (vertex samplers 0 / uniforms 1, fragment samplers 2 /
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
        # ES 3.00 has no storage buffers: under VICTUS_ESSL shaders read storage slot N through a
        # "highp usampler2D vx_storage<N>" that the RHI fills with the buffer's bytes.
        m = re.fullmatch(r"vx_storage(\d+)(_\w*)?", t["name"])
        # GLSL ES uniforms with one name are one uniform across the program, so a vertex and a
        # fragment sampler sharing a name would share a texture unit: prefix them per stage too.
        name = prefix + t["name"]
        text, n = re.subn(rf"\b{re.escape(t['name'])}\b", name, text)
        if n == 0:
            return None, f"sampler {t['name']} not found in output"
        header.append(f"//! storage {m.group(1)} {name}" if m else f"//! sampler {t['binding']} {name}")
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


# A generated GLSL ES declaration: the optional revision line, then "" or raw-string chunks.
ESSL_DECL = re.compile(r'\n(?:// essl rev=\d+\n)?const char k_essl_\d+\[\] =(?: "";|\n(?:R"VICTUSMSL\(.*?\)VICTUSMSL"\n?)+;)',
                       re.S)


def essl_decl(i, text):
    body = f"const char k_essl_{i}[] =\n{c_string(text)};" if text else f'const char k_essl_{i}[] = "";'
    return f"// essl rev={ESSL_REV}\n{body}"


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
    essl_stale = [s.name for s in units() if s.name in previous and f"// essl rev={ESSL_REV}\n" not in previous[s.name][1]]
    if essl_stale:
        for tool in ("glslangValidator", "spirv-cross"):
            if not shutil.which(tool):
                sys.exit(f"{tool} not found (needed to regenerate GLSL ES output)")
    entries = []
    body = []
    no_essl = []
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        for i, src in enumerate(units()):
            stage = STAGES[src.suffix]
            if src.name not in stale:
                _, block, entry = previous[src.name]
                old = re.search(r"\bk_spirv_(\d+)\b", block).group(1)
                block, entry = renumber(block, old, i), renumber(entry, old, i)
                if f"// essl rev={ESSL_REV}\n" not in block:
                    # GLSL ES from an older generator: regenerate only it, leaving the SPIR-V and
                    # MSL untouched.
                    had_essl = "k_essl_" in block
                    block = ESSL_DECL.sub("", block)
                    text, why = essl_for(src, tmp)
                    if why:
                        no_essl.append(f"{src.name}: {why}")
                    block += "\n" + essl_decl(i, text)
                    if not had_essl:
                        entry = entry.replace(f"sizeof(k_msl_{i}) - 1}}, ",
                                              f"sizeof(k_msl_{i}) - 1}}, {{k_essl_{i}, sizeof(k_essl_{i}) - 1}}, ", 1)
                elif stage != "Compute" and re.search(r'const char k_essl_\d+\[\] = "";', block):
                    no_essl.append(f"{src.name}: (cached)")
                body.append(block)
                entries.append(entry)
                continue
            print(f"compiling {src.name}")
            spv, msl, c = compile_one(src, tmp)
            text, why = essl_for(src, tmp)
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
