import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edm_studio.scene import load_scene
import numpy as np
from collections import Counter
s=load_scene(sys.argv[1]);seen={};pairs=Counter()
for mi,m in enumerate(s.meshes):
    v=s.transformed(m,s._default_world)
    tri=v[m.indices.reshape(-1,3)]
    valid=np.linalg.norm(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]),axis=1)>1e-9
    for f in tri[valid]:
        key=tuple(sorted(tuple(x) for x in np.round(f,4)))
        if key in seen and seen[key]!=mi:pairs[(seen[key],mi)]+=1
        else:seen[key]=mi
for (a,b),count in pairs.most_common(15):
    print(count,a,s.meshes[a].name,s.meshes[a].material,b,s.meshes[b].name,s.meshes[b].material,flush=True)
