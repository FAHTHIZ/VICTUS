# Area lights and light cookies: research for GAP-13

Accessed 2026-11-22. This is input for the next rendering task recorded in STATUS.md ("area lights and cookies, GAP-13"). It is research only: no VICTUS code is described as existing here unless stated.

## What the reference engines ship

| Engine | Area lights | Cookies / light functions | Source |
|---|---|---|---|
| Unity 6.x and 7.0, URP | Rectangle and Disc shapes. In URP, an Area light's Mode is forced to **Baked**: there is no real-time area light in the pipeline that Unity 7 keeps. | 2D cookie for Directional and Spot, cubemap for Point. Directional cookies have size and offset (animatable for scrolling). URP has no cookies on Area lights. | [URP Light component, 7000.0](https://docs.unity.com/en-us/engine/7000.0/manual/lighting-overview/lighting/light-component), [URP 15 Light reference](https://docs.unity3d.com/Packages/com.unity.render-pipelines.universal@15.0/manual/light-component.html) |
| Unreal Engine | Real-time Rect Light. It has Source Width and Height, a spherical attenuation radius, and emits only toward its local +X (like a 180° spot). Intensity is averaged over the area, so a larger rectangle needs more intensity for the same brightness. It also has a barn door angle and length, and a Source Texture on the emitter. | IES profiles on Point, Spot and Rect lights, as a 1D intensity curve swept around the axis rather than a projected texture. Not supported on Directional or Sky lights. | [Rect Lights (UE 4.27 docs, properties unchanged in UE5)](https://dev.epicgames.com/documentation/en-us/unreal-engine/rect-lights?application_version=4.27), [IES Light Profiles](https://dev.epicgames.com/documentation/unreal-engine/using-ies-light-profiles-in-unreal-engine) |
| Godot 4.7 | `AreaLight3D` (cited in the gap register). | Projector textures on Spot and Omni lights. | gap register GAP-13 |

Takeaway: the competitor that VICTUS's mobile-first tiering most resembles (URP) does **not** have real-time area lights. A baked-only area light that feeds the existing lightmapper (ADR-0122) already matches URP. Real-time rect lights on High and Ultra would go beyond URP and match Unreal.

## Technique: linearly transformed cosines (LTC)

- LTC is the standard real-time method. A 3×3 matrix per (view angle, roughness) maps a clamped cosine to an approximate GGX lobe. The polygon integral is then evaluated analytically in constant time per light ([Heitz et al. 2016, via the GPU Zen chapter on linear lights](https://hal.science/hal-02155101v1/file/LTC_linearlights_GPUZen.pdf)). The same table extends to line (tube) lights, which are cheaper than polygons.
- Data: two 64×64 float tables from `selfshadow/ltc_code` `fit/results/ltc.js`, used unchanged by other engines, for example [Ollin](https://github.com/eaviles/Ollin/blob/main/THIRD-PARTY-NOTICES.md) and [luma.gl](https://github.com/visgl/luma.gl/blob/master/modules/engine/THIRD-PARTY-NOTICES.md), which pins commit `31e5e96`.
- Licence: BSD-style. Redistributions in binary form must reproduce the copyright notice, and users of the code are asked to cite the SIGGRAPH 2016 paper ([LICENSE](https://raw.githubusercontent.com/selfshadow/ltc_code/master/LICENSE)). The table therefore needs an entry in THIRD_PARTY_AND_LICENSES.md and in the exported `THIRD_PARTY_NOTICES.txt`, and a dependency pin like the other vendored data.
- Anisotropic GGX needs a separate 8⁴ table ([KT et al., I3D 2022](https://arxiv.org/html/2203.11904v2), 0.7 ms at 1080p on an RTX 2080 in their engine). VICTUS has no anisotropic material, so this does not apply yet.
- Disks can use an inscribed polygon. luma.gl uses a 32-gon, with an area deficit under 0.65% ([luma.gl area-light docs](https://luma.gl/docs/api-reference/gltf/gltf-area-lights)).

## Fit with VICTUS today

- Clustered forward with CPU light assignment (ADR-0046) can carry a rect light as a local light with a sphere bound. That bound is the same as Unreal's attenuation radius.
- Shadows: the local-light atlas (ADR-0052) is perspective-only. A rect light could reuse a spot-style 180° frustum shadow, which gives hard shadows from the centre. Soft area shadows are a separate problem, and the sun contact shadows (ADR-0183) do not cover local lights.
- Cookies: a spot cookie is one more texture lookup in light space, using the same projection as the spot shadow map. A point cookie needs a cubemap. A directional cookie needs world-space tiling, and Unity's size and offset are a reasonable parameter set.
- Tiers: LTC costs one 2D lookup pair plus a polygon clip per light per pixel. Low and Medium could fall back to a representative-point approximation, or to a baked-only area light, which is URP's behaviour.
- Validation: there is no hardware reference. A path-traced reference image of one rect light over a rough plane, compared with `victus compare` (ADR-0102), is the honest way to check the integration.
