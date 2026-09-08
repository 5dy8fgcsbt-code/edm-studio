"""DCS NumberNode UV controls, shared by GPU preview and portable export."""
import numpy as np

def controls(mesh):return mesh.extras.get('edm_properties',{}).get('number_controls',[])

def number_arguments(mesh):
    return sorted({int(c[i]) for c in controls(mesh) for i in (0,2) if int(c[i]) not in (-1,0xffffffff)})

def uv_channel(material,slot):
    slots=material.texture_coordinates_channels
    if slot==13:slot=2 if len(slots)<=13 or slots[13]==0xffffffff else 13
    channel=slots[slot] if slot<len(slots) else 0
    return int(channel) if channel!=0xffffffff else 0

def texture_uv(mesh,material,slot,args=None):
    channel=uv_channel(material,slot)
    uv=np.array(mesh.uvs[channel] if channel<len(mesh.uvs) else mesh.uvs[0] if mesh.uvs else
                np.zeros((len(mesh.positions),2)),dtype=np.float32,copy=True)
    ref=(material.texture_by_index(13) or material.texture_by_index(2)) if slot==13 else material.texture_by_index(slot)
    if ref and len(ref.matrix)==16:
        m=np.array(ref.matrix).reshape(4,4)
        uv=uv@m[:2,:2]+m[3,:2]
    shift=material.uniforms.get('decalShift' if slot==3 else 'diffuseShift')
    if isinstance(shift,(tuple,list)) and len(shift)>=2:uv+=shift[:2]
    if slot==3 and mesh.number_indices is not None:
        values=[]
        for arg_u,scale_u,arg_v,scale_v in controls(mesh):
            values.append([(args or {}).get(int(arg_u),0.)*scale_u if int(arg_u) not in (-1,0xffffffff) else 0.,
                           (args or {}).get(int(arg_v),0.)*scale_v if int(arg_v) not in (-1,0xffffffff) else 0.])
        uv+=np.asarray(values,dtype=np.float32)[mesh.number_indices]
    return np.ascontiguousarray(uv,dtype=np.float32)

def bort_mapping(scene):
    available=set(scene.number_args)
    for order in ([442,31,32],[443,444,445],[30,31,32],[31,32]):
        if set(order)<=available:return list(order)
    return []

def bort_arguments(scene,text):
    order=bort_mapping(scene)
    if not order:raise ValueError('模型没有已识别的快捷编号布局，请使用编号参数调节。')
    if not text.isascii() or not text.isdigit() or len(text)>len(order):raise ValueError(f'请输入最多 {len(order)} 位数字')
    return dict(zip(order,[int(c)/10. for c in text.zfill(len(order))]))
