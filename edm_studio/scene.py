"""EDM -> explicit transform graph shared by the preview and glTF exporter.

Confirmed against UniModelDesc!ArgAnimationNode::animate:
M T(p + sum(pos)) Q1 Rn ... R1
  Q2^-1 Sbase Q2  Qsn^-1 Sn Qsn ... Qs1^-1 S1 Qs1.
Keeping these as separate nodes preserves nonuniform scale and shear exactly.
"""
from dataclasses import dataclass, field, replace
from pathlib import Path
from collections import Counter
import numpy as np
from .math3d import *
from .edm import types as t
from .edm.parser import EDMFileParser

@dataclass
class Track:
    node: int
    path: str
    argument: int
    keys: list = field(default_factory=list)
    ranges: list | None = None
    conjugate: bool = False
    stored_wxyz: bool = True

    def evaluate(self,value):
        if self.ranges is not None:
            visible=any(lo<=value<hi for lo,hi in self.ranges)
            return np.full(3,1. if visible else 0.)
        q=self.path=='rotation'
        v=sample(self.keys,value,q,self.stored_wxyz)
        return inverse(v) if self.conjugate else v

    def domain(self):
        if self.ranges is not None:
            # Huge numbers are unbounded visibility sentinels, not clip times.
            points=[v for r in self.ranges for v in r if np.isfinite(v) and abs(v)<=1e4]
            return min([0.,*points]),max([1.,*points])
        return min(k.frame for k in self.keys),max(k.frame for k in self.keys)

@dataclass
class Mesh:
    name: str
    node: int
    material: int
    positions: np.ndarray
    normals: np.ndarray
    indices: np.ndarray
    uvs: list = field(default_factory=list)
    joints: np.ndarray | None = None
    weights: np.ndarray | None = None
    skin_nodes: list = field(default_factory=list)
    inverse_bind: np.ndarray | None = None
    extras: dict = field(default_factory=dict)
    number_indices: np.ndarray | None = None

def rotation_quat(m):
    """Stable matrix -> quaternion, via the symmetric eigenproblem."""
    r=np.array(m)[:3,:3]
    k=np.array([[r[0,0]-r[1,1]-r[2,2],r[0,1]+r[1,0],r[0,2]+r[2,0],r[2,1]-r[1,2]],
                [r[0,1]+r[1,0],r[1,1]-r[0,0]-r[2,2],r[1,2]+r[2,1],r[0,2]-r[2,0]],
                [r[0,2]+r[2,0],r[1,2]+r[2,1],r[2,2]-r[0,0]-r[1,1],r[1,0]-r[0,1]],
                [r[2,1]-r[1,2],r[0,2]-r[2,0],r[1,0]-r[0,1],np.trace(r)]])/3
    _,v=np.linalg.eigh(k)
    q=v[:,-1]
    return q if q[3]>=0 else -q

def jsonable(obj):
    if isinstance(obj,np.ndarray): return obj.tolist()
    if isinstance(obj,np.generic): return obj.item()
    if isinstance(obj,dict):return {k:jsonable(v) for k,v in obj.items()}
    if hasattr(obj,'__dict__'):return {k:jsonable(v) for k,v in vars(obj).items()}
    if isinstance(obj,(list,tuple)):return [jsonable(v) for v in obj]
    return obj

def orient_indices(positions,normals,indices):
    """Normalize a uniformly reversed EDM surface to glTF's local winding.

    Some mirrored EDM parts already reverse indices in mesh space. Their
    vertex normals reveal this; correcting only the node determinant would
    compensate the reflection twice and cull the exterior (F-14 right flaps).
    Do not rewrite mixed, ambiguous or degenerate surfaces triangle by triangle.
    """
    tris=indices.reshape(-1,3);p=positions[tris].astype(np.float64)
    cross=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);area=np.linalg.norm(cross,axis=1)
    dot=np.einsum('ij,ij->i',cross,normals[tris].mean(axis=1))
    reliable=np.abs(dot)>area*.2
    total=float(area.sum());evidence=float(area[reliable].sum())
    reverse=total>0 and evidence>total*.5 and float(area[reliable&(dot<0)].sum())>evidence*.95
    return (np.ascontiguousarray(tris[:,[0,2,1]].ravel()) if reverse else indices),reverse

