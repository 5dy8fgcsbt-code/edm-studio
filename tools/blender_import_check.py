"""Third-party interoperability check; runs in Blender background mode."""
import bpy,sys,json,time
from pathlib import Path
args=sys.argv[sys.argv.index('--')+1:];path=Path(args[0]);out=Path(args[1])
bpy.ops.wm.read_factory_settings(use_empty=True)
start=time.perf_counter()
bpy.ops.import_scene.gltf(filepath=str(path),import_shading='NORMALS')
meshes=[o for o in bpy.context.scene.objects if o.type=='MESH']
arms=[o for o in bpy.context.scene.objects if o.type=='ARMATURE']
actions=list(bpy.data.actions)
result={'file':str(path),'blender_version':bpy.app.version_string,'mesh_objects':len(meshes),'armatures':len(arms),'actions':len(actions),
        'action_names':[a.name for a in actions[:30]],'import_seconds':time.perf_counter()-start}
# Evaluate world geometry at two times in the default imported clip. Sampling
# the same vertices checks real animation evaluation, including skin modifiers.
def positions(frame):
    bpy.context.scene.frame_set(frame)
    dg=bpy.context.evaluated_depsgraph_get();values=[]
    for obj in meshes:
        ev=obj.evaluated_get(dg)
        if len(ev.data.vertices):
            for i in [0,len(ev.data.vertices)//2,len(ev.data.vertices)-1]:values.append(tuple(ev.matrix_world@ev.data.vertices[i].co))
    return values
a=positions(1);b=positions(60)
result['max_vertex_motion']=max((sum((x-y)**2 for x,y in zip(p,q))**.5 for p,q in zip(a,b)),default=0)
result['passed']=len(meshes)>0 and len(actions)>0 and result['max_vertex_motion']>1e-4
out.write_text(json.dumps(result,indent=2),encoding='utf-8')
print('EDM_STUDIO_BLENDER_CHECK',json.dumps(result),flush=True)
if not result['passed']:raise RuntimeError('No animated geometry after Blender import')
