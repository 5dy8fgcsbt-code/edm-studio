import io,tempfile,unittest
from pathlib import Path
import numpy as np
from edm_studio.scene import Scene,orient_indices
from edm_studio.math3d import projection_planes
from edm_studio.edm import types as t
from edm_studio.edm.parser import EDMFileParser
from edm_studio.gltf import export_scene
from tests.fixture_writer import make_edm
from tools.validate_roundtrip import GltfReader

class SurfaceTests(unittest.TestCase):
    def test_mirrored_edm_winding_survives_export(self):
        edm=EDMFileParser(io.BytesIO(make_edm())).parse()
        mat=edm.materials[0];mat.vertex_format=t.VertexFormat((4,3));mat.textures=[]
        edm.nodes=[t.TransformNode('TransformNode','Mirrored',0,matrix=tuple(np.diag([-1.,1.,1.,1.]).ravel()))]
        vertices=np.array([[0,0,0,0,0,0,1],[1,0,0,0,0,0,1],[0,1,0,0,0,0,1]],dtype='<f4')
        edm.render_nodes=[t.RenderNode('RenderNode','Panel',0,material_id=0,parents=[t.ParentEntry(0)],vertex_data=vertices,index_data=[0,2,1])]
        edm.connectors=[];scene=Scene(edm);mesh=scene.meshes[0]
        self.assertTrue(mesh.extras['edm_winding_reversed'])
        np.testing.assert_array_equal(mesh.indices,[0,1,2])
        expected=np.array([[0,0,0],[-1,0,0],[0,1,0]])
        np.testing.assert_allclose(scene.transformed(mesh,scene.evaluate({})),expected)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'panel.glb';export_scene(scene,path,embed_textures=False)
            g=GltfReader(path);prim=g.doc['meshes'][0]['primitives'][0]
            np.testing.assert_array_equal(g.accessor(prim['indices'])[:,0],[0,1,2])
            self.assertFalse(g.doc['materials'][0]['doubleSided'])
            self.assertLess(np.linalg.det(g.evaluate()[mesh.node,:3,:3]),0)

    def test_preserve_consistent_and_ambiguous_faces(self):
        pos=np.array([[0,0,0],[1,0,0],[0,1,0]],dtype=np.float32)
        normal=np.tile([0,0,1.],(3,1))
        for indices in [[0,1,2],[0,1,2,0,2,1],[0,0,0]]:
            original=np.array(indices,dtype=np.uint32)
            result,changed=orient_indices(pos,normal,original)
            self.assertFalse(changed);np.testing.assert_array_equal(result,original)

    def test_closeup_resolves_submillimetre_surfaces(self):
        def depth(z,n,f):return f/(f-n)-f*n/((f-n)*z)
        n,f=projection_planes(12,14);steps=2**24-1
        self.assertGreater((depth(12.0002,n,f)-depth(12,n,f))*steps,1)
        self.assertLess((depth(12.0002,.001,100)-depth(12,.001,100))*steps,1)
        for d in [.001,.1,1,12,40,1000]:
            near,far=projection_planes(d,14)
            self.assertLess(near,d);self.assertGreater(far,d)

if __name__=='__main__':unittest.main()
