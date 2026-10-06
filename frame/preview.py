import struct, numpy as np, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
def load(p):
    with open(p,"rb") as f:
        f.read(80); n=struct.unpack("<I",f.read(4))[0]
        d=np.frombuffer(f.read(),dtype=[("n","<3f4"),("v","<9f4"),("a","<u2")],count=n)
    return d["v"].reshape(-1,3,3).astype(float)
views=[("frame_back.stl",-90,90,"Part B from the BACK: enclosure, PCB guides, ribbon slot, cable exit"),
       ("frame_back.stl",26,-66,"Part B oblique: 20 mm box + 4 lid snap posts"),
       ("frame_lid.stl",-90,90,"Part C lid: 4 snap holes")]
fig=plt.figure(figsize=(19,6.5),dpi=110)
for i,(name,elev,azim,title) in enumerate(views):
    t=load(name); ax=fig.add_subplot(1,3,i+1,projection="3d")
    nrm=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]); nrm/=np.linalg.norm(nrm,axis=1,keepdims=True)+1e-12
    sh=0.55+0.45*np.abs(nrm[:,2])
    ax.add_collection3d(Poly3DCollection(t,facecolors=plt.cm.Greys(0.34*sh+0.16),edgecolors="none"))
    ax.set_xlim(-95,95); ax.set_ylim(-95,95); ax.set_zlim(-95,95)
    ax.view_init(elev=elev,azim=azim); ax.set_axis_off(); ax.set_title(title,fontsize=9)
plt.tight_layout(); plt.savefig("preview.png"); print("preview.png written")
