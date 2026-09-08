"""Small original EDM fixtures. No DCS game assets are redistributed."""
import struct,math
import numpy as np

def make_edm(version=10,number=False,visibility_version=0,visibility_matrix=None,spot_layouts=()):
    names=['model::RootNode','model::Node','model::ArgAnimationNode','model::ArgVisibilityNode','model::RenderNode','model::NumberNode',
           'VERTEX_FORMAT','MATERIAL_NAME','NAME','TEXTURES','def_material','Demo metal','RENDER_NODES',
           'model::Property<unsigned int>','__VERSION__','model::FakeSpotLightsNode']
    def u(x):return struct.pack('<I',x)
    def d(*v):return struct.pack('<'+'d'*len(v),*v)
    def raw(s):
        b=s.encode('cp1251');return u(len(b))+b
    def string(s):return u(names.index(s)) if version==10 else raw(s)
    def header(name):return raw(name)+u(0)+u(0)
    identity=tuple(np.eye(4).ravel())
    f=b'EDM'+struct.pack('<H',version)
    if version==10:
        table=b''.join(s.encode()+b'\0' for s in names);f+=u(len(table))+table
    f+=u(0)+u(0)
    f+=string('model::RootNode')+header('Animation fixture')+(b'\0' if version==8 else b'')
    f+=d(-1,-1,-1,1,1,1,*([0]*12))
    channels=[4,3,0,0,2]
    f+=u(1)+u(3)+string('VERTEX_FORMAT')+u(len(channels))+bytes(channels)
    f+=string('MATERIAL_NAME')+string('def_material')+string('NAME')+string('Demo metal')+u(0)+u(0)
    f+=u(3)+string('model::Node')+header('Root')
    f+=string('model::ArgAnimationNode')+header('Gear hinge')
    f+=d(*identity)+d(0,1,0)+d(0,0,0,1)+d(math.sin(.2),0,0,math.cos(.2))+d(1,1,1)
    f+=u(0)+u(1)+u(0)+u(3)
    for t,angle in [(-1,-math.pi/3),(0,0),(1,math.pi/3)]:f+=d(t,0,0,math.sin(angle/2),math.cos(angle/2))
    f+=u(0)
    f+=string('model::ArgVisibilityNode')
    if visibility_version:
        f+=raw('Visibility gate')+u(0)+u(1)+string('model::Property<unsigned int>')+string('__VERSION__')+u(visibility_version)
    else:f+=header('Visibility gate')
    f+=u(1)+u(40)+u(1)+d(0.,.75)
    if visibility_version==1:
        f+=struct.pack('<16f',*(identity if visibility_matrix is None else visibility_matrix))
    f+=struct.pack('<3i',-1,0,1)
    vertices=[];indices=[]
    faces=[([1,0,0],[(1,-1,-1),(1,1,-1),(1,1,1),(1,-1,1)]),([-1,0,0],[(-1,-1,1),(-1,1,1),(-1,1,-1),(-1,-1,-1)]),
           ([0,1,0],[(-1,1,-1),(-1,1,1),(1,1,1),(1,1,-1)]),([0,-1,0],[(-1,-1,1),(-1,-1,-1),(1,-1,-1),(1,-1,1)]),
           ([0,0,1],[(1,-1,1),(1,1,1),(-1,1,1),(-1,-1,1)]),([0,0,-1],[(-1,-1,-1),(-1,1,-1),(1,1,-1),(1,-1,-1)])]
    for normal,corners in faces:
        base=len(vertices)
        for pos,uv in zip(corners,[(0,0),(0,1),(1,1),(1,0)]):vertices.append([pos[0]*.12,pos[1]*.8,pos[2]*.12,0,*normal,*uv])
        indices.extend([base,base+1,base+2,base,base+2,base+3])
    v=np.array(vertices,dtype='<f4')
    f+=u(1)+string('RENDER_NODES')+u(1+len(spot_layouts))
    for i,layout in enumerate(spot_layouts):
        f+=string('model::FakeSpotLightsNode')+raw('Spot fixture '+str(i))+u(0)
        if layout is None:f+=u(0)
        else:f+=u(1)+string('model::Property<unsigned int>')+string('__VERSION__')+u(layout)
        # Two controls and two lights exercise per-entry extension handling.
        f+=u(0)+u(0)+u(2)
        for parent in (0,1):f+=u(parent)+u(0)+struct.pack('<3f',0,0,0)
        f+=u(2)
        for direction in ((0.,0.,1.),(0.,1.,0.)):
            f+=struct.pack('<3d10fB',1.,2.,3.,*([.5]*10),1)
            if layout==2:f+=struct.pack('<3f',*direction)
    f+=string('model::NumberNode' if number else 'model::RenderNode')+header('Demo strut')
    f+=u(0)+u(0)+u(1)+u(2)+struct.pack('<i',-1)+u(len(v))+u(9)+v.tobytes()
    f+=bytes([1])+u(len(indices))+u(5)+struct.pack('<'+'H'*len(indices),*indices)
    if number:f+=u(1)+struct.pack('<ifIf',-1,1.,32,.91)
    return f
