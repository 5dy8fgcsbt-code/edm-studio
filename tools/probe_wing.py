"""Read-only inspection of F-14 right wing render parts and visibility chains."""
import sys,json,argparse
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edm_studio.scene import load_scene,jsonable
p=argparse.ArgumentParser(description=__doc__);p.add_argument('model');p.add_argument('--output',default='validation/f14-wing-probe.json')
opts=p.parse_args();s=load_scene(opts.model)
rows=[]
print('root',s.edm.root.props,'extra buckets',s.edm.extra_render_items.keys())
for i,m in enumerate(s.meshes):
    v=s.transformed(m,s._default_world);lo=v.min(0);hi=v.max(0)
    if hi[2]<2.5 or lo[2]>11 or hi[0]<-6 or lo[0]>4 or np.linalg.norm(hi-lo)<.01:continue
    mat=s.materials[m.material];ri=m.extras['edm_render_index'];n=s.edm.render_nodes[ri]
    chain=[];p=m.extras['edm_parent'] if m.joints is None else n.bones[0]
    while p>=0:
        node=s.edm.nodes[p]
        chain.append({'index':p,'name':node.name,'type':node.type,'props':jsonable(node.props),'vis':getattr(node,'vis_data',None)})
        p=node.parent_idx
    row={'mesh':i,'name':m.name,'material':mat.name,'shader':mat.material_name,'material_props':jsonable(mat.uniforms),
         'box':[lo.tolist(),hi.tolist()],'type':n.type,'render_props':jsonable(n.props),
         'damage':m.extras.get('edm_damage_argument'),'chain':chain,'bones':getattr(n,'bones',None)}
    rows.append(row)
    print(i,m.name,mat.name,n.type,'box',np.round([lo,hi],2).tolist(), 'vis',[(r['index'],r['vis']) for r in chain if r['vis']])
output=Path(opts.output);output.parent.mkdir(parents=True,exist_ok=True)
output.write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf8')
