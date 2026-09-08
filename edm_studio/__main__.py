import argparse,json
from .scene import load_scene
from .gltf import export_scene

def main():
    parser=argparse.ArgumentParser(description='EDM Studio: local EDM to animated GLB/glTF')
    parser.add_argument('input',nargs='?');parser.add_argument('-o','--output')
    parser.add_argument('--args',nargs='*',type=int,help='Arguments to export; omitted means all; empty means static')
    parser.add_argument('--duration',type=float,default=3.)
    parser.add_argument('--textures');parser.add_argument('--no-textures',action='store_true');parser.add_argument('--inspect',action='store_true')
    parser.add_argument('--livery',help='Livery directory, description.lua, or ZIP (optionally ZIP::entry)')
    parser.add_argument('--list-liveries',action='store_true',help='Discover installed and Saved Games liveries for this model')
    parser.add_argument('--bort',help='Aircraft number for recognized DCS digit handles, e.g. 408')
    parser.add_argument('--lua-context',default='{}',help='JSON object of globals for dynamic livery Lua')
    opts=parser.parse_args()
    if not opts.input:
        from .app import main as ui
        return ui()
    scene=load_scene(opts.input)
    if opts.list_liveries:
        from .liveries import discover_liveries
        catalog=discover_liveries(opts.input)
        print(json.dumps({'liveries':[l.metadata() for l in catalog.liveries],'roots':catalog.roots,'warnings':catalog.warnings},ensure_ascii=False,indent=2))
    elif opts.inspect:print(json.dumps(scene.summary(),ensure_ascii=False,indent=2))
    elif opts.output:
        from .liveries import read_livery
        from .numbering import bort_arguments
        context=json.loads(opts.lua_context)
        if not isinstance(context,dict):parser.error('--lua-context must be a JSON object')
        selected=read_livery(opts.livery,context=context) if opts.livery else None
        baseline=bort_arguments(scene,opts.bort) if opts.bort else None
        report=export_scene(scene,opts.output,arguments=opts.args,duration=opts.duration,textures=opts.textures,embed_textures=not opts.no_textures,livery=selected,baseline_args=baseline)
        print(json.dumps({'output':opts.output,'animations':len(report['exported_arguments']),'triangles':report['triangles']}))
    else:parser.error('Use --inspect or --output')

if __name__=='__main__':raise SystemExit(main())
