"""Explicit lobby helper rebuild; normal APK builds use the verified embedded artifacts."""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from verify_lobby_resources import (ARTIFACT_NAMES, NATIVE, ORIGINAL_MANIFEST_SHA256,
                                    ROOT, SOURCE, bridge_contract, original_manifest,
                                    require, sha256, source_hashes, verify_resources)


def run_logged(command, log, timeout=180):
    result = subprocess.run([str(part) for part in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', timeout=timeout)
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'Command failed with exit code {result.returncode}; see {log}\n'
                           + result.stdout[-6000:])
    return result.stdout


def unity_tools(unity):
    require(unity.is_file(), 'Unity editor executable not found: ' + str(unity))
    roots = (unity.parent / 'Data', unity.parent.parent, unity.parent)
    data = next((p for p in roots if (p / 'MonoBleedingEdge').is_dir()), None)
    require(data is not None, 'Cannot find MonoBleedingEdge beside the supplied Unity editor')
    mono_root = data / 'MonoBleedingEdge'
    mono = next((mono_root / 'bin' / name for name in ('mono.exe', 'mono')
                 if (mono_root / 'bin' / name).is_file()), None)
    compiler = mono_root / 'lib/mono/4.5/csc.exe'
    framework = mono_root / 'lib/mono/4.8-api'
    cecil = mono_root / 'lib/mono/gac/Mono.Cecil/0.11.1.0__0738eb9f132ed756/Mono.Cecil.dll'
    require(mono is not None and compiler.is_file() and framework.is_dir() and cecil.is_file(),
            'Unity must include Mono, C# compiler, .NET 4.8 reference assemblies, and Mono.Cecil 0.11.1')
    return mono, compiler, framework, cecil


def bundle_verifier():
    spec = importlib.util.spec_from_file_location('standalone_lobby_verify_bundle', SOURCE / 'verify_bundle.py')
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except ImportError as error:
        raise RuntimeError('Shader rebuild verification requires: python -m pip install -r '
                           'managed/lobby_model/requirements.txt') from error
    return module.verify


def publish(staged, run_dir):
    """Publish only a validated set, retaining and restoring the prior set on failure."""
    verify_resources(SOURCE, staged, NATIVE / 'lobby_render.cpp')
    names = sorted(ARTIFACT_NAMES) + ['lobby_model.build.json']
    backup = run_dir / 'previous-embedded'
    backup.mkdir()
    for name in names:
        shutil.copy2(NATIVE / name, backup / name)
    tag = uuid.uuid4().hex
    temporary = {name: NATIVE / (name + '.publish-' + tag) for name in names}
    replaced = []
    try:
        for name in names:
            shutil.copy2(staged / name, temporary[name])
        for name in names:
            os.replace(temporary[name], NATIVE / name)
            replaced.append(name)
        verify_resources()
    except BaseException:
        for name in replaced:
            shutil.copy2(backup / name, NATIVE / name)
        raise
    finally:
        for path in temporary.values():
            if path.exists():
                path.unlink()


def build(args):
    baseline = original_manifest(SOURCE)
    input_hashes = source_hashes(SOURCE)
    unity = args.unity.resolve()
    references = args.references.resolve()
    require(references.is_dir(), 'Game reference directory not found: ' + str(references))
    mono, compiler, framework, cecil = unity_tools(unity)
    verify_bundle = bundle_verifier()
    for name in baseline['assembly_references']:
        require((references / name).is_file(), 'Required game assembly missing: ' + name)
    game_assemblies = [p for p in sorted(references.glob('*.dll'))
                       if p.name not in ('mscorlib.dll', 'netstandard.dll', 'Microsoft.CSharp.dll')
                       and not p.name.startswith('System')]
    framework_assemblies = [framework / name for name in ('mscorlib.dll', 'System.dll', 'System.Core.dll')]
    for path in framework_assemblies:
        require(path.is_file(), 'Framework reference missing: ' + str(path))
    tracked_references = {p.name: p for p in game_assemblies}
    tracked_references.update({name: references / name for name in baseline['assembly_references']})
    reference_hashes = {name: sha256(path) for name, path in sorted(tracked_references.items())}
    framework_hashes = {p.name: sha256(p) for p in framework_assemblies}
    native_contract = bridge_contract((NATIVE / 'lobby_render.cpp').read_text(encoding='utf-8'))

    build_root = (ROOT / 'build/lobby-resources').resolve()
    require(build_root.is_relative_to(ROOT.resolve()), 'Build directory resolves outside the standalone project')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8]
    run_dir = build_root / stamp
    project = run_dir / 'unity-project'
    (project / 'Packages').mkdir(parents=True)
    (project / 'ProjectSettings').mkdir()
    dependencies = {
        'com.unity.render-pipelines.universal': baseline['urp_reference'],
        'com.unity.ugui': '1.0.0',
        'com.unity.modules.assetbundle': '1.0.0',
        'com.unity.modules.physics': '1.0.0',
        'com.unity.modules.jsonserialize': '1.0.0',
    }
    (project / 'Packages/manifest.json').write_text(
        json.dumps({'dependencies': dependencies}, indent=2) + '\n', encoding='utf-8')
    (project / 'ProjectSettings/ProjectVersion.txt').write_text(
        'm_EditorVersion: ' + baseline['unity'] + '\n', encoding='utf-8')
    shutil.copytree(SOURCE / 'Assets', project / 'Assets')
    code = project / 'FTRez.LobbyModel.dll'
    response = project / 'game-compile.rsp'
    response.write_text('\n'.join([
        '/nostdlib+', '/target:library', '/optimize+', '/langversion:9.0', '/deterministic+',
        f'/out:"{code}"',
        *[f'/reference:"{dll}"' for dll in game_assemblies + framework_assemblies],
        *[f'"{path}"' for path in sorted((SOURCE / 'Assets/LobbyModel').glob('*.cs'))]
    ]) + '\n', encoding='utf-8')
    print('Building lobby helper in ' + str(run_dir), flush=True)
    run_logged([mono, compiler, '/noconfig', '@' + str(response)], run_dir / 'compile.log')
    inspector = project / 'InspectHelper.exe'
    run_logged([mono, compiler, '/nologo', '/out:' + str(inspector),
                '/reference:' + str(cecil), SOURCE / 'BuildTools/InspectHelper.cs'], run_dir / 'inspector-compile.log')
    inspected = run_logged([mono, inspector, code], run_dir / 'helper-api.txt')
    revisions = [line[len('revision\t'):] for line in inspected.splitlines() if line.startswith('revision\t')]
    require(len(revisions) == 1 and revisions[0], 'Compiled helper has no unique revision')
    api = {'revision': revisions[0],
           'methods': sorted(line[len('method\t'):] for line in inspected.splitlines() if line.startswith('method\t'))}
    for method in native_contract:
        require(method in api['methods'], 'Compiled helper API missing: ' + method)

    unity_log = run_dir / 'unity-build.log'
    run_logged([unity, '-batchmode', '-nographics', '-projectPath', project, '-buildTarget', 'Android',
                '-executeMethod', 'BuildLobbyModel.Build', '-quit', '-logFile', unity_log],
               run_dir / 'unity-launch.log', args.timeout)
    bundle = project / 'Build/Android/lobby-model.bundle'
    shader_report = verify_bundle(bundle.read_bytes())
    require(shader_report['unity'] == baseline['unity'], 'Shader built with an unexpected Unity version')
    pipeline_report = json.loads((project / 'Build/Android/pipeline.json').read_text(encoding='utf-8'))
    require(pipeline_report == baseline['build_pipeline'], 'Lobby shader pipeline contract changed')
    require(input_hashes == source_hashes(SOURCE), 'Helper source changed during rebuild')
    require(reference_hashes == {name: sha256(path) for name, path in sorted(tracked_references.items())},
            'Game assembly reference changed during rebuild')
    require(framework_hashes == {p.name: sha256(p) for p in framework_assemblies},
            'Framework assembly reference changed during rebuild')

    staged = run_dir / 'artifacts'
    staged.mkdir()
    shutil.copyfile(code, staged / 'lobby_model.dll.bytes')
    shutil.copyfile(bundle, staged / 'lobby_model.bundle')
    manifest = {
        'unity': baseline['unity'], 'urp_reference': baseline['urp_reference'],
        'game_references': str(references),
        'artifacts': {name: {'bytes': (staged / name).stat().st_size, 'sha256': sha256(staged / name)}
                      for name in sorted(ARTIFACT_NAMES)},
        'sources': input_hashes, 'assembly_references': reference_hashes,
        'shader_validation': shader_report, 'build_pipeline': pipeline_report, 'helper_api': api,
        'standalone_build': {
            'original_manifest_sha256': ORIGINAL_MANIFEST_SHA256,
            'created_utc': datetime.now(timezone.utc).isoformat(),
            'unity_executable': str(unity), 'framework_references': framework_hashes,
            'builder_sha256': sha256(Path(__file__).resolve()),
            'verifier_sha256': sha256(ROOT / 'tools/verify_lobby_resources.py'),
            'native_contract': native_contract,
        },
    }
    (staged / 'lobby_model.build.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    verify_resources(SOURCE, staged, NATIVE / 'lobby_render.cpp')
    if args.publish:
        publish(staged, run_dir)
        print('Published verified lobby resources; previous artifacts retained in ' + str(run_dir / 'previous-embedded'))
    else:
        print('Verified rebuilt artifacts: ' + str(staged))
        print('Embedded APK resources were not replaced. Use --publish on an explicit rebuild to publish a verified set.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unity', type=Path, required=True, help='Unity 2022.3.62f3 editor executable with Android support')
    parser.add_argument('--references', type=Path, required=True, help='Game DummyDll/reference assembly directory')
    parser.add_argument('--publish', action='store_true', help='Publish only after assembly, shader, and contract validation')
    parser.add_argument('--timeout', type=int, default=900, help='Unity build timeout in seconds (default: 900)')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    try:
        build(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
