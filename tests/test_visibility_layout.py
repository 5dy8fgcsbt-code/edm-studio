import io,tempfile,unittest
from pathlib import Path
import numpy as np
from edm_studio.edm.parser import EDMFileParser,EDMParseError
from edm_studio.scene import Scene
from edm_studio.gltf import export_scene
from tests.fixture_writer import make_edm
from tools.validate_roundtrip import GltfReader

class VisibilityLayoutTests(unittest.TestCase):
    def test_versioned_matrix_preserves_stream_and_visibility_animation(self):
        for version in (8,10):
            with self.subTest(version=version):
                parser=EDMFileParser(io.BytesIO(make_edm(version=version,visibility_version=1)))
                parsed=parser.parse();self.assertEqual(parser.r.remaining(),0)
                self.assertEqual(len(parsed.render_nodes),1)
                np.testing.assert_array_equal(parsed.nodes[2].trailing_matrix,np.eye(4).ravel())
                self.assertEqual(parsed.nodes[2].vis_data,[(40,[(0.,.75)])])
                scene=Scene(parsed);mesh=scene.meshes[0]
                with tempfile.TemporaryDirectory() as tmp:
                    path=Path(tmp)/'visibility.glb'
                    export_scene(scene,path,arguments=[40],embed_textures=False)
                    reader=GltfReader(path);clip=reader.doc['animations'][0]
                    for value in (0.,.74,.75,1.):
                        expected=scene.evaluate({40:value})
                        actual=reader.evaluate(clip,value*clip['extras']['duration_seconds'])
                        np.testing.assert_allclose(actual,expected,atol=1e-6)
                        visible=abs(np.linalg.det(actual[mesh.node,:3,:3]))>.5
                        self.assertEqual(visible,value<.75)

    def test_unknown_layout_and_unverified_nonidentity_matrix_fail_explicitly(self):
        with self.assertRaisesRegex(EDMParseError,'Unsupported ArgVisibilityNode layout'):
            EDMFileParser(io.BytesIO(make_edm(visibility_version=2))).parse()
        transform=np.eye(4);transform[3,0]=2
        parsed=EDMFileParser(io.BytesIO(make_edm(visibility_version=1,visibility_matrix=transform.ravel()))).parse()
        with self.assertRaisesRegex(ValueError,'非单位可见性扩展矩阵'):
            Scene(parsed)

if __name__=='__main__':unittest.main()
