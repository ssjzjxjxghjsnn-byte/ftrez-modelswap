"""Build the helper against the game's actual assembly identities and its shader in Unity."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from verify_bundle import verify
from verify_resources import source_files, bridge_contract, require


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--unity', type=Path, default=Path('E:/Unity/2022.3.62f3/Editor/Unity.exe'))
    parser.add_argument('--references', type=Path, default=Path('D:/Il2CppDumper-win-v6.7.46/DummyDll'))
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--validate', action='store_true')
    parser.add_argument('--validation-model', type=Path, help='Real Saki GLB for additional bind-pose GPU validation')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    input_hashes = {p.relative_to(source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in source_files(source)}
    repo = source.parents[2]
    project = repo / 'build/unity-lobby-model'
    native = repo / 'lsp-ui/src/main/cpp/self_model'
    (project / 'Packages').mkdir(parents=True, exist_ok=True)
    (project / 'ProjectSettings').mkdir(exist_ok=True)
    (project / 'Packages/manifest.json').write_text(json.dumps({'dependencies': {
        'com.unity.render-pipelines.universal': '14.0.12',
        'com.unity.ugui': '1.0.0',
        'com.unity.modules.assetbundle': '1.0.0',
        'com.unity.modules.physics': '1.0.0',
        'com.unity.modules.jsonserialize': '1.0.0'
    }}, indent=2), encoding='utf-8')
    (project / 'ProjectSettings/ProjectVersion.txt').write_text('m_EditorVersion: 2022.3.62f3\n', encoding='utf-8')
    shutil.copytree(source / 'Assets', project / 'Assets', dirs_exist_ok=True)
    if args.validation_model and not args.validate:
        parser.error('--validation-model requires --validate')
    marker = project / 'ValidationModel/enabled.txt'
    if marker.exists():
        marker.unlink()
    if args.validation_model:
        from validation_model import prepare
        fixture = prepare(args.validation_model, project, source.parent)
        marker.write_text(str(fixture), encoding='utf-8')
    framework = args.unity.parent / 'Data/MonoBleedingEdge/lib/mono/4.8-api'
    assemblies = [path for path in sorted(args.references.glob('*.dll'))
                  if path.name not in ('mscorlib.dll', 'netstandard.dll', 'Microsoft.CSharp.dll')
                  and not path.name.startswith('System')]
    assemblies += [framework / name for name in ('mscorlib.dll', 'System.dll', 'System.Core.dll')]
    required = ['mscorlib.dll', 'UnityEngine.CoreModule.dll', 'UnityEngine.UIModule.dll',
                'Unity.RenderPipelines.Universal.Runtime_30.dll', 'Unity.RenderPipelines.Core.Runtime_10.dll']
    for name in required:
        if not (args.references / name).is_file():
            raise FileNotFoundError(args.references / name)
    reference_hashes = {name: hashlib.sha256((args.references / name).read_bytes()).hexdigest() for name in required}
    code = project / 'FTRez.LobbyModel.dll'
    response = project / 'game-compile.rsp'
    inputs = sorted((source / 'Assets/LobbyModel').glob('*.cs'))
    response.write_text('\n'.join([
        '/nostdlib+', '/target:library', '/optimize+', '/langversion:9.0', '/deterministic+',
        f'/out:"{code}"', *[f'/reference:"{dll}"' for dll in assemblies],
        *[f'"{path}"' for path in inputs]
    ]), encoding='utf-8')
    mono = args.unity.parent / 'Data/MonoBleedingEdge/bin/mono.exe'
    compiler = args.unity.parent / 'Data/MonoBleedingEdge/lib/mono/4.5/csc.exe'
    compiled = subprocess.run([str(mono), str(compiler), '/noconfig', '@' + str(response)], capture_output=True, text=True)
    (project / 'game-compile.log').write_text(compiled.stdout + compiled.stderr, encoding='utf-8')
    print(compiled.stdout[:12000])
    compiled.check_returncode()
    if args.compile_only:
        print(code)
        return
    cecil = args.unity.parent / 'Data/MonoBleedingEdge/lib/mono/gac/Mono.Cecil/0.11.1.0__0738eb9f132ed756/Mono.Cecil.dll'
    inspector = project / 'InspectHelper.exe'
    subprocess.run([str(mono), str(compiler), '/nologo', '/out:' + str(inspector),
                    '/reference:' + str(cecil), str(source / 'BuildTools/InspectHelper.cs')], check=True)
    inspected = subprocess.check_output([str(mono), str(inspector), str(code)], text=True)
    api = {'methods': sorted(line[len('method\t'):] for line in inspected.splitlines() if line.startswith('method\t')),
           'revision': next(line[len('revision\t'):] for line in inspected.splitlines() if line.startswith('revision\t'))}
    for method in bridge_contract((native / 'lobby_render.cpp').read_text(encoding='utf-8')):
        require(method in api['methods'], 'Compiled helper API missing: ' + method)
    method = 'ValidateLobbyModel.Run' if args.validate else 'BuildLobbyModel.Build'
    log = project / ('validation.log' if args.validate else 'build.log')
    subprocess.run([str(args.unity), '-batchmode', *(['-force-d3d11'] if args.validate else ['-nographics']),
                    '-projectPath', str(project), '-buildTarget', 'Android', '-executeMethod', method,
                    *([] if args.validate else ['-quit']), '-logFile', str(log)], check=True, timeout=300)
    if args.validate:
        print((project / 'Validation/result.txt').read_text(encoding='utf-8'))
        return
    bundle = project / 'Build/Android/lobby-model.bundle'
    shader_report = verify(bundle.read_bytes())
    pipeline_report = json.loads((project / 'Build/Android/pipeline.json').read_text(encoding='utf-8'))
    require(input_hashes == {p.relative_to(source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in source_files(source)}, 'Helper source changed during resource build')
    require(reference_hashes == {name: hashlib.sha256((args.references / name).read_bytes()).hexdigest()
                                for name in required}, 'Game reference changed during resource build')
    # Do not replace working embedded artifacts until final shader validation passes.
    shutil.copyfile(code, native / 'lobby_model.dll.bytes')
    shutil.copyfile(bundle, native / 'lobby_model.bundle')
    manifest = {'unity': '2022.3.62f3', 'urp_reference': '14.0.12', 'game_references': str(args.references),
                'artifacts': {}, 'sources': {}, 'assembly_references': {},
                'shader_validation': shader_report, 'build_pipeline': pipeline_report, 'helper_api': api}
    for path in [native / 'lobby_model.dll.bytes', native / 'lobby_model.bundle']:
        manifest['artifacts'][path.name] = {'bytes': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    manifest['sources'] = input_hashes
    manifest['assembly_references'] = reference_hashes
    (native / 'lobby_model.build.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest['artifacts'], indent=2))


if __name__ == '__main__':
    main()
