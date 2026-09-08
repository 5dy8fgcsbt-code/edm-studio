"""Double precision math, column vectors, xyzw quaternions."""
import math
import numpy as np

IDENTITY_Q = np.array([0., 0., 0., 1.])

def projection_planes(distance,radius):
    # Orbit zoom must not collapse the near plane to 1 mm: at aircraft scale
    # that makes adjacent skin/interior surfaces indistinguishable in 24-bit Z.
    near=max(1e-5,radius*1e-5,distance*.005,distance-radius*1.2)
    far=max(near*2,distance+radius*5)
    return near,far

def matrix(values):
    return np.array(values, dtype=np.float64).reshape(4,4).T.copy() if len(values) else np.eye(4)

def translation(v):
    m=np.eye(4); m[:3,3]=v; return m

def scaling(v):
    return np.diag([*v,1.])

def quat(q):
    q=np.asarray(q,dtype=np.float64)
    norm=np.linalg.norm(q)
    if not np.isfinite(norm) or norm<1e-12: raise ValueError('Invalid quaternion')
    return q/norm

def xyzw(wxyz): return np.array([*wxyz[1:],wxyz[0]],dtype=np.float64)

def inverse(q):
    q=quat(q).copy(); q[:3]*=-1; return q

def multiply(a,b):
    a=np.asarray(a); b=np.asarray(b)
    return np.r_[a[3]*b[:3]+b[3]*a[:3]+np.cross(a[:3],b[:3]),a[3]*b[3]-a[:3]@b[:3]]

def rotation(q):
    x,y,z,w=quat(q)
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),0],
                     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),0],
                     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),0],
                     [0,0,0,1]],dtype=np.float64)

def slerp(a,b,t):
    a=quat(a); b=quat(b); dot=float(a@b)
    if dot<0: b=-b; dot=-dot
    dot=min(1.,dot)
    if dot>.9995:return quat(a+(b-a)*t)
    angle=math.acos(dot)
    return (math.sin((1-t)*angle)*a+math.sin(t*angle)*b)/math.sin(angle)

def sample(keys,value,quaternion=False,stored_wxyz=True):
    if not keys: return IDENTITY_Q.copy() if quaternion else np.zeros(3)
    values=[k.frame for k in keys]
    idx=int(np.searchsorted(values,value,side='right'))
    a=keys[max(0,idx-1)]; b=keys[min(len(keys)-1,idx)]
    av=xyzw(a.value) if quaternion and stored_wxyz else np.asarray(a.value,dtype=float)
    bv=xyzw(b.value) if quaternion and stored_wxyz else np.asarray(b.value,dtype=float)
    t=0. if a.frame==b.frame else min(1.,max(0.,(value-a.frame)/(b.frame-a.frame)))
    return slerp(av,bv,t) if quaternion else av*(1-t)+bv*t
