"""Validate executable Android lobby shader data, not just platform labels."""
import argparse
import hashlib
import json
from pathlib import Path

import UnityPy
from UnityPy.export.ShaderConverter import ShaderProgram
from UnityPy.helpers import CompressionHelper
from UnityPy.streams import EndianBinaryReader

PASSES = ('CoverageDepth', 'SelfBeforeUI', 'TransparentForegroundDepth', 'ReflectionSnapshot')
PLATFORMS = {9: ('OpenGLES3', {4, 5, 6}), 18: ('Vulkan', {25})}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify(data):
    shaders = [o for o in UnityPy.load(data).objects if o.type.name == 'Shader']
    require(len(shaders) == 1, 'Lobby bundle must contain exactly one shader')
    obj = shaders[0]
    tree = obj.read_typetree()
    parsed = tree['m_ParsedForm']
    require(parsed['m_Name'] == 'Hidden/FTRez/LobbyModel', 'Wrong lobby shader name')
    require(len(parsed['m_SubShaders']) == 1, 'Unexpected lobby SubShader count')
    sub = parsed['m_SubShaders'][0]
    require(dict(sub['m_Tags']['tags']).get('RenderPipeline') == 'UniversalPipeline', 'Missing URP tag')
    passes = sub['m_Passes']
    names = [p['m_State']['m_Name'] for p in passes]
    require(names == list(PASSES), f'Missing or reordered lobby passes: {names}')
    require(set(tree['platforms']) == set(PLATFORMS), 'Expected GLES3 and Vulkan programs')
    blob = bytes(tree['compressedBlob'])
    platforms = {}
    for i, platform in enumerate(tree['platforms']):
        name, types = PLATFORMS[platform]
        offsets = tree['offsets'][i]
        sizes = tree['compressedLengths'][i]
        decoded_sizes = tree['decompressedLengths'][i]
        require(len(offsets) == len(sizes) == len(decoded_sizes) == 1, f'{name}: unsupported program segment layout')
        offset, size = offsets[0], sizes[0]
        require(offset >= 0 and size > 0 and offset + size <= len(blob), f'{name}: invalid compressed range')
        raw = CompressionHelper.decompress_lz4(blob[offset:offset + size], decoded_sizes[0])
        require(len(raw) == decoded_sizes[0] and len(raw) > 4, f'{name}: empty GPU program table')
        programs = ShaderProgram(EndianBinaryReader(raw, endian='<'), obj.version).m_SubPrograms
        require(programs, f'{name}: empty GPU program table')
        pass_report = []
        for p in passes:
            require(platform in p['m_Platforms'], f'{name}: pass platform missing')
            references = []
            # Unity 2022 stores player programs in nested rows. GLES combines
            # vertex and fragment code, so an empty progFragment is legitimate.
            for stage in ('progVertex', 'progFragment'):
                prog = p[stage]
                references.extend(prog.get('m_SubPrograms', []))
                for row in prog.get('m_PlayerSubPrograms', []):
                    references.extend(row)
            matched = [r for r in references if r['m_GpuProgramType'] in types]
            require(matched, f'{name}: {p["m_State"]["m_Name"]} has no executable references')
            for ref in matched:
                index = ref['m_BlobIndex']
                require(0 <= index < len(programs), f'{name}: invalid program index {index}')
                program = programs[index]
                require(int(program.m_ProgramType) in types and len(program.m_ProgramCode) > 0,
                        f'{name}: empty or wrong program at index {index}')
            pass_report.append(dict(name=p['m_State']['m_Name'], executable_references=len(matched)))
        platforms[name] = dict(decompressed_bytes=len(raw), table_entries=len(programs), passes=pass_report)
    return dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), unitypy=UnityPy.__version__,
                unity=obj.assets_file.unity_version, shader=parsed['m_Name'], platforms=platforms)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = json.dumps(verify(args.bundle.read_bytes()), indent=2) + '\n'
    if args.report:
        args.report.write_text(result, encoding='utf-8')
    print(result)
