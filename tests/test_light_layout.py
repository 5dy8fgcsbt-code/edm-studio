import io,tempfile,unittest
from pathlib import Path
from edm_studio.edm.parser import EDMFileParser,EDMParseError
from edm_studio.scene import Scene
from edm_studio.gltf import export_scene
from tests.fixture_writer import make_edm
from tools.validate_roundtrip import GltfReader

class SpotLightLayoutTests(unittest.TestCase):
    def test_legacy_and_direction_records_preserve_following_geometry(self):
        for version in (8,10):
            with self.subTest(version=version):
                parser=EDMFileParser(io.BytesIO(make_edm(version=version,spot_layouts=(None,0,1,2))))
                parsed=parser.parse();self.assertEqual(parser.r.remaining(),0)
                self.assertEqual(len(parsed.render_nodes),5)
                self.assertEqual([n.type for n in parsed.render_nodes],['FakeSpotLightsNode']*4+['RenderNode'])
                self.assertEqual(parsed.render_nodes[-1].name,'Demo strut')
                scene=Scene(parsed);self.assertEqual(len(scene.meshes),1)
                self.assertTrue(any('FakeSpotLightsNode' in w for w in scene.warnings))
                with tempfile.TemporaryDirectory() as tmp:
                    path=Path(tmp)/'lights.glb';export_scene(scene,path,embed_textures=False)
                    reader=GltfReader(path)
                    self.assertEqual(len(reader.doc['meshes']),1)
                    self.assertEqual(len(reader.doc['animations']),2)
                    primitive=reader.doc['meshes'][0]['primitives'][0]
                    self.assertEqual(len(reader.accessor(primitive['indices'])),36)

    def test_unknown_spot_light_layout_is_not_silently_skipped(self):
        with self.assertRaisesRegex(EDMParseError,'Unsupported FakeSpotLightsNode layout'):
            EDMFileParser(io.BytesIO(make_edm(spot_layouts=(3,)))).parse()

if __name__=='__main__':unittest.main()
