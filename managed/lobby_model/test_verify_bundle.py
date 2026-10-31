"""Regression tests serialize broken bundles through the production validator."""
from pathlib import Path
import unittest
import UnityPy
from verify_bundle import verify

BUNDLE = Path(__file__).resolve().parents[2] / 'src/main/cpp/self_model/lobby_model.bundle'


class BundleTests(unittest.TestCase):
    def broken(self, mutate):
        env = UnityPy.load(BUNDLE.read_bytes())
        shader = next(o for o in env.objects if o.type.name == 'Shader')
        tree = shader.read_typetree()
        mutate(tree)
        shader.save_typetree(tree)
        return next(iter(env.files.values())).save()

    def test_android_programs(self):
        result = verify(BUNDLE.read_bytes())
        self.assertEqual(set(result['platforms']), {'OpenGLES3', 'Vulkan'})

    def test_empty_passes(self):
        data = self.broken(lambda t: t['m_ParsedForm']['m_SubShaders'][0].update(m_Passes=[]))
        with self.assertRaisesRegex(RuntimeError, 'Missing or reordered'):
            verify(data)

    def test_reordered_passes(self):
        data = self.broken(lambda t: t['m_ParsedForm']['m_SubShaders'][0]['m_Passes'].reverse())
        with self.assertRaisesRegex(RuntimeError, 'Missing or reordered'):
            verify(data)

    def test_missing_platform(self):
        data = self.broken(lambda t: t.update(platforms=[9]))
        with self.assertRaisesRegex(RuntimeError, 'Expected GLES3 and Vulkan'):
            verify(data)

    def test_invalid_program_reference(self):
        def mutate(tree):
            p = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
            for row in p['progVertex']['m_PlayerSubPrograms']:
                for program in row:
                    program['m_BlobIndex'] = 999999
        data = self.broken(mutate)
        with self.assertRaisesRegex(RuntimeError, 'invalid program index'):
            verify(data)


if __name__ == '__main__':
    unittest.main()
