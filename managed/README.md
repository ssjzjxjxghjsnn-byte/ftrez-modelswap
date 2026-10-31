# Managed rendering resources

`lobby_model/` preserves all 32 source, shader, editor, inspector, and Python
files named in the original build manifest, byte for byte. The original
manifest is archived as `lobby_model/original-build.json`. Its historical
`game_references` path records provenance; normal APK builds do not access it.

The embedded helper is `FTRez.LobbyModel.Entry`, revision
`lobby-direct-20261023`, compiled for Unity `2022.3.62f3` and URP `14.0.12`.
The original DLL and bundle remain in `app/src/main/cpp/self_model`:

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `lobby_model.dll.bytes` | 93696 | `6034d6cdf168282cb69371be5abf438e6244f0e3ef85fbed0a22ad63facc271f` |
| `lobby_model.bundle` | 25785 | `772b11903ac7e88b430e9267024edcf89d5cf84d71c85df61be026863a1de0df` |

Run from the standalone project root with Python 3.9 or newer:

```bash
python tools/verify_lobby_resources.py
```

The verifier uses only the standard library. It checks every source hash,
artifact size/hash, archived provenance, recorded GLES3/Vulkan shader
validation, and every current `LobbyEntry` lookup in `lobby_render.cpp`
against the API inspected from the compiled helper. It does not run Unity,
import UnityPy, or require game reference assemblies. A changed source or
missing helper API fails verification until a matching resource set is built.

Current native calls use these public static methods:

- `Initialize(System.Byte[]) -> System.Void`
- `SetTargets(UnityEngine.Transform, UnityEngine.Camera, UnityEngine.Transform[], UnityEngine.Renderer[], System.Int32[], System.Boolean) -> System.Void`
- `GetStatus() -> System.String`
- `SetGeometryHooks(System.IntPtr) -> System.Void`
- `WatchInstantiation(UnityEngine.AsyncOperation) -> System.Void`

For an intentional helper rebuild, supply both external tool paths explicitly:

```bash
python -m pip install -r managed/lobby_model/requirements.txt
python tools/build_lobby_resources.py --unity /path/to/Unity.exe --references /path/to/DummyDll
```

Unity must be version `2022.3.62f3` with Android support, its bundled Mono
compiler, .NET 4.8 reference assemblies, and Mono.Cecil `0.11.1`. The game
reference directory must contain the identities recorded in the manifest,
including `Unity.RenderPipelines.Universal.Runtime_30.dll` and
`Unity.RenderPipelines.Core.Runtime_10.dll`. The rebuilt helper's exact
reference hashes are recorded in its new manifest.

Each rebuild creates a unique directory under `build/lobby-resources/`.
The tool compiles the C# helper against the supplied game assemblies,
inspects its public API, builds the Android shader bundle in Unity, and
verifies executable shader programs for every required GLES3/Vulkan pass.
Source/reference hashes are checked again before staging the result.
Logs and resulting artifacts remain in that build directory.

Add `--publish` to the rebuild command to replace the embedded DLL, bundle,
and current manifest only after those checks pass. The previous embedded set
is retained in the build directory and restored if publishing fails. The
archived original manifest is never rewritten. Do not run a native APK build
concurrently with resource publishing.

The scripts inside `lobby_model/` are preserved historical build inputs;
their original path defaults are not the standalone build entry point.
Use `tools/build_lobby_resources.py` for standalone helper rebuilding.

`toon/` additionally preserves the original toon/GLTF shader assets, editor
builder, licenses, and notes. To build that shader bundle into this project's
build directory, pass all path arguments to its preserved builder:

```bash
python managed/toon/build.py --unity /path/to/Unity.exe --project build/unity-toon --assets managed/toon/Assets
```

This optional toon build leaves the existing embedded `ftrez-toon.bundle`
unchanged; its output is `build/unity-toon/Build/Android/ftrez-toon.bundle`.
