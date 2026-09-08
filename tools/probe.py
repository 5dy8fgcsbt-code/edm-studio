"""Read-only compatibility probe; never modifies source assets."""
import sys, json, time, collections
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from edm_studio.edm.parser import EDMFileParser

for path in sys.argv[1:]:
    start = time.perf_counter()
    try:
        with open(path, 'rb') as f:
            p = EDMFileParser(f)
            s = p.parse()
            info = dict(file=path, version=s.version, remaining=p.r.remaining(),
                nodes=len(s.nodes), types=dict(collections.Counter(n.type for n in s.nodes)),
                render=dict(collections.Counter(n.type for n in s.render_nodes)),
                materials=len(s.materials), triangles=sum(len(getattr(n,'index_data',[]))//3 for n in s.render_nodes),
                animation=[])
            for i,n in enumerate(s.nodes):
                if hasattr(n, 'base'):
                    info['animation'].append(dict(i=i,name=n.name,parent=n.parent_idx,base=vars(n.base),pos=n.pos_data,rot=n.rot_data,scale=n.scale_data))
            out=Path(__file__).resolve().parents[1]/'validation'/(Path(path).stem+'.probe.json')
            out.write_text(json.dumps(info,default=lambda x:vars(x),ensure_ascii=False,indent=2),encoding='utf-8')
            del info['animation']
            print(json.dumps(info,ensure_ascii=False), 'seconds',round(time.perf_counter()-start,2),flush=True)
    except Exception as e:
        import traceback
        print(path, type(e).__name__, str(e),flush=True)
        traceback.print_exc(limit=4)
