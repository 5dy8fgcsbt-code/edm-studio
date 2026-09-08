"""Reopen emitted glTF buffers and evaluate their animation graph independently."""
import sys,json,struct,base64
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edm_studio.math3d import translation,rotation,scaling,slerp
from edm_studio.scene import load_scene

class GltfReader:
    def __init__(self,path):
        raw=Path(path).read_bytes()
        if raw[:4]==b'glTF':
            magic,version,length=struct.unpack_from('<III',raw)
            assert version==2 and length==len(raw)
            n=struct.unpack_from('<I',raw,12)[0];self.doc=json.loads(raw[20:20+n])
            count,tag=struct.unpack_from('<II',raw,20+n);assert tag==0x004e4942
            self.binary=raw[28+n:28+n+count]
        else:
            self.doc=json.loads(raw);self.binary=base64.b64decode(self.doc['buffers'][0]['uri'].split(',')[1])
        self.cache={};self.parents=[-1]*len(self.doc['nodes']);self.order=[]
        for i,n in enumerate(self.doc['nodes']):
            for j in n.get('children',[]):assert self.parents[j]==-1;self.parents[j]=i
        pending=[i for i,p in enumerate(self.parents) if p==-1]
        while pending:
            i=pending.pop();self.order.append(i);pending.extend(self.doc['nodes'][i].get('children',[]))
        assert len(self.order)==len(self.parents)

    def accessor(self,index):
        if index in self.cache:return self.cache[index]
        a=self.doc['accessors'][index];v=self.doc['bufferViews'][a['bufferView']]
        dtype={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1'}[a['componentType']]
        dims={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
        arr=np.frombuffer(self.binary,dtype=dtype,count=a['count']*dims,offset=v.get('byteOffset',0)+a.get('byteOffset',0)).reshape(-1,dims)
        self.cache[index]=arr;return arr

    def evaluate(self,clip=None,time=0.):
        overrides={}
        if clip:
            for c in clip['channels']:
                sampler=clip['samplers'][c['sampler']];times=self.accessor(sampler['input'])[:,0];values=self.accessor(sampler['output'])
                i=int(np.searchsorted(times,time,side='right'))
                a=max(0,i-1);b=min(len(times)-1,i)
                t=0. if a==b else (time-float(times[a]))/float(times[b]-times[a])
                if sampler.get('interpolation')=='STEP':value=values[a]
                elif c['target']['path']=='rotation':value=slerp(values[a],values[b],t)
                else:value=values[a]*(1-t)+values[b]*t
                overrides.setdefault(c['target']['node'],{})[c['target']['path']]=value
        world=[]
        for i,n in enumerate(self.doc['nodes']):
            n={**n,**overrides.get(i,{})}
            if 'matrix' in n:m=np.array(n['matrix']).reshape(4,4).T
            else:m=translation(n.get('translation',[0,0,0]))@rotation(n.get('rotation',[0,0,0,1]))@scaling(n.get('scale',[1,1,1]))
            world.append(m)
        for i in self.order:
            if self.parents[i]>=0:world[i]=world[self.parents[i]]@world[i]
        return np.array(world)

def validate(source,output):
    scene=load_scene(source);g=GltfReader(output);results=[]
    for clip in g.doc.get('animations',[]):
        arg=clip['extras']['edm_argument'];lo=clip['extras']['argument_min'];hi=clip['extras']['argument_max'];duration=clip['extras']['duration_seconds']
        if arg not in [0,1,2,3,5,9,10,11,12,17,38,50]:continue
        for phase in [.137,.5,.873]:
            val=lo+(hi-lo)*phase;actual=g.evaluate(clip,phase*duration)
            baseline=clip['extras'].get('other_arguments',{})
            baseline={int(k):v for k,v in baseline.items()} if isinstance(baseline,dict) else {}
            expected=scene.evaluate({**baseline,arg:val})
            # Number atlas states are extra children appended after the original
            # transform graph; their discrete UV/visibility is tested separately.
            error=float(np.max(np.abs(actual[:len(expected)]-expected)))
            results.append({'argument':arg,'phase':phase,'max_matrix_error':error})
    maxerr=max(r['max_matrix_error'] for r in results)
    report={'source':source,'output':output,'comparisons':len(results)*len(scene.nodes),'max_matrix_error':maxerr,'passed':maxerr<5e-4,'results':results}
    Path(output+'.roundtrip.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='results'}),flush=True)
    assert report['passed']

if __name__=='__main__':validate(sys.argv[1],sys.argv[2])
