"""Portable glTF 2.0 export with independent DCS-argument animation clips."""
import base64,copy,hashlib,json,os,struct,tempfile,itertools
from pathlib import Path
import numpy as np
from .scene import jsonable
from .textures import TextureResolver
from .math3d import quat
from .liveries import read_livery
from .numbering import controls,texture_uv

class Builder:
    def __init__(self):
        self.data=bytearray();self.cache={}
        self.doc={'asset':{'version':'2.0','generator':'EDM Studio 0.3.4'},'scene':0,'scenes':[{'nodes':[0]}],
                  'nodes':[],'meshes':[],'materials':[],'accessors':[],'bufferViews':[],'buffers':[]}

    def blob(self,raw,target=None):
        self.data.extend(b'\0'*((-len(self.data))%4));offset=len(self.data);self.data.extend(raw)
        view={'buffer':0,'byteOffset':offset,'byteLength':len(raw)}
        if target:view['target']=target
        self.doc['bufferViews'].append(view);return len(self.doc['bufferViews'])-1

    def accessor(self,arr,kind,target=None,bounds=False):
        arr=np.ascontiguousarray(arr)
        component={np.dtype('<f4'):5126,np.dtype('<u4'):5125,np.dtype('<u2'):5123,np.dtype('u1'):5121}[arr.dtype]
        if arr.dtype.kind=='f' and not np.isfinite(arr).all():raise ValueError('Non-finite glTF data')
        raw=arr.tobytes();key=(kind,component,target,bounds,len(arr),hashlib.sha256(raw).digest())
        if key in self.cache:return self.cache[key]
        a={'bufferView':self.blob(raw,target),'componentType':component,'count':len(arr),'type':kind}
        if bounds:
            values=arr.reshape(len(arr),-1);a['min']=values.min(axis=0).tolist();a['max']=values.max(axis=0).tolist()
        self.doc['accessors'].append(a);idx=len(self.doc['accessors'])-1;self.cache[key]=idx;return idx

def material_color(mat):
    opacity=mat.uniforms.get('opacityValue',1.)
    opacity=float(opacity) if isinstance(opacity,(float,int)) else 1.
    c=mat.uniforms.get('diffuseColor',(0.66,0.7,0.75,opacity))
    if not isinstance(c,(list,tuple)) or len(c)<3:c=(.66,.7,.75,opacity)
    return [*np.clip(c[:3],0,1).tolist(),max(0.,min(1.,opacity))]

