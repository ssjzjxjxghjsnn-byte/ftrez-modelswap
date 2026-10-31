"""Reject stale lobby resources before compiling or packaging the native bridge."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise RuntimeError(message + '; rebuild with: python lsp-ui/model_tools/lobby_model/build.py')


def source_files(source):
    paths = [p for folder in ('Assets', 'BuildTools') for p in (source / folder).rglob('*') if p.is_file()]
    return sorted(paths + list(source.glob('*.py')) + [source / 'requirements.txt'])


def bridge_contract(text):
    calls = re.findall(r'api\.method\("LobbyEntry",\s*"([^\"]+)",\s*\{([^}]*)\},\s*"([^\"]+)",\s*true\)', text)
    require(calls, 'No managed bridge contract found')
    return sorted(name + '\t' + result + '\t' + ','.join(re.findall(r'"([^\"]+)"', parameters))
                  for name, parameters, result in calls)


def verify_resources(source, native):
    manifest = json.loads((native / 'lobby_model.build.json').read_text(encoding='utf-8'))
    files = {p.relative_to(source).as_posix(): p for p in source_files(source)}
    require(set(files) == set(manifest['sources']), 'Helper source file list changed since the full resource build')
    for name, path in files.items():
        require(hashlib.sha256(path.read_bytes()).hexdigest() == manifest['sources'][name], 'Unbuilt helper source: ' + name)
    for name, expected in manifest['assembly_references'].items():
        require(hashlib.sha256((Path(manifest['game_references']) / name).read_bytes()).hexdigest() == expected,
                'Changed game assembly reference: ' + name)
    for name, expected in manifest['artifacts'].items():
        data = (native / name).read_bytes()
        require(len(data) == expected['bytes'] and hashlib.sha256(data).hexdigest() == expected['sha256'],
                'Changed helper artifact: ' + name)
    api = manifest.get('helper_api', {})
    for method in bridge_contract((native / 'lobby_render.cpp').read_text(encoding='utf-8')):
        require(method in api.get('methods', []), 'Embedded helper API missing: ' + method)
    require(api.get('revision'), 'Embedded helper revision missing')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    source = Path(__file__).resolve().parent
    manifest = verify_resources(source, source.parents[2] / 'lsp-ui/src/main/cpp/self_model')
    print('PASS: lobby resources and native API contract; ' + manifest['helper_api']['revision'])
