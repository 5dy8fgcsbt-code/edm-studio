"""Development oracle: compare the independent C++ port with the established reader."""
import argparse,json,time,subprocess
from pathlib import Path
import numpy as np
from edm_studio.scene import load_scene

def main():
    ap=argparse.ArgumentParser();ap.add_argument('model');ap.add_argument('--prefix',required=True);ap.add_argument('--baseline',default='');a=ap.parse_args()
    prefix=Path(a.prefix);prefix.parent.mkdir(parents=True,exist_ok=True)
    cmd=['build/native/Release/edm-native-cli.exe','inspect',a.model,'--output',str(prefix)+'.json','--dump-world',str(prefix)+'-world.json']
    if a.baseline:cmd+=['--baseline',a.baseline]
    with open(str(prefix)+'.log','w',encoding='utf-8') as log:subprocess.run(cmd,check=True,stdout=log,stderr=log)
    start=time.perf_counter();scene=load_scene(a.model);elapsed=time.perf_counter()-start
    args={int(k):float(v) for k,v in (part.split('=') for part in a.baseline.split(',') if part)}
    world=scene.evaluate(args);native=json.loads(Path(str(prefix)+'-world.json').read_text(encoding='utf-8'))
    counts=json.loads(Path(str(prefix)+'.json').read_text(encoding='utf-8'));original=scene.summary()
    for field in ['scene_nodes','export_meshes','triangles','skinned_meshes','materials','arguments','number_arguments','winding_normalized_meshes','connectors','collision_nodes']:
        assert counts[field]==original[field],(field,counts[field],original[field])
    heads={n['extras']['edm_node']:n for n in native['nodes'] if 'edm_node' in n['extras'] and not n['name'].endswith('/ skin visibility')}
    matrix_error=0
    for i,head in enumerate(scene.heads):
        difference=float(np.max(np.abs(np.array(heads[i]['matrix']).reshape(4,4).T-world[head])))
        matrix_error=max(matrix_error,difference)
    vertex_error=0;sample_count=0
    for mesh,record in zip(scene.meshes,native['meshes']):
        assert mesh.name==record['name'],(mesh.name,record['name'])
        assert len(mesh.positions)==record['vertex_count'] and len(mesh.indices)==record['indices']
        positions=scene.transformed(mesh,world);sample=positions[::max(1,len(positions)//16)]
        delta=float(np.max(np.abs(sample-np.asarray(record['samples']))));vertex_error=max(vertex_error,delta);sample_count+=len(sample)
    report={'model':a.model,'baseline':args,'python_scene_seconds':elapsed,'native_scene_seconds':counts['parse_seconds']+counts['scene_build_seconds'],'source_matrix_comparisons':len(heads),'vertex_samples':sample_count,'max_matrix_error':matrix_error,'max_vertex_error_metres':vertex_error,'counts_match':True}
    Path(str(prefix)+'-comparison.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report,indent=2))
    assert matrix_error<2e-5 and vertex_error<1e-4,report
if __name__=='__main__':main()
