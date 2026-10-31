"""Exercise the build guard against stale DLLs and changed native API requirements."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from verify_resources import source_files, verify_resources


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.source, self.native, self.refs = (root / p for p in ('source', 'native', 'refs'))
        for path in (self.source / 'Assets', self.native, self.refs): path.mkdir(parents=True)
        (self.source / 'Assets/Entry.cs').write_text('fixture', encoding='utf-8')
        (self.source / 'requirements.txt').write_text('', encoding='utf-8')
        (self.native / 'lobby_render.cpp').write_text(
            'api.method("LobbyEntry","SetGeometryHooks",{"System.IntPtr"},"System.Void",true);', encoding='utf-8')
        (self.native / 'lobby_model.dll.bytes').write_bytes(b'current helper fixture')
        self.manifest = dict(sources={p.relative_to(self.source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in source_files(self.source)},
                             game_references=str(self.refs), assembly_references={},
                             artifacts={'lobby_model.dll.bytes': dict(bytes=22, sha256=hashlib.sha256(b'current helper fixture').hexdigest())},
                             helper_api=dict(revision='fixture', methods=['SetGeometryHooks\tSystem.Void\tSystem.IntPtr']))
        self.write_manifest()

    def write_manifest(self):
        (self.native / 'lobby_model.build.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    def test_matching_build(self):
        self.assertEqual(verify_resources(self.source, self.native)['helper_api']['revision'], 'fixture')

    def test_stale_helper(self):
        (self.native / 'lobby_model.dll.bytes').write_bytes(b'old helper')
        with self.assertRaisesRegex(RuntimeError, 'Changed helper artifact'):
            verify_resources(self.source, self.native)

    def test_changed_source(self):
        (self.source / 'Assets/Entry.cs').write_text('new implementation', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'Unbuilt helper source'):
            verify_resources(self.source, self.native)

    def test_new_source(self):
        (self.source / 'Assets/Hooks.cs').write_text('new component', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'source file list changed'):
            verify_resources(self.source, self.native)

    def test_missing_method_or_wrong_signature(self):
        for methods in ([], ['SetGeometryHooks\tSystem.Void\tSystem.Int32']):
            self.manifest['helper_api']['methods'] = methods
            self.write_manifest()
            with self.assertRaisesRegex(RuntimeError, 'Embedded helper API missing: SetGeometryHooks'):
                verify_resources(self.source, self.native)

    def test_new_native_requirement(self):
        with (self.native / 'lobby_render.cpp').open('a', encoding='utf-8') as output:
            output.write('api.method("LobbyEntry","NewMethod",{},"System.Void",true);')
        with self.assertRaisesRegex(RuntimeError, 'Embedded helper API missing: NewMethod'):
            verify_resources(self.source, self.native)


if __name__ == '__main__':
    unittest.main()
