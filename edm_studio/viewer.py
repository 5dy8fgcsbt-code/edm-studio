"""Native Qt/OpenGL model viewport; no web service or model uploads."""
import ctypes,math
import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QLabel
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from OpenGL.GL import *
from OpenGL.GLU import gluPerspective,gluLookAt
from .gltf import material_color
from .numbering import texture_uv
from .pbr_shader import VERTEX,FRAGMENT
from .math3d import projection_planes

class Viewport(QOpenGLWidget):
    def __init__(self,parent=None):
        super().__init__(parent)
        self.scene=None;self.args={};self.world=None;self.buffers=[];self.dirty=True
        self.texture_ids={};self.material_textures={};self.roughmet_textures={};self.decal_textures={}
        self.show_textures=True;self.show_roughmet=True;self.program=0;self.uniforms={}
        self.yaw=135.;self.pitch=18.;self.distance=20.;self.target=np.zeros(3);self.radius=5.
        self.wire=False;self.last=None;self.gpu='';self.error=None
        self.setMinimumSize(540,400)
        # Raster widget text is composed over the GL view by Qt. It must not
        # share the model renderer's GL state or consume camera mouse events.
        self.overlay=QLabel(self);self.overlay.setTextFormat(Qt.TextFormat.PlainText)
        self.overlay.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.overlay.setStyleSheet('background:transparent; color:#8b9fb8; border:0;')
        self.overlay.setWordWrap(True);self._overlay_state=None
        self._update_overlay()

    def _update_overlay(self):
        state=(self.scene is not None,self.error)
        if state==self._overlay_state:return
        self._overlay_state=state
        if self.error:
            self.overlay.setAlignment(Qt.AlignmentFlag.AlignLeft|Qt.AlignmentFlag.AlignTop)
            self.overlay.setText('OpenGL: '+self.error)
        elif self.scene is None:
            self.overlay.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.overlay.setText('打开或拖入 EDM 文件\n\n在这里检查模型与动画')
        else:
            self.overlay.setAlignment(Qt.AlignmentFlag.AlignLeft|Qt.AlignmentFlag.AlignBottom)
            self.overlay.setText('左键旋转  ·  右键平移  ·  滚轮缩放  ·  F 重置视角')

    def resizeEvent(self,event):
        super().resizeEvent(event)
        self.overlay.setGeometry(self.rect().adjusted(18,18,-18,-18))

    def initializeGL(self):
        glClearColor(.035,.048,.067,1.)
        glEnable(GL_DEPTH_TEST);glEnable(GL_NORMALIZE)
        glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);glEnable(GL_LIGHT1)
        glEnable(GL_COLOR_MATERIAL);glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE)
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE,GL_TRUE)
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT,[.24,.24,.27,1])
        glLightfv(GL_LIGHT0,GL_DIFFUSE,[.8,.83,.9,1])
        glLightfv(GL_LIGHT1,GL_DIFFUSE,[.3,.36,.42,1])
        self.gpu=glGetString(GL_RENDERER).decode(errors='replace')
        from OpenGL.GL.shaders import compileProgram,compileShader
        self.program=compileProgram(compileShader(VERTEX,GL_VERTEX_SHADER),compileShader(FRAGMENT,GL_FRAGMENT_SHADER))
        self.uniforms={n:glGetUniformLocation(self.program,n) for n in ('baseMap','rmMap','decalMap','hasBase','hasRM','hasDecal','isNumber','baseColor')}

    def set_scene(self,scene):
        self.makeCurrent()
        for b in self.buffers:
            glDeleteBuffers(6,[b[k] for k in ('pos','nor','index','uv','rmuv','decaluv')])
        self._clear_textures()
        self.buffers=[];self.scene=scene;self.args={};self.world=scene.evaluate({});self.error=None
        for mesh in scene.meshes:
            ids=glGenBuffers(6)
            row={k:int(v) for k,v in zip(('pos','nor','index','uv','rmuv','decaluv'),ids)};row['count']=len(mesh.indices)
            mat=scene.materials[mesh.material]
            for key,data,target in [('pos',mesh.positions,GL_ARRAY_BUFFER),('nor',mesh.normals,GL_ARRAY_BUFFER),
                                    ('uv',texture_uv(mesh,mat,0),GL_ARRAY_BUFFER),('rmuv',texture_uv(mesh,mat,13),GL_ARRAY_BUFFER),
                                    ('decaluv',texture_uv(mesh,mat,3),GL_ARRAY_BUFFER),('index',mesh.indices,GL_ELEMENT_ARRAY_BUFFER)]:
                glBindBuffer(target,row[key]);glBufferData(target,data.nbytes,data,GL_DYNAMIC_DRAW if key!='index' else GL_STATIC_DRAW)
            self.buffers.append(row)
        glBindBuffer(GL_ARRAY_BUFFER,0);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0)
        self.doneCurrent();self.dirty=True;self.fit();self.update()

    def _clear_textures(self):
        if self.texture_ids:glDeleteTextures(list(self.texture_ids.values()))
        self.texture_ids={};self.material_textures={};self.roughmet_textures={};self.decal_textures={}

    def set_textures(self,preview):
        self.makeCurrent();self._clear_textures()
        try:
            glPixelStorei(GL_UNPACK_ALIGNMENT,1)
            for key,(width,height,pixels) in preview.images.items():
                tex=int(glGenTextures(1));self.texture_ids[key]=tex;glBindTexture(GL_TEXTURE_2D,tex)
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR)
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR)
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT)
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels)
                glGenerateMipmap(GL_TEXTURE_2D)
            self.material_textures={i:self.texture_ids[key] for i,key in preview.materials.items()}
            self.roughmet_textures={i:self.texture_ids[key] for i,key in preview.roughmet.items()}
            self.decal_textures={i:self.texture_ids[key] for i,key in preview.decals.items()}
            glBindTexture(GL_TEXTURE_2D,0)
        finally:self.doneCurrent()
        self.update()

    def set_arguments(self,args):
        self.args=dict(args)
        if self.scene:self.world=self.scene.evaluate(self.args)
        self.dirty=True;self.update()

    def fit(self):
        if not self.scene:return
        points=[]
        for m in self.scene.meshes:
            v=self.scene.transformed(m,self.world)
            if len(v):points.extend([v.min(axis=0),v.max(axis=0)])
        a=np.array(points);lo=a.min(axis=0);hi=a.max(axis=0)
        self.target=(lo+hi)/2;self.radius=max(.1,float(np.linalg.norm(hi-lo))/2)
        self.distance=self.radius*2.8;self.update()

    def paintGL(self):
        try:
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)
            if not self.error:self._paint_model()
        except Exception as e:
            self.error=str(e)
        finally:glUseProgram(0)
        self._update_overlay()

    def _paint_model(self):
        glUseProgram(0);glActiveTexture(GL_TEXTURE0);glClientActiveTexture(GL_TEXTURE0)
        # Re-establish depth/lighting after Qt's window composition so rear
        # panels cannot paint over the skin.
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE)
        glDisable(GL_BLEND);glDisable(GL_TEXTURE_2D);glEnable(GL_NORMALIZE)
        glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);glEnable(GL_LIGHT1)
        glEnable(GL_COLOR_MATERIAL)
        w=max(1,self.width());h=max(1,self.height());ratio=self.devicePixelRatioF()
        glViewport(0,0,int(w*ratio),int(h*ratio))
        glMatrixMode(GL_PROJECTION);glLoadIdentity()
        near,far=projection_planes(self.distance,self.radius)
        gluPerspective(42,w/h,near,far)
        glMatrixMode(GL_MODELVIEW);glLoadIdentity()
        yaw=math.radians(self.yaw);pitch=math.radians(self.pitch)
        eye=self.target+self.distance*np.array([math.cos(pitch)*math.cos(yaw),math.sin(pitch),math.cos(pitch)*math.sin(yaw)])
        gluLookAt(*eye,*self.target,0,1,0)
        glLightfv(GL_LIGHT0,GL_POSITION,[1,2,1,0]);glLightfv(GL_LIGHT1,GL_POSITION,[-1,.5,-1,0])
        glDisable(GL_LIGHTING);glLineWidth(1.)
        step=10**math.floor(math.log10(max(.1,self.radius/5)))
        glColor3f(.11,.16,.21);glBegin(GL_LINES)
        for i in range(-20,21):
            glVertex3f(i*step,-self.radius*.55,-20*step);glVertex3f(i*step,-self.radius*.55,20*step)
            glVertex3f(-20*step,-self.radius*.55,i*step);glVertex3f(20*step,-self.radius*.55,i*step)
        glEnd();glEnable(GL_LIGHTING)
        if not self.scene:return
        glPolygonMode(GL_FRONT_AND_BACK,GL_LINE if self.wire else GL_FILL)
        glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_NORMAL_ARRAY)
        glUseProgram(self.program)
        for name,unit in [('baseMap',0),('rmMap',1),('decalMap',2)]:glUniform1i(self.uniforms[name],unit)
        def translucent(mesh):
            mat=self.scene.materials[mesh.material]
            return mat.blending in (1,2,3,4) or material_color(mat)[3]<.999
        # Draw transparent glass after opaque surfaces; retain model depth for occlusion.
        ordered=sorted(zip(self.scene.meshes,self.buffers),key=lambda pair:translucent(pair[0]))
        for mesh,buf in ordered:
            mat=self.scene.materials[mesh.material]
            if self.dirty and mesh.number_indices is not None:
                uv=texture_uv(mesh,mat,3,self.args)
                glBindBuffer(GL_ARRAY_BUFFER,buf['decaluv']);glBufferData(GL_ARRAY_BUFFER,uv.nbytes,uv,GL_DYNAMIC_DRAW)
            if mesh.joints is not None and self.dirty:
                palette=self.world[mesh.skin_nodes]@mesh.inverse_bind
                positions=self.scene.transformed(mesh,self.world).astype(np.float32)
                normals=np.zeros_like(mesh.normals)
                for k in range(mesh.joints.shape[1]):
                    transforms=palette[mesh.joints[:,k],:3,:3]
                    normals+=np.einsum('nij,nj->ni',transforms,mesh.normals)*mesh.weights[:,k,None]
                lengths=np.linalg.norm(normals,axis=1);normals/=np.maximum(lengths,1e-20)[:,None]
                for name,data in [('pos',positions),('nor',normals)]:
                    glBindBuffer(GL_ARRAY_BUFFER,buf[name]);glBufferData(GL_ARRAY_BUFFER,data.nbytes,data,GL_DYNAMIC_DRAW)
            glPushMatrix()
            if mesh.joints is None:glMultMatrixd(self.world[mesh.node].T.copy())
            if mat.culling==1:glDisable(GL_CULL_FACE)
            else:
                glEnable(GL_CULL_FACE);glCullFace(GL_BACK)
                transform=self.world[mesh.node] if mesh.joints is None else self.world[mesh.skin_nodes[0]]@mesh.inverse_bind[0]
                glFrontFace(GL_CW if np.linalg.det(transform[:3,:3])<0 else GL_CCW)
            glUniform4f(self.uniforms['baseColor'],*material_color(mat))
            glUniform1i(self.uniforms['isNumber'],mesh.number_indices is not None)
            for unit,key,mapping,uniform in [(0,'uv',self.material_textures,'hasBase'),
                    (1,'rmuv',self.roughmet_textures,'hasRM'),(2,'decaluv',self.decal_textures,'hasDecal')]:
                texture=mapping.get(mesh.material,0) if self.show_textures and mesh.uvs and (unit!=1 or self.show_roughmet) else 0
                glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,texture)
                glUniform1i(self.uniforms[uniform],bool(texture))
                glClientActiveTexture(GL_TEXTURE0+unit);glEnableClientState(GL_TEXTURE_COORD_ARRAY)
                glBindBuffer(GL_ARRAY_BUFFER,buf[key]);glTexCoordPointer(2,GL_FLOAT,0,ctypes.c_void_p(0))
            if translucent(mesh):
                glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE)
            else:glDisable(GL_BLEND);glDepthMask(GL_TRUE)
            if mat.decal or mesh.extras.get('edm_type')=='NumberNode':
                glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(-1.,-1.)
            else:glDisable(GL_POLYGON_OFFSET_FILL)
            glBindBuffer(GL_ARRAY_BUFFER,buf['pos']);glVertexPointer(3,GL_FLOAT,0,ctypes.c_void_p(0))
            glBindBuffer(GL_ARRAY_BUFFER,buf['nor']);glNormalPointer(GL_FLOAT,0,ctypes.c_void_p(0))
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buf['index']);glDrawElements(GL_TRIANGLES,buf['count'],GL_UNSIGNED_INT,ctypes.c_void_p(0))
            glPopMatrix()
        glBindBuffer(GL_ARRAY_BUFFER,0);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0)
        glDisableClientState(GL_VERTEX_ARRAY);glDisableClientState(GL_NORMAL_ARRAY)
        for unit in range(3):
            glActiveTexture(GL_TEXTURE0+unit);glClientActiveTexture(GL_TEXTURE0+unit)
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);glDisable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,0)
        glUseProgram(0);glActiveTexture(GL_TEXTURE0);glClientActiveTexture(GL_TEXTURE0)
        glDepthMask(GL_TRUE);glDisable(GL_BLEND)
        glDisable(GL_CULL_FACE);glFrontFace(GL_CCW)
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);self.dirty=False

    def mousePressEvent(self,event):self.last=event.position()
    def mouseMoveEvent(self,event):
        if self.last is None:return
        d=event.position()-self.last;self.last=event.position()
        if event.buttons()&Qt.MouseButton.LeftButton:
            self.yaw-=d.x()*.4;self.pitch=max(-85,min(85,self.pitch+d.y()*.4))
        elif event.buttons()&(Qt.MouseButton.RightButton|Qt.MouseButton.MiddleButton):
            yaw=math.radians(self.yaw)
            right=np.array([-math.sin(yaw),0,math.cos(yaw)])
            self.target+=right*d.x()*self.distance*.0015+np.array([0,1,0])*d.y()*self.distance*.0015
        self.update()
    def wheelEvent(self,event):
        self.distance=max(.001,min(self.radius*500,self.distance*math.exp(-event.angleDelta().y()*.001)))
        self.update()
