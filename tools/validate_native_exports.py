"""Independent native GLB verification in the established Python math oracle."""
import argparse,json
from pathlib import Path
import numpy as np
from edm_studio.scene import load_scene
from tools.validate_roundtrip import GltfReader

ap=argparse.ArgumentParser();ap.add_argument('model');ap.add_argument('output');a=ap.parse_args()
scene=load_scene(a.model);g=GltfReader(a.output)
heads={n['extras']['edm_node']:i for i,n in enumerate(g.doc['nodes']) if 'edm_node' in n.get('extras',{}) and not n.get('name','').endswith('/ skin visibility')}
mesh_nodes={(n.get('name',''),n.get('extras',{}).get('edm_render_index'),n.get('extras',{}).get('edm_parent')):i for i,n in enumerate(g.doc['nodes']) if 'mesh' in n}
results=[];comparisons=0;vertex_comparisons=0
for clip in g.doc.get('animations',[]):
    arg=clip['extras']['edm_argument'];lo=clip['extras']['argument_min'];hi=clip['extras']['argument_max'];duration=clip['extras']['duration_seconds']
    for phase in [.137,.5,.873]:
        baseline=clip['extras'].get('other_arguments',{})
        baseline={int(k):v for k,v in baseline.items()} if isinstance(baseline,dict) else {}
        expected=scene.evaluate({**baseline,arg:lo+(hi-lo)*phase});actual=g.evaluate(clip,phase*duration)
        error=max(float(np.max(np.abs(actual[heads[i]]-expected[h]))) for i,h in enumerate(scene.heads));comparisons+=len(heads)
        vertex_error=0
        for mesh in scene.meshes:
            key=(mesh.name,mesh.extras.get('edm_render_index'),mesh.extras.get('edm_parent'))
            if key not in mesh_nodes:continue
            ni=mesh_nodes[key];node=g.doc['nodes'][ni];primitive=g.doc['meshes'][node['mesh']]['primitives'][0];attrs=primitive['attributes'];positions=g.accessor(attrs['POSITION'])
            ids=np.arange(0,len(positions),max(1,len(positions)//8));p=np.c_[positions[ids],np.ones(len(ids))]
            if 'skin' in node:
                skin=g.doc['skins'][node['skin']];ibm=g.accessor(skin['inverseBindMatrices']).reshape(-1,4,4).transpose(0,2,1);palette=actual[skin['joints']]@ibm;got=np.zeros((len(ids),3))
                for group in (0,1):
                    if f'JOINTS_{group}' not in attrs:continue
                    joints=g.accessor(attrs[f'JOINTS_{group}'])[ids];weights=g.accessor(attrs[f'WEIGHTS_{group}'])[ids]
                    for k in range(4):got+=np.einsum('nij,nj->ni',palette[joints[:,k]],p)[:,:3]*weights[:,k,None]
            else:got=(p@actual[ni].T)[:,:3]
            wanted=scene.transformed(mesh,expected)[ids];vertex_error=max(vertex_error,float(np.max(np.abs(got-wanted))));vertex_comparisons+=len(ids)
        results.append({'argument':arg,'phase':phase,'matrix_error':error,'vertex_error_metres':vertex_error})
report={'output':a.output,'source_matrix_comparisons':comparisons,'vertex_samples':vertex_comparisons,'max_matrix_error':max((r['matrix_error'] for r in results),default=0),'max_vertex_error_metres':max((r['vertex_error_metres'] for r in results),default=0),'results':results}
report['passed']=bool(results) and report['max_matrix_error']<5e-4 and report['max_vertex_error_metres']<5e-4
Path(a.output+'.native-roundtrip.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in report.items() if k!='results'},indent=2),flush=True);assert report['passed']


