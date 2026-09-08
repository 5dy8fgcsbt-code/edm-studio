import tempfile,unittest,zipfile
from pathlib import Path
from edm_studio.lua_runtime import evaluate_config
from edm_studio.lua_data import LuaDataError
from edm_studio.liveries import read_livery

class DynamicLuaTests(unittest.TestCase):
    def test_functions_loops_context_and_includes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'skin';root.mkdir()
            (root/'shared.lua').write_text('return {prefix="body"}')
            text='''local shared=require('shared')
            livery={}
            local function texture(i) return string.format('%s_%02d',shared.prefix,i) end
            for i=1,3 do table.insert(livery, {texture(i),ROUGHNESS_METALLIC,texture(i)..'_RM',false}) end
            if country=='USA' then name=_('US skin') else name='Other' end
            custom_args={[1001]=1/2}
            '''
            p=root/'description.lua';p.write_text(text)
            l=read_livery(p,context={'country':'USA'})
            self.assertEqual(l.name,'US skin');self.assertEqual(l.custom_args,{1001:.5})
            self.assertEqual(l.override('body_03',13).name,'body_03_RM')
            self.assertEqual(l.evaluation['includes'],['shared.lua'])
            z=Path(tmp)/'pack.zip'
            with zipfile.ZipFile(z,'w') as a:
                a.writestr('skin/description.lua',"dofile('../shared.lua')\nname='zip'")
                a.writestr('shared.lua',"livery={{'body',0,'diff',false}}")
            self.assertEqual(read_livery(z).name,'zip')

    def test_failures_do_not_return_partial_livery(self):
        prefix="livery={{'a',0,'b',false}}\n"
        for tail in ['while true do end','os.execute("echo unsafe")','python.eval("1+1")',
                     'require("socket")','debug.sethook()',"livery[1]=string.rep('x',64000000)",
                     'if true then livery[1][3]=undefined() end']:
            with self.subTest(tail=tail),self.assertRaises(LuaDataError):evaluate_config(prefix+tail)
        env,_,meta=evaluate_config("livery={}\nif false then os.execute('unused') end")
        self.assertEqual(env['livery'],{});self.assertEqual(meta['engine'],'Lua 5.4')
        env,_,_=evaluate_config("local name='private local'\nlivery={}")
        self.assertNotIn('name',env)
        with self.assertRaisesRegex(LuaDataError,'timed out'):
            evaluate_config('while true do pcall(function() while true do end end) end',timeout=.6)

    def test_include_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'unit'/'skin';root.mkdir(parents=True)
            (Path(tmp)/'outside.lua').write_text('livery={}')
            p=root/'description.lua';p.write_text("dofile('../../outside.lua')")
            with self.assertRaisesRegex(LuaDataError,'escapes'):read_livery(p)

if __name__=='__main__':unittest.main()
