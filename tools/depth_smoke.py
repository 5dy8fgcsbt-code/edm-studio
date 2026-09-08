"""Reproduce and compare F-14 winding/depth fixes in the actual GPU viewport."""
import sys,json,time,argparse
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))

def main():
    import numpy as np
    from PySide6.QtWidgets import QApplication
    from PySide6.QtGui import QSurfaceFormat
    from PySide6.QtCore import QTimer,QSettings
    from edm_studio.app import Window,STYLE
    from edm_studio import viewer
    p=argparse.ArgumentParser();p.add_argument('model');p.add_argument('--output',default='validation/depth-wing')
    opt=p.parse_args();out=Path(opt.output).resolve();out.mkdir(parents=True,exist_ok=True)
    fmt=QSurfaceFormat();fmt.setVersion(2,1);fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile)
    fmt.setDepthBufferSize(24);fmt.setSamples(4);QSurfaceFormat.setDefaultFormat(fmt)
    app=QApplication(sys.argv);app.setStyleSheet(STYLE);w=Window();w.settings=QSettings(str(out/'settings.ini'),QSettings.Format.IniFormat)
    w.settings.clear();w.show();original=viewer.gluPerspective
    state={'stage':0,'fix':False};report={};start=time.monotonic()
    def projection(fovy,aspect,near,far):
        if not state['fix']:near=max(.001,w.viewport.distance-w.viewport.radius*1.8);far=max(100,w.viewport.distance+w.viewport.radius*5)
        report['fixed_projection' if state['fix'] else 'original_projection']={'near':near,'far':far,'distance':w.viewport.distance,'radius':w.viewport.radius}
        return original(fovy,aspect,near,far)
    viewer.gluPerspective=projection
    def finish(error=None):
        report['error']=error or w.viewport.error;report['gpu']=w.viewport.gpu
        (out/'result.json').write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report),flush=True)
        app.exit(1 if report['error'] else 0)
    w.failed=finish
    def camera():
        w.viewport.yaw=155;w.viewport.pitch=20;w.viewport.target=np.array([-.7,.7,5.2]);w.viewport.distance=12
        w.viewport.update()
    def step():
        if time.monotonic()-start>100:return finish('timeout')
        if not w.scene or (w.worker and w.worker.isRunning()):QTimer.singleShot(250,step);return
        if state['stage']==0:
            state['normalized']=[m for m in w.scene.meshes if m.extras.get('edm_winding_reversed')]
            report['normalized_meshes']=len(state['normalized'])
            for m in state['normalized']:m.indices=np.ascontiguousarray(m.indices.reshape(-1,3)[:,[0,2,1]].ravel())
            w.viewport.set_scene(w.scene);w.viewport.set_textures(w.preview);camera()
        elif state['stage']==1:
            w.viewport.grabFramebuffer().save(str(out/'before.png'));state['fix']=True
            for m in state['normalized']:m.indices=np.ascontiguousarray(m.indices.reshape(-1,3)[:,[0,2,1]].ravel())
            w.viewport.set_scene(w.scene);w.viewport.set_textures(w.preview);camera()
        elif state['stage']==2:
            w.viewport.grabFramebuffer().save(str(out/'after.png'));return finish()
        w.viewport.update();state['stage']+=1;QTimer.singleShot(800,step)
    QTimer.singleShot(100,lambda:w.load_path(opt.model));QTimer.singleShot(500,step)
    return app.exec()

if __name__=='__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    raise SystemExit(main())