class Scene:
    def __init__(self,parsed,source='model.edm'):
        self.edm=parsed; self.source=Path(source)
        self.nodes=[]; self.parents=[]; self.meshes=[]; self.tracks=[]
        self.warnings=[]; self.heads=[]; self.tails=[]
        self.materials=parsed.materials
        self.visibility={};self.gates={};self.skin_clones={};self.track_map={}
        self.limits={}; self._static=[]
        self.add_node(self.source.stem,-1,extras={'edm_version':parsed.version,'units':'metres','up_axis':'Y'})
        self._build_nodes()
        self._build_meshes()
        self._build_connectors()
        self._build_domains()
        self._static=[self.local_matrix(n) for n in self.nodes]
        self.order=self._topological_order()
        self._default_world=self.evaluate({})
        if not self.meshes:raise ValueError('文件没有可导出的三角网格')
        for mat in self.materials:
            if mat.animated_uniforms:
                self.warn('EDM 材质参数动画以元数据保留；DCS 专用着色器无法完整映射为 glTF PBR。')

    def warn(self,msg):
        if msg not in self.warnings:self.warnings.append(msg)

    def add_node(self,name,parent,**fields):
        idx=len(self.nodes); self.nodes.append({'name':name,**fields}); self.parents.append(parent);return idx

    def factor(self,parent,name,path,value):
        value=np.asarray(value,dtype=float)
        if not np.all(np.isfinite(value)):raise ValueError(f'{name}: non-finite transform')
        return self.add_node(name,parent,**{path:value.tolist()})

    def affine(self,parent,name,m):
        if not np.isfinite(m).all() or not np.allclose(m[3],[0,0,0,1],atol=1e-7):
            raise ValueError(f'{name}: invalid affine matrix')
        if np.allclose(m,np.eye(4),atol=1e-12):return parent
        u,s,vt=np.linalg.svd(m[:3,:3])
        if np.linalg.det(u)<0:u[:,-1]*=-1;s[-1]*=-1
        if np.linalg.det(vt)<0:vt[-1,:]*=-1;s[-1]*=-1
        n=self.add_node(name,parent,translation=m[:3,3].tolist(),rotation=rotation_quat(u).tolist(),scale=s.tolist())
        return self.factor(n,name+' / basis','rotation',rotation_quat(vt))

    def animated(self,parent,name,path,arg,keys,**kwargs):
        if not keys:return parent
        times=np.array([k.frame for k in keys])
        if not np.isfinite(times).all() or (np.diff(times)<=0).any():
            raise ValueError(f'{name}: 关键帧必须有限且严格递增')
        idx=self.add_node(name,parent)
        track=Track(idx,path,arg,keys,**kwargs)
        self.nodes[idx][path]=track.evaluate(0.).tolist()
        self.nodes[idx]['extras']={'edm_argument':arg,'edm_channel':path}
        self.tracks.append(track)
        return idx

    def _build_nodes(self):
        for i,n in enumerate(self.edm.nodes):
            name=n.name or f'{n.type} {i}'
            h=self.add_node(name,0,extras={'edm_node':i,'edm_type':n.type,'edm_properties':jsonable(n.props)})
            p=h
            if isinstance(n,t.ArgAnimationNode):
                b=n.base
                p=self.affine(p,name+' / matrix',matrix(b.matrix))
                p=self.factor(p,name+' / position','translation',b.position)
                for arg,keys in n.pos_data:p=self.animated(p,name+f' / position {arg}','translation',arg,keys)
                p=self.factor(p,name+' / Q1','rotation',quat(xyzw(b.quat1)))
                for arg,keys in reversed(n.rot_data):p=self.animated(p,name+f' / rotation {arg}','rotation',arg,keys)
                p=self.factor(p,name+' / inverse scale basis','rotation',inverse(xyzw(b.quat2)))
                p=self.factor(p,name+' / base scale','scale',b.scale)
                p=self.factor(p,name+' / scale basis','rotation',quat(xyzw(b.quat2)))
                for arg,(qkeys,skeys) in reversed(n.scale_data):
                    if not skeys:raise ValueError(f'{name}: 缺少缩放向量关键帧')
                    p=self.animated(p,name+f' / inverse scale basis {arg}','rotation',arg,qkeys,stored_wxyz=False,conjugate=True)
                    p=self.animated(p,name+f' / scale {arg}','scale',arg,skeys)
                    p=self.animated(p,name+f' / scale basis {arg}','rotation',arg,qkeys,stored_wxyz=False)
            elif isinstance(n,t.BoneNode):p=self.affine(p,name+' / matrix',matrix(n.matrix1))
            elif isinstance(n,t.TransformNode):p=self.affine(p,name+' / matrix',matrix(n.matrix))
            elif isinstance(n,t.ArgVisibilityNode):
                # Visibility is a render predicate, not a bone transform.
                self.visibility[i]=n.vis_data
                if n.trailing_matrix:
                    self.nodes[h]['extras']['edm_visibility_matrix']=jsonable(n.trailing_matrix)
                    if not np.allclose(matrix(n.trailing_matrix),np.eye(4),atol=1e-7,rtol=0):
                        raise ValueError(f'{name}: 非单位可见性扩展矩阵的语义尚未验证，无法可靠转换。')
            elif isinstance(n,t.BillboardNode):
                self.warn('Billboard 随相机朝向的行为未导出。')
            elif isinstance(n,t.LodNode):
                self.nodes[h]['extras']['edm_lod_levels']=jsonable(n.levels)
                self.warn('内部 LOD 层级以元数据保留；当前导出所有层级，可在目标软件中筛选。')
            self.heads.append(h);self.tails.append(p)
        for i,n in enumerate(self.edm.nodes):self.parents[self.heads[i]]=self.tails[n.parent_idx] if n.parent_idx>=0 else 0
        self.track_map={tr.node:tr for tr in self.tracks}

    def visibility_chain(self,source_node):
        out=[]
        while source_node>=0:
            if source_node in self.visibility:out.append(source_node)
            source_node=self.edm.nodes[source_node].parent_idx
        return tuple(sorted(out))

    def gate(self,parent,conditions):
        if not conditions:return parent
        key=(parent,conditions)
        if key in self.gates:return self.gates[key]
        p=parent
        for source_idx in conditions:
            for arg,ranges in self.visibility[source_idx]:
                v=self.add_node(f'Visibility {source_idx} / {arg}',p)
                tr=Track(v,'scale',arg,ranges=ranges)
                self.nodes[v]['scale']=tr.evaluate(0).tolist()
                self.nodes[v]['extras']={'edm_visibility_ranges':jsonable(ranges),'edm_argument':arg}
                self.tracks.append(tr);p=v
        self.gates[key]=p;return p

    def skin_joint(self,node,conditions):
        if not conditions:return node
        clones=self.skin_clones.setdefault(conditions,{})
        if node in clones:return clones[node]
        if node==0:return self.gate(0,conditions)
        parent=self.skin_joint(self.parents[node],conditions)
        fields={k:jsonable(v) for k,v in self.nodes[node].items() if k!='name'}
        idx=self.add_node(self.nodes[node]['name']+' / skin visibility',parent,**fields)
        if node in self.track_map:self.tracks.append(replace(self.track_map[node],node=idx))
        clones[node]=idx;return idx

    def _build_connectors(self):
        for n in self.edm.connectors:
            if n.parent>=len(self.tails):raise ValueError('Invalid connector parent')
            self.add_node(n.name or 'Connector',self.tails[n.parent] if n.parent>=0 else 0,extras={'edm_type':'Connector','edm_properties':jsonable(n.props)})

    def _build_meshes(self):
        for ri,n in enumerate(self.edm.render_nodes):
            if not isinstance(n,(t.RenderNode,t.SkinNode)):
                self.warn(f'{n.type}：DCS 灯光效果保留在报告中，未转换为网格。'); continue
            if not (0<=n.material_id<len(self.materials)):raise ValueError('Invalid material index')
            fmt=self.materials[n.material_id].vertex_format
            raw=np.asarray(n.vertex_data,dtype='<f4')
            ids=np.asarray(n.index_data,dtype=np.uint32)
            if len(ids)==0 or len(raw)==0:continue
            if len(ids)%3 or ids.max()>=len(raw):raise ValueError(f'Render {ri}: invalid triangle indices')
            if fmt is None or fmt.stride!=raw.shape[1]:raise ValueError(f'Render {ri}: vertex format mismatch')
            posoff=fmt.offset_of(0); noff=fmt.offset_of(1)
            if posoff<0 or fmt.size_of(0)<3:raise ValueError('Missing POSITION channel')
            if isinstance(n,t.SkinNode):parts=[(None,ids)]
            else:
                parts=[]
                ends=len(n.parents)>1 and n.parents[-1].index_start==len(ids)
                groups=None
                if len(n.parents)>1 and fmt.size_of(0)==4:
                    groups=raw[ids,posoff+3].reshape(-1,3)
                    if not np.isfinite(groups).all() or (groups<0).any() or (groups>=len(n.parents)).any() or (groups!=np.floor(groups)).any() or not np.all(groups==groups[:,:1]):
                        raise ValueError(f'Render {ri}: invalid per-vertex transform index')
                for pi,pe in enumerate(n.parents):
                    if groups is not None:
                        # Modern files leave all index boundaries at zero.
                        # The shader selects transforms from POSITION.w.
                        parts.append((pe,ids.reshape(-1,3)[groups[:,0]==pi].ravel()))
                        continue
                    if ends:
                        start=n.parents[pi-1].index_start if pi else 0;end=pe.index_start
                    else:
                        start=pe.index_start;end=n.parents[pi+1].index_start if pi+1<len(n.parents) else len(ids)
                    if start<0 or end<start or end>len(ids) or start%3 or end%3:raise ValueError('Invalid multi-parent index range')
                    parts.append((pe,ids[start:end]))
                if not n.parents:parts=[(None,ids)]
                elif groups is None and not ends and n.parents[0].index_start!=0:raise ValueError('Missing leading parent range')
            for pi,(pe,indices) in enumerate(parts):
                if len(indices)==0:continue
                used,inv=np.unique(indices,return_inverse=True)
                v=raw[used]; pos=np.ascontiguousarray(v[:,posoff:posoff+3])
                if not np.isfinite(pos).all():raise ValueError('Non-finite vertex position')
                if noff>=0 and fmt.size_of(1)>=3:
                    nor=np.array(v[:,noff:noff+3],copy=True)
                    lengths=np.linalg.norm(nor,axis=1)
                    bad=(lengths<1e-10)|~np.isfinite(lengths)
                    nor[bad]=[0,1,0]; lengths[bad]=1
                    nor/=lengths[:,None]
                else:
                    nor=np.zeros_like(pos)
                    tris=inv.reshape(-1,3)
                    norms=np.cross(pos[tris[:,1]]-pos[tris[:,0]],pos[tris[:,2]]-pos[tris[:,0]])
                    for k in range(3):np.add.at(nor,tris[:,k],norms)
                    lengths=np.linalg.norm(nor,axis=1);nor/=np.maximum(lengths,1e-20)[:,None]
                    nor[lengths<1e-20]=[0,1,0]
                uvs=[np.ascontiguousarray(v[:,fmt.offset_of(c):fmt.offset_of(c)+2]) for c in range(4,9) if fmt.size_of(c)>=2]
                # DCS DDS and glTF both use image-top V=0. Do not flip twice.
                name=n.name or f'{n.type} {ri}'
                if len(parts)>1:name+=f' / {pi}'
                parent=pe.node if pe else -1
                if parent>=len(self.tails):raise ValueError('Invalid render parent')
                extras={'edm_render_index':ri,'edm_type':n.type,'edm_parent':parent,'edm_damage_argument':pe.damage_arg if pe else -1,'edm_properties':jsonable(n.props)}
                conditions=self.visibility_chain(n.bones[0] if isinstance(n,t.SkinNode) else parent)
                placement=self.gate(self.tails[parent] if parent>=0 else 0,conditions) if not isinstance(n,t.SkinNode) else -1
                idx=self.add_node(name,placement,extras=extras)
                mesh=Mesh(name,idx,n.material_id,pos,np.ascontiguousarray(nor),np.asarray(inv,dtype=np.uint32),uvs,extras=extras)
                mesh.indices,reversed_winding=orient_indices(mesh.positions,mesh.normals,mesh.indices)
                if reversed_winding:mesh.extras['edm_winding_reversed']=True
                if n.type=='NumberNode':
                    off=fmt.offset_of(21);control=n.props.get('number_controls',[])
                    if off>=0 and control:
                        if not np.isfinite([[c[1],c[3]] for c in control]).all():raise ValueError('Invalid NumberNode UV multiplier')
                        selectors=v[:,off]
                        if not np.isfinite(selectors).all() or (selectors!=np.floor(selectors)).any() or (selectors<0).any() or (selectors>=len(control)).any():
                            raise ValueError('Invalid NumberNode control selector')
                        mesh.number_indices=selectors.astype(np.int32)
                if isinstance(n,t.SkinNode):
                    if not n.bones or any(b<0 or b>=len(self.tails) for b in n.bones):raise ValueError('Invalid skin bone table')
                    if fmt.size_of(0)!=4 or fmt.size_of(21)!=4:raise ValueError('Unsupported skin vertex layout')
                    # Four uint8 JOINTS packed in the fourth POSITION word.
                    mesh.joints=np.ascontiguousarray(v[:,posoff+3]).view(np.uint8).reshape(-1,4).astype(np.uint16)+1
                    off=fmt.offset_of(21);mesh.weights=np.array(v[:,off:off+4],copy=True)
                    if not np.isfinite(mesh.weights).all() or (mesh.weights<0).any():raise ValueError('Invalid skin weights')
                    nonzero=mesh.weights>0
                    if (mesh.joints[nonzero]>=len(n.bones)).any():raise ValueError('Invalid skin joint index')
                    sums=mesh.weights.sum(axis=1)
                    # DCS vt_utils.hlsl: the implicit fifth influence is
                    # saturate(1-sum(weights)) * palette[0]. glTF can express
                    # this losslessly using JOINTS_1/WEIGHTS_1.
                    remainder=np.maximum(0.,1.-sums)
                    if (remainder>1e-7).any():
                        mesh.joints=np.pad(mesh.joints,((0,0),(0,4)))
                        mesh.weights=np.pad(mesh.weights,((0,0),(0,4)))
                        mesh.weights[:,4]=remainder
                        sums=mesh.weights.sum(axis=1)
                    nonzero=mesh.weights>0
                    if (mesh.joints[nonzero]>=len(n.bones)).any():raise ValueError('Invalid skin joint index')
                    mesh.joints[~nonzero]=0
                    mesh.weights/=sums[:,None]
                    mesh.skin_nodes=[self.skin_joint(self.tails[b],conditions) for b in n.bones]
                    ibm=[]
                    for b in n.bones:
                        bone=self.edm.nodes[b]
                        values=bone.matrix2 if isinstance(bone,t.BoneNode) else getattr(bone,'bone_transform',None)
                        if not values:raise ValueError(f'Bone {b} has no inverse bind matrix')
                        ibm.append(matrix(values))
                    mesh.inverse_bind=np.array(ibm)
                self.meshes.append(mesh)
        if self.edm.shell_nodes:self.warn('碰撞壳体未加入可见模型；节点数量记录在报告中。')

    def _build_domains(self):
        from .numbering import number_arguments
        self.number_args=sorted({arg for mesh in self.meshes if mesh.number_indices is not None for arg in number_arguments(mesh)})
        for tr in self.tracks:
            lo,hi=tr.domain()
            if tr.argument not in self.limits:self.limits[tr.argument]=[lo,hi]
            else:self.limits[tr.argument]=[min(lo,self.limits[tr.argument][0]),max(hi,self.limits[tr.argument][1])]
        for arg in self.number_args:self.limits.setdefault(arg,[0.,1.])
        self.limits=dict(sorted(self.limits.items()))

    @staticmethod
    def local_matrix(n):
        if 'matrix' in n:return matrix(n['matrix'])
        return translation(n.get('translation',[0,0,0]))@rotation(n.get('rotation',IDENTITY_Q))@scaling(n.get('scale',[1,1,1]))

    def _topological_order(self):
        order=[]; children=[[] for _ in self.nodes]
        for i,p in enumerate(self.parents):
            if p>=0:children[p].append(i)
        pending=[i for i,p in enumerate(self.parents) if p<0]
        while pending:
            i=pending.pop();order.append(i);pending.extend(children[i])
        if len(order)!=len(self.nodes):raise ValueError('Scene graph cycle')
        return order

    def evaluate(self,args):
        mats=np.array(self._static)
        for tr in self.tracks:
            v=tr.evaluate(args.get(tr.argument,0.))
            mats[tr.node]=translation(v) if tr.path=='translation' else rotation(v) if tr.path=='rotation' else scaling(v)
        for i in self.order:
            if self.parents[i]>=0:mats[i]=mats[self.parents[i]]@mats[i]
        return mats

    def transformed(self,mesh,world):
        if mesh.joints is None:
            m=world[mesh.node];pos=mesh.positions@m[:3,:3].T+m[:3,3]
            return pos
        palette=world[mesh.skin_nodes]@mesh.inverse_bind
        pos=np.zeros_like(mesh.positions,dtype=np.float64)
        for k in range(mesh.joints.shape[1]):
            m=palette[mesh.joints[:,k]]
            pos+=(np.einsum('nij,nj->ni',m[:,:3,:3],mesh.positions)+m[:,:3,3])*mesh.weights[:,k,None]
        return pos

    def summary(self):
        return {'source':str(self.source),'edm_version':self.edm.version,'scene_nodes':len(self.edm.nodes),
                'render_types':dict(Counter(n.type for n in self.edm.render_nodes)),
                'export_meshes':len(self.meshes),'triangles':sum(len(m.indices)//3 for m in self.meshes),
                'skinned_meshes':sum(m.joints is not None for m in self.meshes),'materials':len(self.materials),
                'arguments':{str(k):v for k,v in self.limits.items()},'animation_tracks':len(self.tracks),
                'number_arguments':self.number_args,
                'winding_normalized_meshes':sum(bool(m.extras.get('edm_winding_reversed')) for m in self.meshes),
                'connectors':len(self.edm.connectors),'collision_nodes':len(self.edm.shell_nodes),
                'warnings':self.warnings,'animation_formula':'M T(p+sum(pos)) Q1 Rn...R1 Q2^-1 Sbase Q2 Qsn^-1 Sn Qsn ... Qs1^-1 S1 Qs1'}

def load_scene(path):
    with open(path,'rb') as f:parsed=EDMFileParser(f).parse()
    return Scene(parsed,path)
