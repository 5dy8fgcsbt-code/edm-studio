import io,unittest,tempfile,struct,json
from pathlib import Path
import numpy as np
from edm_studio.edm.parser import EDMFileParser,EDMParseError
from edm_studio.edm import types as t
from edm_studio.scene import Scene,load_scene
from edm_studio.math3d import *
from edm_studio.gltf import export_scene
from tests.fixture_writer import make_edm
from tools.validate_roundtrip import GltfReader

class ConversionTests(unittest.TestCase):
    def scene(self,**kwargs):return Scene(EDMFileParser(io.BytesIO(make_edm(**kwargs))).parse())
    def test_versions_and_number_mesh(self):
        for version in [8,10]:
            for number in [False,True]:
                s=self.scene(version=version,number=number)
                self.assertEqual(sum(len(m.indices) for m in s.meshes),36)
                self.assertEqual(s.edm.version,version)
                if number:self.assertEqual(s.edm.render_nodes[0].props['number_controls'][0][2],32)

    def test_truncation_unknown_and_trailing_are_errors(self):
        data=make_edm()
        for raw in [data[:2],data[:-1],data+b'junk',b'XYZ'+data[3:],b'EDM'+struct.pack('<H',99)]:
            with self.assertRaises((ValueError,EDMParseError,struct.error)):EDMFileParser(io.BytesIO(raw)).parse()

    def test_negative_domain_and_visibility_end_exclusive(self):
        s=self.scene();self.assertEqual(s.limits[0],[-1,1])
        tr=next(tr for tr in s.tracks if tr.ranges is not None)
        np.testing.assert_array_equal(tr.evaluate(0),[1,1,1])
        np.testing.assert_array_equal(tr.evaluate(.75),[0,0,0])

    def test_scale_orientation_native_formula(self):
        s=self.scene();node=s.edm.nodes[1]
        node.rot_data=[];node.base.scale=(2,3,4);node.base.quat1=(1,0,0,0)
        s=Scene(s.edm);m=s.evaluate({})[s.tails[1]]
        q=rotation(xyzw(node.base.quat2))
        np.testing.assert_allclose(m,translation(node.base.position)@q.T@scaling([2,3,4])@q,atol=1e-12)

    def test_quaternion_antipodes_follow_short_arc(self):
        q=quat([.2,.3,.4,.5]);np.testing.assert_allclose(rotation(slerp(q,-q,.5)),rotation(q),atol=1e-12)

    def test_glb_gltf_roundtrip_and_independent_clip_reset(self):
        s=self.scene(number=True)
        with tempfile.TemporaryDirectory() as td:
            for ext in ['.glb','.gltf']:
                output=Path(td)/('fixture'+ext)
                export_scene(s,output,embed_textures=False)
                g=GltfReader(output)
                self.assertEqual(len(g.doc['animations']),2)
                for clip in g.doc['animations']:
                    lo=clip['extras']['argument_min'];hi=clip['extras']['argument_max'];arg=clip['extras']['edm_argument']
                    for time in [.2,1.1,2.6]:
                        np.testing.assert_allclose(g.evaluate(clip,time),s.evaluate({arg:lo+(hi-lo)*time/3}),atol=1e-6)
                self.assertTrue(output.with_suffix('.report.json').is_file())

    def test_skin_implicit_weight_and_offset(self):
        s=self.scene();edm=s.edm
        edm.nodes=[t._NodeBase('Node','Root',0),t.BoneNode('Bone','Base',0,parent_idx=0,matrix1=tuple(np.eye(4).ravel()),matrix2=tuple(np.eye(4).ravel())),
                   t.ArgAnimationNode('ArgAnimatedBone','Joint',0,parent_idx=0,bone_transform=tuple(np.eye(4).ravel()),pos_data=[(3,[t.AnimatedKey(0,(0,0,0)),t.AnimatedKey(1,(2,0,0))])])]
        channels=[4,3]+[0]*19+[4]
        edm.materials[0].vertex_format=t.VertexFormat(tuple(channels))
        v=np.array([[0,0,0,0,0,1,0,.25,0,0,0],[1,0,0,0,0,1,0,.25,0,0,0],[0,1,0,0,0,1,0,.25,0,0,0]],dtype='<f4')
        edm.render_nodes=[t.SkinNode('SkinNode','Weighted',0,material_id=0,bones=[1,2],vertex_data=v,index_data=[0,1,2])]
        s=Scene(edm);m=s.meshes[0]
        self.assertEqual(m.joints.shape[1],8)
        np.testing.assert_allclose(m.weights[0],[.25,0,0,0,.75,0,0,0])
        self.assertEqual(m.joints[0,0],1)
        np.testing.assert_allclose(s.transformed(m,s.evaluate({3:1})),v[:,:3]+[.5,0,0],atol=1e-9)

    def test_multi_parent_shader_indices_with_zero_ranges(self):
        s=self.scene();n=s.edm.render_nodes[0]
        v=np.array(n.vertex_data);v[12:,3]=1;n.vertex_data=v
        n.parents=[t.ParentEntry(0,0),t.ParentEntry(1,0)]
        s=Scene(s.edm)
        self.assertEqual(len(s.meshes),2);self.assertEqual(sum(len(m.indices) for m in s.meshes),36)

if __name__=='__main__':unittest.main()
