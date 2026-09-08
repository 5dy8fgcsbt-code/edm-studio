"""Generate small original EDM regression assets; never copies DCS content."""
from pathlib import Path
import struct,math
import numpy as np
from tests.fixture_writer import make_edm

root=Path('native/tests/fixtures');root.mkdir(parents=True,exist_ok=True)
for version in (8,10):
    (root/f'animation-v{version}.edm').write_bytes(make_edm(version=version))
(root/'visibility-v1.edm').write_bytes(make_edm(visibility_version=1))
(root/'visibility-unknown.edm').write_bytes(make_edm(visibility_version=2))
(root/'spots-all-layouts.edm').write_bytes(make_edm(spot_layouts=(None,1,2)))
(root/'spots-unknown.edm').write_bytes(make_edm(spot_layouts=(3,)))

def u(v):return struct.pack('<I',v)
def f(*v):return struct.pack('<'+'f'*len(v),*v)
def d(*v):return struct.pack('<'+'d'*len(v),*v)
def string(s):
    b=s.encode('cp1251');return u(len(b))+b
def base(name):return string(name)+u(0)+u(0)
identity=d(*np.eye(4).ravel())
def container(channels,nodes,parents,renders,textures=()):
    out=b'EDM'+struct.pack('<H',8)+u(0)+u(0)+string('model::RootNode')+base('Native original fixture')+b'\0'+d(-1,-1,-1,1,1,1,*([0]*12))
    out+=u(1)+u(4)+string('VERTEX_FORMAT')+u(len(channels))+bytes(channels)+string('MATERIAL_NAME')+string('def_material')+string('NAME')+string('test')+string('TEXTURES')+u(len(textures))
    for slot,name in textures:out+=u(slot)+struct.pack('<i',-1)+string(name)+u(2)+u(2)+u(10)+u(6)+f(*np.eye(4).ravel())
    out+=u(0)+u(0)+u(len(nodes))+b''.join(nodes)+struct.pack('<'+'i'*len(parents),*parents)
    return out+u(1)+string('RENDER_NODES')+u(len(renders))+b''.join(renders)
def geometry(vertices,indices):
    v=np.array(vertices,dtype='<f4');return u(len(v))+u(v.shape[1])+v.tobytes()+b'\1'+u(len(indices))+u(5)+struct.pack('<'+'H'*len(indices),*indices)
def animated(name,bone=False,position=False,scaled=False):
    out=string('model::ArgAnimatedBone' if bone else 'model::ArgAnimationNode')+base(name)+identity+d(0,0,0)+d(0,0,0,1)+d(math.sin(.2),0,0,math.cos(.2))+d(*( [2,3,4] if scaled else [1,1,1]))
    out+=u(int(position))
    if position:out+=u(3)+u(2)+d(0,0,0,0)+d(1,2,0,0)
    out+=u(0)+u(0)
    if bone:out+=identity
    return out
plain=string('model::Node')+base('Root')
bone=string('model::Bone')+base('Base')+identity+identity
verts=np.array([[0,0,0,0,0,0,1,.25,0,0,0],[1,0,0,0,0,0,1,.25,0,0,0],[0,1,0,0,0,0,1,.25,0,0,0]],dtype='<f4')
verts.view('<u4')[:,3]=0x7fc00000  # NaN payload: only the first packed joint has weight.
render=string('model::SkinNode')+base('Weighted')+u(0)+u(0)+u(2)+u(1)+u(2)+u(0)+geometry(verts,[0,1,2])
(root/'skin-nan-packed.edm').write_bytes(container([4,3]+[0]*19+[4],[plain,bone,animated('Joint',True,True)],[-1,0,0],[render]))
mirror=string('model::TransformNode')+base('Mirrored')+d(*np.diag([-1.,1.,1.,1.]).ravel())
verts=[[0,0,0,0,0,0,1],[1,0,0,0,0,0,1],[0,1,0,0,0,0,1]]
render=string('model::RenderNode')+base('Panel')+u(0)+u(0)+u(1)+u(0)+struct.pack('<i',-1)+geometry(verts,[0,2,1])
(root/'mirrored.edm').write_bytes(container([4,3],[mirror],[-1],[render]))
(root/'scale-basis.edm').write_bytes(container([4,3],[animated('Scaled',scaled=True)],[-1],[render]))
verts=[[0,0,0,0,0,0,1,0,0,0],[1,0,0,0,0,0,1,0,0,0],[0,1,0,0,0,0,1,0,0,0]]
render=string('model::NumberNode')+base('Number')+u(0)+u(0)+u(1)+u(0)+struct.pack('<i',-1)+geometry(verts,[0,1,2])+u(1)+struct.pack('<ifIf',-1,1,32,.91)
(root/'number.edm').write_bytes(container([4,3,0,0,2]+[0]*16+[1],[plain],[-1],[render],[(0,'paint'),(13,'paint_RoughMet'),(3,'digits')]))
verts=[[0,0,0,0,0,0,1],[1,0,0,0,0,0,1],[0,1,0,0,0,0,1],[0,0,0,1,0,0,1],[1,0,0,1,0,0,1],[0,1,0,1,0,0,1]]
render=string('model::RenderNode')+base('Groups')+u(0)+u(0)+u(2)+u(0)+struct.pack('<ii',0,-1)+u(1)+struct.pack('<ii',0,-1)+geometry(verts,[0,1,2,3,4,5])
(root/'multi-parent.edm').write_bytes(container([4,3],[plain,animated('Moving',position=True)],[-1,0],[render]))
print('Generated original native fixtures:',len(list(root.glob('*.edm'))))
