import io,tempfile,unittest
from pathlib import Path
import numpy as np
from PIL import Image
from edm_studio.edm import types as t
from edm_studio.edm.parser import EDMFileParser
from edm_studio.scene import Scene
from edm_studio.gltf import export_scene
from edm_studio.textures import preview_textures,TextureResolver
from edm_studio.numbering import texture_uv,bort_arguments
from tests.fixture_writer import make_edm
from tools.validate_roundtrip import GltfReader

class PbrNumberTests(unittest.TestCase):
    def scene(self,root):
        scene=Scene(EDMFileParser(io.BytesIO(make_edm())).parse(),root/'sample.edm')
        mesh=scene.meshes[0];mat=scene.materials[mesh.material]
        mat.name='test';mat.textures=[t.Texture(0,'paint'),t.Texture(2,'paint_roughmet'),t.Texture(3,'digits')]
        mat.texture_coordinates_channels=(0,0,0,1)
        mesh.uvs=[np.zeros((len(mesh.positions),2),np.float32),np.full((len(mesh.positions),2),.025,np.float32)]
        mesh.number_indices=np.zeros(len(mesh.positions),np.int32)
        mesh.extras['edm_type']='NumberNode'
        mesh.extras['edm_properties']={'number_controls':[[-1,1,32,.91]]}
        scene._build_domains()
        Image.new('RGBA',(4,4),(160,170,180,255)).save(root/'paint.png')
        Image.new('RGBA',(4,4),(17,82,193,255)).save(root/'paint_roughmet.png')
        atlas=Image.new('RGBA',(16,176),(0,0,0,0))
        for y in range(16,176):
            for x in range(3,13):atlas.putpixel((x,y),((y//16)*20,30,50,255))
        atlas.save(root/'digits.png')
        return scene

    def test_roughmet_pixels_mapping_and_live_uv(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);scene=self.scene(root);m=scene.meshes[0]
            self.assertEqual(scene.number_args,[32])
            uv=texture_uv(m,scene.materials[0],3,{32:.4})
            np.testing.assert_allclose(uv[:,0],.025,atol=1e-7)
            np.testing.assert_allclose(uv[:,1],.389,atol=1e-7)
            preview=preview_textures(scene,root,installs=[])
            self.assertTrue(preview.roughmet);self.assertTrue(preview.decals)
            file=root/'test.glb';report=export_scene(scene,file,arguments=[0,32],textures=root,baseline_args={32:.4})
            g=GltfReader(file);mat=g.doc['materials'][0]
            mr=mat['pbrMetallicRoughness'];self.assertEqual(mr['metallicFactor'],1)
            self.assertEqual(mr['roughnessFactor'],1)
            self.assertEqual(mr['metallicRoughnessTexture']['index'],mat['occlusionTexture']['index'])
            tex=g.doc['textures'][mr['metallicRoughnessTexture']['index']]
            view=g.doc['bufferViews'][g.doc['images'][tex['source']]['bufferView']]
            pixels=Image.open(io.BytesIO(g.binary[view['byteOffset']:view['byteOffset']+view['byteLength']]))
            self.assertEqual(pixels.getpixel((0,0)),(17,82,193,255))
            self.assertEqual(report['number_meshes'],1)
            children=g.doc['nodes'][m.node]['children']
            def visible(clip,time):
                world=g.evaluate(clip,time)
                return [i for i in children if np.linalg.norm(world[i][:3,:3])>1e-5]
            # Sample emitted binary animation, not the source evaluator.
            clip=next(c for c in g.doc['animations'] if c['extras']['edm_argument']==32)
            for value in [.05,.35,.85]:
                shown=visible(clip,value*3)
                self.assertEqual(len(shown),1)
                prim=g.doc['meshes'][g.doc['nodes'][shown[0]]['mesh']]['primitives'][0]
                uv=g.accessor(prim['attributes']['TEXCOORD_2'])
                np.testing.assert_allclose(uv[:,1],.025+int(value*10)/10*.91,atol=1e-7)
            original=next(c for c in g.doc['animations'] if c['extras']['edm_argument']==0)
            shown=visible(original,1.5);self.assertEqual(len(shown),1)
            self.assertIn('32=0.4',g.doc['nodes'][shown[0]]['name'])

    def test_explicit_roughmet_override_never_falls_back_to_specular(self):
        from edm_studio.liveries import read_livery
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);scene=self.scene(root)
            p=root/'description.lua';p.write_text('livery={{"test",13,"missing",false}}')
            resolver=TextureResolver(scene.source,root,read_livery(p),[])
            self.assertIsNone(resolver.material(scene.materials[0],13));self.assertTrue(resolver.missing)

    def test_number_time_precision_and_missing_texture_clip(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);scene=self.scene(root);p=root/'close-times.glb'
            export_scene(scene,p,arguments=[32],textures=root,baseline_args={32:.40000000001})
            g=GltfReader(p)
            for sampler in g.doc['animations'][0]['samplers']:
                self.assertTrue((np.diff(g.accessor(sampler['input'])[:,0])>0).all())
            report=export_scene(scene,p,arguments=[32],embed_textures=False)
            self.assertEqual(report['skipped_arguments'],[32]);self.assertNotIn('animations',GltfReader(p).doc)

if __name__=='__main__':unittest.main()
