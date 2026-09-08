import sys, struct, json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edm_studio.edm.parser import EDMFileParser
class Inspect(EDMFileParser):
    def _number_node(self):
        p=self.r.tell()
        print('NUMBER offset',p)
        name,version,props=self._read_base_node()
        start=self.r.tell()
        b=self.r.read_raw(128)
        print('header',repr(name),version,props,'body',start, 'u32',struct.unpack('<32I',b))
        self.r.f.seek(p)
        try:
            n=self._render_node()
            print('as render',n.name,n.material_id,len(n.vertex_data),len(n.index_data),'end',self.r.tell())
            b=self.r.read_raw(64)
            print('AFTER',struct.unpack('<16I',b), [self.r.string_table[x] if x<len(self.r.string_table) else '' for x in struct.unpack('<16I',b)])
        except Exception as e: print('as render ERROR',e)
        raise RuntimeError('probe stop')
    def _read_render_items(self):
        count=self.r.uint()
        for _ in range(count):
            cat=self.r.string(); count2=self.r.uint()
            for _ in range(count2):self._read_named_type()
        return {}
try:
    with open(sys.argv[1],'rb') as f:Inspect(f).parse()
except RuntimeError: pass