def build_document(scene,arguments=None,duration=3.,textures=None,embed_textures=True,progress=None,livery=None,baseline_args=None):
    if not np.isfinite(duration) or duration<=0:raise ValueError('Animation duration must be positive')
    args=list(scene.limits) if arguments is None else sorted(set(int(a) for a in arguments))
    if any(a not in scene.limits for a in args):raise ValueError('Unknown animation argument')
    b=Builder();d=b.doc;d['nodes']=copy.deepcopy(scene.nodes)
    selected=read_livery(livery) if livery else None
    baseline=dict(selected.custom_args) if selected else {}
    baseline.update(baseline_args or {})
    if any(not isinstance(k,int) or not np.isfinite(v) for k,v in baseline.items()):raise ValueError('Invalid baseline argument')
    for track in scene.tracks:
        if track.argument in baseline:d['nodes'][track.node][track.path]=track.evaluate(baseline[track.argument]).tolist()
    d['scenes'][0]['nodes']=[i for i,p in enumerate(scene.parents) if p<0]
    for n in d['nodes']:
        if 'rotation' in n:n['rotation']=np.clip(quat(n['rotation']),-1,1).tolist()
    for i,p in enumerate(scene.parents):
        if p>=0:d['nodes'][p].setdefault('children',[]).append(i)
    resolver=TextureResolver(scene.source,textures,selected) if embed_textures else None
    image_cache={};texture_count=0;roughmet_count=0;decal_materials={};number_variants=[];number_mesh_count=0
    def embed(found,name):
        png=resolver.png_source(found) if resolver else None
        if not png:return None
        digest=hashlib.sha256(png).hexdigest()
        if digest not in image_cache:
            imageidx=len(d.setdefault('images',[]))
            d['images'].append({'name':name,'mimeType':'image/png','bufferView':b.blob(png)})
            textureidx=len(d.setdefault('textures',[]));d['textures'].append({'source':imageidx})
            image_cache[digest]=textureidx
        return image_cache[digest]
    for i,m in enumerate(scene.materials):
        color=material_color(m)
        mat={'name':m.name or f'Material {i}','pbrMetallicRoughness':{'baseColorFactor':color,'metallicFactor':0.,'roughnessFactor':.65},
             'doubleSided':m.culling==1,'extras':{'edm_shader':m.material_name,'edm_uniforms':jsonable(m.uniforms),
             'edm_animated_uniforms':jsonable(m.animated_uniforms),'edm_textures':jsonable(m.textures)}}
        if m.blending in (1,2,3,4) or color[3]<.999:mat['alphaMode']='BLEND'
        if resolver:
            diffuse=embed(resolver.material(m),m.name+' diffuse')
            if diffuse is not None:
                mat['pbrMetallicRoughness']['baseColorTexture']={'index':diffuse}
                mat['pbrMetallicRoughness']['baseColorFactor']=[1,1,1,color[3]]
                texture_count+=1
            roughmet=embed(resolver.material(m,13),m.name+' RoughMet (linear ORM)')
            if roughmet is not None:
                mat['pbrMetallicRoughness'].update({'metallicRoughnessTexture':{'index':roughmet,'texCoord':1},
                                                  'metallicFactor':1.,'roughnessFactor':1.})
                mat['occlusionTexture']={'index':roughmet,'texCoord':1,'strength':1.}
                roughmet_count+=1
        d['materials'].append(mat)
        if progress:progress(f'材质 {i+1}/{len(scene.materials)}')
    if resolver:
        for i in sorted({mesh.material for mesh in scene.meshes if mesh.number_indices is not None}):
            m=scene.materials[i];decal=embed(resolver.material(m,3),m.name+' number atlas')
            if decal is None:continue
            mat=copy.deepcopy(d['materials'][i]);mat['name']+=' / registration'
            mat['pbrMetallicRoughness'].update({'baseColorTexture':{'index':decal,'texCoord':2},
                                               'baseColorFactor':[1,1,1,1],'metallicFactor':0.})
            mat['alphaMode']='MASK';mat['alphaCutoff']=.1
            decal_materials[i]=len(d['materials']);d['materials'].append(mat)
    for mesh in scene.meshes:
        material=scene.materials[mesh.material]
        attrs={'POSITION':b.accessor(mesh.positions.astype('<f4'),'VEC3',34962,True),
               'NORMAL':b.accessor(mesh.normals.astype('<f4'),'VEC3',34962)}
        if mesh.uvs or 'baseColorTexture' in d['materials'][mesh.material]['pbrMetallicRoughness'] or 'occlusionTexture' in d['materials'][mesh.material]:
            attrs['TEXCOORD_0']=b.accessor(texture_uv(mesh,material,0),'VEC2',34962)
            if 'occlusionTexture' in d['materials'][mesh.material]:
                attrs['TEXCOORD_1']=b.accessor(texture_uv(mesh,material,13),'VEC2',34962)
        if mesh.joints is not None:
            for i in range(mesh.joints.shape[1]//4):
                attrs[f'JOINTS_{i}']=b.accessor(mesh.joints[:,i*4:i*4+4].astype('<u2'),'VEC4',34962)
                attrs[f'WEIGHTS_{i}']=b.accessor(mesh.weights[:,i*4:i*4+4].astype('<f4'),'VEC4',34962)
            ibm=np.transpose(mesh.inverse_bind,(0,2,1)).reshape(-1,16).astype('<f4')
            skin={'name':mesh.name+' / skin','joints':mesh.skin_nodes,'inverseBindMatrices':b.accessor(ibm,'MAT4')}
            d['nodes'][mesh.node]['skin']=len(d.setdefault('skins',[]));d['skins'].append(skin)
        if mesh.number_indices is not None and mesh.material in decal_materials:
            number_mesh_count+=1
            triangles=mesh.indices.reshape(-1,3);selectors=mesh.number_indices[triangles]
            if not np.all(selectors==selectors[:,:1]):raise ValueError('Number selector changes inside a triangle')
            for ci,control in enumerate(controls(mesh)):
                indices=triangles[selectors[:,0]==ci].ravel()
                if not len(indices):continue
                handles=sorted({int(control[k]) for k in (0,2) if int(control[k]) not in (-1,0xffffffff)})
                active=[a for a in handles if a in args]
                values={}
                for a in active:
                    lo,hi=scene.limits[a]
                    if hi-lo>100:raise ValueError('Number animation range is too large; export the selected number as a static model')
                    grid=[lo,hi,*[v/10. for v in range(int(np.ceil(lo*10)),int(np.floor(hi*10))+1)]]
                    values[a]=sorted(set([*grid,baseline.get(a,0.)]))
                if np.prod([len(v) for v in values.values()])>1024:raise ValueError('Number atlas has too many states for portable animation')
                combinations=itertools.product(*(values[a] for a in active))
                for combination in combinations:
                    configuration={**baseline,**dict(zip(active,combination))}
                    uv=texture_uv(mesh,material,3,configuration)
                    attributes={**attrs,'TEXCOORD_2':b.accessor(uv,'VEC2',34962)}
                    attributes.setdefault('TEXCOORD_0',b.accessor(texture_uv(mesh,material,0),'VEC2',34962))
                    attributes.setdefault('TEXCOORD_1',attributes['TEXCOORD_0'])
                    # Registration surfaces get a tiny normal offset, matching
                    # DCS's polygon bias without relying on renderer extensions.
                    attributes['POSITION']=b.accessor((mesh.positions+mesh.normals*.0002).astype('<f4'),'VEC3',34962,True)
                    primitive={'attributes':attributes,'indices':b.accessor(indices.astype('<u4'),'SCALAR',34963),
                               'material':decal_materials[mesh.material],'mode':4}
                    name=f'{mesh.name} / number {ci} / '+','.join(f'{a}={configuration[a]:g}' for a in active)
                    ni=len(d['nodes']);visible=all(configuration[a]==baseline.get(a,0.) for a in active)
                    d['nodes'].append({'name':name,'mesh':len(d['meshes']),'scale':[1.,1.,1.] if visible else [0.,0.,0.]})
                    d['nodes'][mesh.node].setdefault('children',[]).append(ni)
                    d['meshes'].append({'name':name,'primitives':[primitive]})
                    if active:number_variants.append((ni,dict(zip(active,combination)),values))
            continue
        primitive={'attributes':attrs,'indices':b.accessor(mesh.indices.astype('<u4'),'SCALAR',34963),'material':mesh.material,'mode':4}
        d['nodes'][mesh.node]['mesh']=len(d['meshes'])
        d['meshes'].append({'name':mesh.name,'primitives':[primitive]})
    if args:d['animations']=[]
    for arg in args:
        if arg in scene.number_args and not any(arg in state for _,state,_ in number_variants) and not any(tr.argument==arg for tr in scene.tracks):continue
        lo,hi=scene.limits[arg]
        # Static-only arguments still get a positive-duration clip.
        span=hi-lo
        clip={'name':f'Argument {arg:03d}','samplers':[],'channels':[],
              'extras':{'edm_argument':arg,'argument_min':lo,'argument_max':hi,'duration_seconds':duration,
                        'mapping':'argument = argument_min + (time / duration_seconds) * (argument_max - argument_min)',
                        'other_arguments':{str(a):v for a,v in baseline.items() if a!=arg} if baseline else 0}}
        for tr in scene.tracks:
            active=tr.argument==arg
            points=[lo,hi]
            if active:
                if tr.ranges is not None:points.extend(v for r in tr.ranges for v in r if lo<v<hi)
                else:points.extend(k.frame for k in tr.keys if lo<k.frame<hi)
            points=sorted(set(points))
            times=np.array([(v-lo)/span*duration for v in points],dtype='<f4') if span else np.array([0.,duration],dtype='<f4')
            if not span:points=[lo,lo]
            values=np.array([tr.evaluate(v if active else baseline.get(tr.argument,0.)) for v in points],dtype='<f4')
            # Merge collisions after float32 time conversion, preserving the last
            # value (STEP transitions must use the new state at the boundary).
            keep=np.r_[np.diff(times)>0,True];times=times[keep];values=values[keep]
            if tr.path=='rotation':
                for k in range(len(values)):
                    values[k]=quat(values[k])
                    if k and values[k-1]@values[k]<0:values[k]*=-1
            sampler={'input':b.accessor(times,'SCALAR',bounds=True),
                     'output':b.accessor(values,'VEC4' if tr.path=='rotation' else 'VEC3'),
                     'interpolation':'STEP' if tr.ranges is not None else 'LINEAR'}
            clip['channels'].append({'sampler':len(clip['samplers']),'target':{'node':tr.node,'path':tr.path}})
            clip['samplers'].append(sampler)
        for ni,state,grids in number_variants:
            points=sorted(set([lo,hi,*[v for v in grids.get(arg,[]) if lo<v<hi]]))
            times=np.asarray([(v-lo)/span*duration for v in points] if span else [0.,duration],dtype='<f4')
            if not span:points=[lo,lo]
            scales=[]
            for point in points:
                visible=True
                for handle,target in state.items():
                    value=point if handle==arg else baseline.get(handle,0.)
                    grid=grids[handle]
                    chosen=grid[max(0,min(len(grid)-1,int(np.searchsorted(grid,value,side='right'))-1))]
                    visible=visible and chosen==target
                scales.append([1.,1.,1.] if visible else [0.,0.,0.])
            scales=np.asarray(scales,dtype='<f4')
            keep=np.r_[np.diff(times)>0,True];times=times[keep];scales=scales[keep]
            clip['channels'].append({'sampler':len(clip['samplers']),'target':{'node':ni,'path':'scale'}})
            clip['samplers'].append({'input':b.accessor(times,'SCALAR',bounds=True),
                                     'output':b.accessor(scales,'VEC3'),'interpolation':'STEP'})
        if clip['channels']:d['animations'].append(clip)
        if progress:progress(f'动画参数 {arg}（{args.index(arg)+1}/{len(args)}）')
    if not d.get('animations'):d.pop('animations',None)
    exported_args=[c['extras']['edm_argument'] for c in d.get('animations',[])]
    report=scene.summary()
    report.update({'exported_arguments':exported_args,'requested_arguments':args,'skipped_arguments':[a for a in args if a not in exported_args],
                   'clip_duration_seconds':duration,'embedded_diffuse_materials':texture_count,
                   'embedded_roughmet_materials':roughmet_count,'number_meshes':number_mesh_count,
                   'number_animation_nodes':len(number_variants),'baseline_arguments':baseline,
                   'missing_textures':sorted(set(resolver.missing)) if resolver else [],
                   'texture_warnings':resolver.warnings if resolver else [],
                   'resolved_textures':resolver.resolved if resolver else {},
                   'livery':selected.metadata() if selected else None,
                   'visibility_encoding':'STEP scale 0/1, lower-inclusive upper-exclusive',
                   'skinning':'packed uint8 indices + 1; residual weight to bone 0; JOINTS_1 when needed',
                   'material_limitations':'ORM mapped to standard glTF PBR. NumberNode digits use alpha-cutout atlas UVs; number clips use discrete 0.1 argument steps and exact selected baseline. Semi-transparent decal color blending, normal maps and DCS effect shaders are not reproduced.'})
    d['extras']={'edm_studio':{k:v for k,v in report.items() if k!='source'}}
    d['buffers']=[{'byteLength':len(b.data)}]
    return d,bytes(b.data),report

def atomic_write(path,data):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    fd,temp=tempfile.mkstemp(prefix=path.name+'.',suffix='.tmp',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as f:f.write(data)
        os.replace(temp,path)
    finally:
        if os.path.exists(temp):os.unlink(temp)

def export_scene(scene,path,**options):
    path=Path(path)
    if path.suffix.lower() not in ('.glb','.gltf'):raise ValueError('Export format must be .glb or .gltf')
    d,binary,report=build_document(scene,**options)
    if path.suffix.lower()=='.gltf':
        d['buffers'][0]['uri']='data:application/octet-stream;base64,'+base64.b64encode(binary).decode('ascii')
        data=json.dumps(d,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf-8')
    else:
        raw=json.dumps(d,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf-8')
        raw+=b' '*((-len(raw))%4);binary+=b'\0'*((-len(binary))%4)
        length=12+8+len(raw)+8+len(binary)
        data=struct.pack('<III',0x46546c67,2,length)+struct.pack('<II',len(raw),0x4e4f534a)+raw+struct.pack('<II',len(binary),0x004e4942)+binary
    # Validate serialization before touching the requested output.
    atomic_write(path,data)
    atomic_write(path.with_suffix('.report.json'),json.dumps(report,ensure_ascii=False,indent=2,allow_nan=False).encode('utf-8'))
    return report
