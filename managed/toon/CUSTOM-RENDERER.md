# Per-model custom renderer

Models with a usable folder bundle automatically use their own **Custom renderer**, regardless of the shared rendering preference. Resolution is per instance, including Self/Profile/Rabbit. **Rendering (no custom)** applies only to models without a usable folder bundle; modes 0–3 remain available for them, and the legacy mode 4 preference resolves to Toon when no folder bundle exists. Existing host/profile ownership checks remain in place. A shader that fails to load still uses the diagnostic fallback described below; another model's bundle is never substituted.

```text
ftrez-models/
  Soyo - ChaoZiRan/
    model.json
    model.glb
    renderer.bundle
```

Dropping a `renderer.bundle` containing exactly one Shader asset into the folder requires no renderer metadata. For another filename or multiple Shader assets, use:

```json
{
  "id": "soyo_chaoziran",
  "name": "Soyo / 超自然骨骼",
  "model": "model.glb",
  "renderer": {
    "bundle": "renderer.bundle",
    "shader": "assets/ftreztoon/simpleurptoonlitoutlineexample.shader"
  }
}
```

`shader` is the exact AssetBundle asset path, not the Shader display name. The filename must stay directly inside the model folder. Copy resources first, then update `model.json` last when deploying an update. Folder scanning detects bundle addition/removal/size or timestamp changes and uses the existing model revision reload flow; changing renderer metadata also forces a revision.

Use a self-contained UnityFS Android Shader AssetBundle (maximum 16 MiB), built for Unity 2022.3.62f3, URP 14 and GLES3/Vulkan. The included example uses URP 14.0.12. Raw `.shader`, HLSL, Blender Eevee nodes, native libraries and URP RendererFeature scripts cannot be loaded this way. External bundle dependencies are not resolved.

The runtime creates a Material per GLB material using the chosen Shader or cloned bundle Material, binds `_BaseMap`/`_MainTex` and `_BaseColor`/`_Color`, respects double-sided flags via `_Cull`/`_CullMode`, and sets cutout properties for alpha parts. Custom shaders should implement these property conventions. All custom shaders, including `FTREZ/EeveeToon`, retain their other shader/material defaults, followed by the model's explicit scalar `shaderFloats` overrides. The generic face/band/lighting/emission preset applies only to built-in rendering styles. There is no per-material shader mapping. Custom mode does not add the game's separate outline material; include outlines in the shader if wanted.

Replacement renderers use normal Unity/URP scene lighting and default blended light/reflection probes. The runtime does not attach the host game's `CustomProbeAgent` or copy host MaterialPropertyBlocks. Each shader still decides how standard lights, SH, reflections, shadows and fog affect its output; camera post-processing remains active. This is not fixed illumination or complete isolation from scene appearance. A custom shader must implement the URP variants it consumes (including the Forward+ light loop and screen UV for clustered reflections when applicable). See [standard lighting correction](../../src/main/cpp/self_model/RENDER-LIGHTING.md).

Invalid/missing bundles, ambiguous shader selection and unsupported shaders report `Custom renderer fallback` in model status and fall back to the original Toon or an available game shader. A renderer failure does not hide the model. Retry after correcting/replacing the bundle or metadata.

Bytes are read on the model worker and counted in the bounded model cache. Unity loads the Shader on the existing game hook thread. After loading, `AssetBundle.Unload(false)` releases the archive while the instance owns the Shader; cleanup destroys instance materials and then its Shader. This allows Self/Profile/Rabbit to load the same archive without keeping duplicate archives open. The embedded Eevee Shader stays retained independently.

## Soyo comparison assets

- `build/model-resources/Soyo-Custom-Renderer.zip`: original pre-mouth-line geometry plus Eevee renderer.
- `build/model-resources/Soyo-Mouth-Line-Custom-Renderer.zip`: separate model ID and folder with the optional mouth contour plus the same renderer.
- `build/model-resources/Soyo-before-mouth-fix.zip`: original backup.

`adapt_soyo.py` defaults to no added mouth contour. Set `FTREZ_MOUTH_LINE=True` in its execution globals to export the separate Mouth Line model. Original Blender source is preserved.

Offline tests and APK compilation do not establish in-game appearance. Compare original Toon, Eevee Toon and Custom renderer on the original-geometry model first; compare the separate mouth-line model afterward. The cause of the missing mouth has not been confirmed on-device.
