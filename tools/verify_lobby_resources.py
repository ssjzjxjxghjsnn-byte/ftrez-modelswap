"""Verify embedded lobby artifacts, preserved sources, and the current native API contract.

Uses only the Python standard library. Historical game-reference paths in the
build manifest are provenance, not dependencies of an APK build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'managed/lobby_model'
NATIVE = ROOT / 'app/src/main/cpp/self_model'
ORIGINAL_MANIFEST_SHA256 = '0abc1f749d8e763801dc4dffa55ae9c2bc54eb05573f4a707202972c81af5178'
ARTIFACT_NAMES = {'lobby_model.dll.bytes', 'lobby_model.bundle'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def source_files(source):
    paths = [p for folder in ('Assets', 'BuildTools')
             for p in (source / folder).rglob('*') if p.is_file()]
    return sorted(paths + list(source.glob('*.py')) + [source / 'requirements.txt'])


def source_hashes(source):
    return {p.relative_to(source).as_posix(): sha256(p) for p in source_files(source)}


def bridge_contract(text):
    # Ignore examples in comments; only actual calls declare dependencies.
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.DOTALL)
    calls = re.findall(
        r'\bapi\s*\.\s*method\s*\(\s*"LobbyEntry"\s*,\s*"([^"]+)"\s*,'
        r'\s*\{([^}]*)\}\s*,\s*"([^"]+)"\s*,\s*true\s*\)', text)
    declared = len(re.findall(r'\bapi\s*\.\s*method\s*\(\s*"LobbyEntry"', text))
    require(calls and len(calls) == declared,
            'Cannot parse every current LobbyEntry native method lookup')
    return sorted({name + '\t' + result + '\t' + ','.join(re.findall(r'"([^"]+)"', parameters))
                   for name, parameters, result in calls})


def original_manifest(source):
    path = source / 'original-build.json'
    require(sha256(path) == ORIGINAL_MANIFEST_SHA256,
            'Preserved original lobby build manifest changed')
    return json.loads(path.read_text(encoding='utf-8'))


def verify_resources(source=SOURCE, native=NATIVE, bridge=None):
    baseline = original_manifest(source)
    manifest = json.loads((native / 'lobby_model.build.json').read_text(encoding='utf-8'))
    expected_sources = manifest.get('sources', {})
    actual_sources = source_hashes(source)
    require(set(actual_sources) == set(expected_sources),
            'Helper source file list changed; rebuild the lobby resources')
    for name, digest in actual_sources.items():
        require(digest == expected_sources[name], 'Unbuilt helper source: ' + name)
    artifacts = manifest.get('artifacts', {})
    require(set(artifacts) == ARTIFACT_NAMES, 'Unexpected embedded lobby artifact set')
    for name, expected in artifacts.items():
        path = native / name
        require(path.stat().st_size == expected['bytes'] and sha256(path) == expected['sha256'],
                'Embedded helper artifact differs from build manifest: ' + name)
    derivative = manifest.get('standalone_build')
    if derivative is None:
        require(manifest == baseline, 'Original artifact provenance changed without a standalone rebuild')
    else:
        require(derivative.get('original_manifest_sha256') == ORIGINAL_MANIFEST_SHA256,
                'Rebuilt helper has no matching original provenance')
    shader = manifest.get('shader_validation', {})
    bundle = artifacts['lobby_model.bundle']
    require(shader.get('sha256') == bundle['sha256'] and shader.get('bytes') == bundle['bytes'],
            'Shader validation does not describe the embedded bundle')
    require(set(shader.get('platforms', {})) == {'OpenGLES3', 'Vulkan'},
            'Embedded lobby shader is missing an Android graphics API')
    api = manifest.get('helper_api', {})
    require(bool(api.get('revision')), 'Embedded helper revision missing')
    contract = bridge_contract((bridge or native / 'lobby_render.cpp').read_text(encoding='utf-8'))
    for method in contract:
        require(method in api.get('methods', []), 'Embedded helper API missing: ' + method)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--native', type=Path, default=NATIVE)
    parser.add_argument('--bridge', type=Path, help='Native bridge source when verifying staged artifacts')
    args = parser.parse_args()
    try:
        manifest = verify_resources(args.source, args.native, args.bridge)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        return 1
    print('PASS: lobby source hashes, embedded artifacts, and native API contract; '
          + manifest['helper_api']['revision'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
