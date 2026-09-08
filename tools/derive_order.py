from native_oracle import *
import itertools
o=Oracle(); n=t.ArgAnimationNode(type='ArgAnimationNode',name='test',version=0)
rng=np.random.default_rng(8)
def wq():
 q=quat(rng.normal(size=4)); return (q[3],*q[:3])
def sq(q,s):
 r=rotation(q); return r.T@scaling(s)@r
n.base.matrix=tuple((translation([3,2,1])@rotation(xyzw(wq()))).T.ravel())
n.base.position=(1,2,3); n.base.quat1=wq(); n.base.quat2=wq(); n.base.scale=(2,3,4)
n.rot_data=[(1,[t.AnimatedKey(0,wq())]),(2,[t.AnimatedKey(0,wq())])]
n.pos_data=[(4,[t.AnimatedKey(0,(.1,.2,.3))])]
n.scale_data=[(5,([t.AnimatedKey(0,tuple(xyzw(wq())))],[t.AnimatedKey(0,(1.1,.8,.9))])),(6,([t.AnimatedKey(0,tuple(xyzw(wq())))],[t.AnimatedKey(0,(.6,.7,.9))]))]
actual=o.evaluate(n,{})
pre=matrix(n.base.matrix)@translation(np.array(n.base.position)+[.1,.2,.3])
rot=[rotation(xyzw(n.base.quat1))]+[rotation(xyzw(k[0].value)) for _,k in n.rot_data]
scales=[sq(xyzw(n.base.quat2),n.base.scale)]+[sq(k[0][0].value,k[1][0].value) for _,k in n.scale_data]
for rp in itertools.permutations(range(3)):
 for sp in itertools.permutations(range(3)):
  m=pre.copy()
  for i in rp:m=m@rot[i]
  for i in sp:m=m@scales[i]
  e=np.max(np.abs(actual-m))
  if e<1e-9:print('MATCH rotation',rp,'scale',sp,'error',e)
