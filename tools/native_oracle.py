"""Optional read-only reference evaluation using the user's installed DCS DLL.

Never injects into DCS. Only calls pure math in this separate Python process.
Object-layout offsets are tied to the locally examined UniModelDesc.dll.
This tool is for validation only; the shipped application does not load DCS.
"""
import ctypes as c, os, struct, sys, hashlib
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edm_studio.math3d import *
from edm_studio.edm import types as t

class Oracle:
    def __init__(self,bin_dir=r'D:\SteamLibrary\steamapps\common\DCSWorld\bin'):
        expected='f1f03eacda2e0bf1aaa8bf2eb7167dcc0bb557deccf9d0328c5e2d2b6e9d4d73'
        if hashlib.sha256((Path(bin_dir)/'UniModelDesc.dll').read_bytes()).hexdigest()!=expected:
            raise RuntimeError('This validation oracle only supports the examined DCS DLL build; re-derive offsets before using another build.')
        self.dll_dir=os.add_dll_directory(bin_dir)
        self.dll=c.WinDLL(str(Path(bin_dir)/'UniModelDesc.dll'))
        self.fn=getattr(self.dll,'?animate@ArgAnimationNode@model@@QEBAXPEBVIModelArguments@2@AEAVMatrixd@osg@@@Z')
        self.fn.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p]; self.fn.restype=None
        self.callback=c.CFUNCTYPE(c.c_float,c.c_void_p,c.c_uint)(lambda _,i:self.args.get(i,0.))
        self.vtable=(c.c_void_p*2)(0,c.cast(self.callback,c.c_void_p).value)
        self.interface=(c.c_void_p*1)(c.addressof(self.vtable))
        self.keep=[]; self.args={}

    def evaluate(self,node,args):
        self.args=args; self.keep=[]
        obj=c.create_string_buffer(0x220)
        def write(offset,data):c.memmove(c.addressof(obj)+offset,data,len(data))
        def doubles(offset,values):write(offset,struct.pack('<'+'d'*len(values),*values))
        def vec(keys,quaternion=False):
            raw=b''.join(struct.pack('<d',k.frame)+struct.pack('<'+'d'*len(k.value),*(xyzw(k.value) if quaternion else k.value)) for k in keys)
            buf=c.create_string_buffer(raw); self.keep.append(buf)
            begin=c.addressof(buf)
            return struct.pack('<QQQ',begin,begin+len(raw),begin+len(raw))
        def tracks(offset,tracks,kind):
            raw=b''
            for arg,keys in tracks:
                raw+=struct.pack('<II',arg,0)
                if kind=='scale':raw+=vec(keys[0])+vec(keys[1])
                else:raw+=vec(keys,kind=='rotation')
            buf=c.create_string_buffer(raw); self.keep.append(buf); begin=c.addressof(buf)
            write(offset,struct.pack('<QQQ',begin,begin+len(raw),begin+len(raw)))
        b=node.base
        doubles(0xb0,b.matrix or tuple(np.eye(4).ravel()))
        tracks(0x130,node.pos_data,'translation'); tracks(0x148,node.rot_data,'rotation'); tracks(0x160,node.scale_data,'scale')
        doubles(0x178,b.position); doubles(0x190,xyzw(b.quat1)); doubles(0x1b0,xyzw(b.quat2)); doubles(0x1d0,b.scale)
        out=(c.c_double*16)(*np.eye(4).ravel())
        self.fn(c.addressof(obj),c.addressof(self.interface),c.addressof(out))
        return np.array(out).reshape(4,4).T

if __name__=='__main__':
    o=Oracle()
    n=t.ArgAnimationNode(type='ArgAnimationNode',name='oracle',version=0)
    n.base.position=(1,2,3); n.base.quat1=(.9238795325,0,0,.3826834324)
    n.base.quat2=(.9659258263,.2588190451,0,0); n.base.scale=(2,3,4)
    actual=o.evaluate(n,{})
    print('NATIVE',actual)
    q1=rotation(xyzw(n.base.quat1)); q2=rotation(xyzw(n.base.quat2)); s=scaling(n.base.scale)
    for name,m in [('Q2 S invQ2',q2@s@q2.T),('invQ2 S Q2',q2.T@s@q2)]:
        expected=translation(n.base.position)@q1@m
        print(name, 'error',np.max(np.abs(expected-actual)))
