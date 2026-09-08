import sys,json
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edm_studio.scene import load_scene,jsonable
import pefile
p=pefile.PE(r'D:\SteamLibrary\steamapps\common\DCSWorld\bin\UniModelDesc.dll')
for s in p.DIRECTORY_ENTRY_EXPORT.symbols:
    if s.name and any(x in s.name for x in (b'NumberNode',b'TailNumber',b'NumberDesc')):print(hex(s.address),s.name.decode())
for f in ['FA-18C/Shapes/fa-18c_lod2.edm','F14/Shapes/f-14b.edm']:
    s=load_scene(Path(r'D:\SteamLibrary\steamapps\common\DCSWorld\CoreMods\aircraft')/f)
    out=[]
    for n in s.edm.render_nodes:
        if n.type!='NumberNode':continue
        m=s.materials[n.material_id];fmt=m.vertex_format
        raw=np.array(n.vertex_data).reshape(-1,fmt.stride)
        out.append({'name':n.name,'material':jsonable(m),'props':n.props,'parents':jsonable(n.parents),'format':jsonable(fmt),
                    'vertices':raw[:12].tolist(),'channel21':np.unique(raw[:,fmt.offset_of(21)]).tolist() if fmt.size_of(21) else None})
    path=Path('validation')/(s.source.stem+'-numbers.json');path.write_text(json.dumps(out,indent=2),encoding='utf8')
    print(f,'number nodes',len(out),'sample',out[0] if out else None)
    print('texture indices',sorted(set(t.index for m in s.materials for t in m.textures)))
    for m in s.materials:
        if m.name.lower() in ['hb_f14_ext_01','f18c1']:
            print(jsonable(m))
