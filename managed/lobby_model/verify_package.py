"""Verify that the APK embeds the current lobby helper and aligned ARM64 library."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile
from verify_bundle import verify
from verify_resources import verify_resources


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    source = Path(__file__).resolve().parent
    repo = source.parents[2]
    native = repo / 'lsp-ui/src/main/cpp/self_model'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', type=Path, default=repo / 'lsp-ui/build/outputs/apk/debug/lsp-ui-debug.apk')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    manifest_path = native / 'lobby_model.build.json'
    manifest = verify_resources(source, native)

    with zipfile.ZipFile(args.apk) as archive:
        libraries = [entry for entry in archive.infolist() if entry.filename.endswith('.so')]
        require(len(libraries) == 1 and libraries[0].filename == 'lib/arm64-v8a/libDLL.so',
                'APK must contain exactly lib/arm64-v8a/libDLL.so')
        entry = libraries[0]
        require(entry.compress_type == zipfile.ZIP_STORED, 'Native library is compressed')
        library = archive.read(entry)
        with args.apk.open('rb') as stream:
            stream.seek(entry.header_offset)
            header = stream.read(30)
        require(header[:4] == b'PK\x03\x04', 'Invalid ZIP local header')
        name_size, extra_size = struct.unpack_from('<HH', header, 26)
        library_offset = entry.header_offset + 30 + name_size + extra_size
        require(library_offset % 16384 == 0, 'Native library ZIP data is not 16 KB aligned')

    require(library[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', library, 18)[0] == 183,
            'Native library is not little-endian ELF64 AArch64')
    table = struct.unpack_from('<Q', library, 32)[0]
    stride, count = struct.unpack_from('<HH', library, 54)
    segments = []
    for index in range(count):
        kind, _, offset, address, _, file_size, memory_size, alignment = struct.unpack_from(
            '<IIQQQQQQ', library, table + index * stride)
        if kind != 1:
            continue
        require(alignment >= 16384 and (offset - address) % 16384 == 0, 'ELF LOAD segment is not 16 KB compatible')
        segments.append(dict(offset=offset, address=address, file_size=file_size, memory_size=memory_size, alignment=alignment))
    require(segments, 'ELF has no LOAD segments')

    artifacts = {}
    for name, expected in manifest['artifacts'].items():
        data = (native / name).read_bytes()
        require(len(data) == expected['bytes'] and sha256(data) == expected['sha256'], f'Changed helper artifact: {name}')
        require(library.count(data) == 1, f'APK does not contain exactly one current embedded {name}')
        artifacts[name] = dict(expected, elf_offset=library.index(data))

    embedded_bundle = artifacts['lobby_model.bundle']
    start = embedded_bundle['elf_offset']
    shader_report = verify(library[start:start + embedded_bundle['bytes']])
    require(shader_report == manifest['shader_validation'], 'Embedded shader validation differs from resource build')

    relevant_sources = [native / name for name in ('lobby_render.cpp', 'lobby_render.h', 'lobby_render.S', 'lobby_binding.h', 'lobby_geometry_hooks.h', 'unity_api.h', 'model_runtime.cpp', 'model_shared_pool.h', 'model_mesh_sharing.h')]
    relevant_sources += [repo / 'native-common/src/modules/impl/render/self_model.h', repo / 'lsp-ui/src/main/cpp/CMakeLists.txt',
                         repo / 'lsp-ui/src/main/cpp/scene/scene_locator.cpp',
                         repo / 'native-common/src/platform/android/external_overlay.h',
                         repo / 'native-common/src/platform/android/external_overlay.cpp',
                         repo / 'native-common/src/ui/naven_ui.h']
    report = dict(apk=str(args.apk.resolve()), bytes=args.apk.stat().st_size, sha256=sha256(args.apk.read_bytes()),
                  native_library=dict(path=entry.filename, bytes=len(library), sha256=sha256(library),
                                      zip_data_offset=library_offset, load_segments=segments),
                  embedded=artifacts, helper_manifest_sha256=sha256(manifest_path.read_bytes()),
                  native_sources={p.relative_to(repo).as_posix(): sha256(p.read_bytes()) for p in relevant_sources},
                  source_manifest_verified=True, helper_api=manifest['helper_api'], shader_validation=shader_report, build_pipeline=manifest['build_pipeline'],
                  signature='Run apksigner verify separately', in_game_validated=False)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: current source/reference/artifact hashes; exact embedded DLL and shader; one ARM64 library; ZIP/ELF 16 KB alignment')
    print(f'APK SHA256: {report["sha256"]}')


if __name__ == '__main__':
    main()
