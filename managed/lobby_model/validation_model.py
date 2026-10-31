"""Export a real GLB bind-pose fixture; never changes the delivered model.

Deliberately supports only the uncompressed, identity mesh transforms used by
the Saki source asset. Unsupported inputs fail instead of silently simplifying.
"""
import hashlib
import json
import shutil
import struct
from pathlib import Path


def prepare(model: Path, project: Path, tools: Path):
    raw = model.read_bytes()
    magic, version, length = struct.unpack_from('<III', raw)
    assert (magic, version, length) == (0x46546C67, 2, len(raw))
    length, kind = struct.unpack_from('<II', raw, 12)
    assert kind == 0x4E4F534A
    doc = json.loads(raw[20:20 + length])
    offset = 20 + length
    length, kind = struct.unpack_from('<II', raw, offset)
    assert kind == 0x004E4942
    data = raw[offset + 8:offset + 8 + length]
    destination = project / 'ValidationModel'
    destination.mkdir(exist_ok=True)

    def view(index):
        v = doc['bufferViews'][index]
        assert v.get('buffer', 0) == 0
        start = v.get('byteOffset', 0)
        return data[start:start + v['byteLength']]

    def accessor(index):
        a = doc['accessors'][index]
        assert 'sparse' not in a and not a.get('normalized', False)
        code = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[a['componentType']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        fmt = '<' + code * width
        stride = doc['bufferViews'][a['bufferView']].get('byteStride', struct.calcsize(fmt))
        chunk = view(a['bufferView'])
        return [struct.unpack_from(fmt, chunk, a.get('byteOffset', 0) + i * stride) for i in range(a['count'])]

    metadata = {'source': str(model.resolve()), 'sha256': hashlib.sha256(raw).hexdigest(), 'parts': []}
    for i, img in enumerate(doc.get('images', [])):
        assert img['mimeType'] == 'image/png'
        (destination / f'image-{i}.png').write_bytes(view(img['bufferView']))
    for node_index, node in enumerate(doc['nodes']):
        if 'mesh' not in node:
            continue
        current = node_index
        while current is not None:
            n = doc['nodes'][current]
            assert all(k not in n for k in ('matrix', 'translation', 'rotation', 'scale')), 'Fixture requires identity mesh ancestors'
            current = next((i for i, parent in enumerate(doc['nodes']) if current in parent.get('children', [])), None)
        for p in doc['meshes'][node['mesh']]['primitives']:
            assert p.get('mode', 4) == 4 and 'targets' not in p and 'extensions' not in p
            mat = doc['materials'][p['material']]
            extras = mat.get('extras', {})
            if not extras.get('ftRezRenderVisible', True):
                continue
            assert mat.get('alphaMode', 'OPAQUE') == 'OPAQUE'
            attrs = p['attributes']
            positions, normals, uvs = (accessor(attrs[k]) for k in ('POSITION', 'NORMAL', 'TEXCOORD_0'))
            indices = [v[0] for v in accessor(p['indices'])]
            assert len(positions) == len(normals) == len(uvs) and len(indices) % 3 == 0
            path = destination / f'part-{len(metadata["parts"])}.bin'
            with path.open('wb') as out:
                out.write(struct.pack('<II', len(positions), len(indices)))
                for pos, normal, uv in zip(positions, normals, uvs):
                    # Match model_glb.cpp: reflect Z, flip UV.y, reverse winding.
                    out.write(struct.pack('<8f', pos[0], pos[1], -pos[2], normal[0], normal[1], -normal[2], uv[0], 1 - uv[1]))
                for i in range(0, len(indices), 3):
                    out.write(struct.pack('<3I', indices[i], indices[i + 2], indices[i + 1]))
            pbr = mat.get('pbrMetallicRoughness', {})
            texture = pbr.get('baseColorTexture')
            floats = json.loads(extras.get('ftRezShaderFloats', '{}'))
            metadata['parts'].append({
                'name': mat['name'], 'file': path.name,
                'image': f'image-{doc["textures"][texture["index"]]["source"]}.png' if texture else '',
                'color': pbr.get('baseColorFactor', [1, 1, 1, 1]),
                'metallic': pbr.get('metallicFactor', 1), 'roughness': pbr.get('roughnessFactor', 1),
                'cull': 0 if mat.get('doubleSided') else 2,
                'floats': [{'name': k, 'value': v} for k, v in floats.items()]})
    assert len(metadata['parts']) >= 2
    shaders = project / 'Assets/ValidationShaders'
    for src in (tools / 'saki_renderer/Assets/SakiSource', tools / 'toon/Assets/FTRezToon', tools / 'toon/Assets/FTRezGLTF'):
        shutil.copytree(src, shaders / src.name, dirs_exist_ok=True)
    shutil.copyfile(tools / 'saki_renderer/Assets/SakiSource/source-masks.png', destination / 'source-masks.png')
    metadata['shaderSources'] = {str(p.relative_to(shaders)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in shaders.rglob('*') if p.is_file() and p.suffix != '.meta'}
    (destination / 'model.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    return destination / 'model.json'
