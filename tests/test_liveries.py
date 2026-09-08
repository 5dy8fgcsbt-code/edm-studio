import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
import numpy as np
from PIL import Image
from edm_studio.lua_data import read_data,LuaDataError
from edm_studio.liveries import read_livery,discover_liveries
from edm_studio.textures import TextureResolver,preview_textures
from edm_studio.edm import types as t
from edm_studio.edm.parser import EDMFileParser
from edm_studio.scene import Scene
from edm_studio.gltf import export_scene
from tests.fixture_writer import make_edm
from tools.validate_roundtrip import GltfReader


def png(color):
    stream=io.BytesIO();Image.new('RGBA',(4,4),color).save(stream,format='PNG');return stream.getvalue()


class LiveryTests(unittest.TestCase):
    def test_lua_literals_comments_constants_and_local_variables(self):
        env,warnings=read_data('''--[=[ livery = { broken } ]=]
local prefix = "custom_"
livery = {
 {"body", DIFFUSE, prefix .. "paint", false};
 {"body", ROUGHNESS_METALLIC, [=[body_rough]=], true},
 {"numbers", DECAL, "empty", true},
}
name = _("中文涂装\\nTest")
custom_args = { [38] = 1; [50] = -0.25; }
countries = {"USA", "CHN"}
livery[1][3] = "final"
''')
        self.assertEqual(env['livery'][1][3],'final');self.assertEqual(env['livery'][2][2],13)
        self.assertEqual(env['custom_args'][50],-.25);self.assertEqual(env['name'],'中文涂装\nTest');self.assertFalse(warnings)

    def test_no_execution_and_no_conditional_misinterpretation(self):
        env,warnings=read_data('os.execute("do not execute")\nlivery={{"body",0,"ok",false}}\nif false then\nlivery={{"body",0,"wrong",false}}\nend')
        self.assertEqual(env['livery'][1][3],'ok');self.assertGreaterEqual(len(warnings),2)
        for source in ['livery={ {"body",0, require("x"),false}}','livery={"unfinished"','livery='+('{'*45)+'1'+('}'*45)]:
            with self.assertRaises(LuaDataError):read_data(source)

    def test_discover_moved_saved_games_profiles_and_installed_zip(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);install=root/'game';(install/'Bazar').mkdir(parents=True)
            model=install/'CoreMods'/'aircraft'/'FA-18C'/'Shapes'/'fa-18c_lod2.edm';model.parent.mkdir(parents=True);model.write_bytes(b'EDM')
            save=root/'Moved Saved Games';skin=save/'DCS.openbeta'/'Liveries'/'FA-18C_hornet'/'Local';skin.mkdir(parents=True)
            desc=b'livery={{"f18c1",0,"paint",false}}; name="User skin"'
            (skin/'description.lua').write_bytes(desc)
            official=install/'CoreMods'/'aircraft'/'FA-18C'/'Liveries'/'FA-18C_hornet';official.mkdir(parents=True)
            with zipfile.ZipFile(official/'Pack.zip','w') as z:z.writestr('Skin/description.lua',desc.replace(b'User skin',b'Installed'))
            wrong=save/'DCS.openbeta'/'Liveries'/'f-14b'/'Wrong';wrong.mkdir(parents=True);(wrong/'description.lua').write_bytes(desc)
            c=discover_liveries(model,saved_roots=[save],installs=[install])
            self.assertEqual([l.name for l in c.liveries],['User skin','Installed'])
            self.assertEqual(c.liveries[1].entry,'Skin/description.lua');self.assertFalse(c.warnings)

    def test_local_and_common_namespaces_zip_relative_and_missing(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);model=root/'Shapes'/'model.edm';model.parent.mkdir();model.touch()
            common=root/'Textures'/'nested';common.mkdir(parents=True);(common/'paint.v1.png').write_bytes(png('red'))
            skin=root/'Liveries'/'skin.zip';skin.parent.mkdir()
            with zipfile.ZipFile(skin,'w') as z:
                z.writestr('Skin/description.lua','livery={{"local",0,"paint.v1",false},{"common",0,"paint.v1",true},{"missing",0,"missing",false},{"relative",0,"../Shared/test",false}}')
                z.writestr('Skin/PAINT.v1.PNG',png('blue'));z.writestr('Shared/test.png',png('green'))
            l=read_livery(skin);r=TextureResolver(model,livery=l,installs=[])
            colors=[]
            for name in ('local','common','relative'):
                material=t.Material(name=name);colors.append(r.material(material).image().getpixel((0,0)))
            self.assertEqual(colors,[Image.new('RGBA',(1,1),color).getpixel((0,0)) for color in ('blue','red','green')])
            self.assertIsNone(r.material(t.Material(name='missing')))
            self.assertIsNone(r.livery_local('../../../outside.png'));self.assertIsNone(r.livery_local('C:/private.png'))
            self.assertEqual(len(r.missing),1)

    def test_livery_export_uses_same_pixels_and_custom_argument_baseline(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);s=Scene(EDMFileParser(io.BytesIO(make_edm())).parse());s.source=root/'demo.edm'
            skin=root/'skin';skin.mkdir();(skin/'paint.png').write_bytes(png('blue'))
            (skin/'description.lua').write_text('livery={{"Demo metal",0,"paint",false}}; name="Blue"; custom_args={[0]=0.25,[40]=0.25}',encoding='utf-8')
            l=read_livery(skin);preview=preview_textures(s,livery=l,installs=[])
            self.assertEqual(len(preview.materials),1)
            for arguments in ([],[40]):
                path=root/('animated.glb' if arguments else 'static.glb');report=export_scene(s,path,livery=l,arguments=arguments)
                g=GltfReader(path);self.assertEqual(report['livery']['name'],'Blue');self.assertEqual(report['embedded_diffuse_materials'],1)
                view=g.doc['bufferViews'][g.doc['images'][0]['bufferView']]
                # Check the actual embedded image via a standalone GLB chunk read.
                raw=path.read_bytes();json_size=int.from_bytes(raw[12:16],'little');start=28+json_size+view.get('byteOffset',0)
                embedded=Image.open(io.BytesIO(raw[start:start+view['byteLength']])).convert('RGBA')
                self.assertEqual(embedded.tobytes(),next(iter(preview.images.values()))[2])
                if arguments:
                    result=g.evaluate(g.doc['animations'][0],.6)
                    expected=s.evaluate({0:.25,40:.6/3})
                    np.testing.assert_allclose(result,expected,atol=1e-6)
                else:
                    for tr in s.tracks:
                        np.testing.assert_allclose(g.doc['nodes'][tr.node][tr.path],tr.evaluate(l.custom_args[tr.argument]),atol=1e-6)


if __name__=='__main__':unittest.main()
