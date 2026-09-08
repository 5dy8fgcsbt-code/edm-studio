"""Automated native-app QA. Captures this app's own widgets only."""
import sys,json,argparse
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QTimer
from PySide6.QtGui import QSurfaceFormat
from edm_studio.app import Window,STYLE
from edm_studio.scene import load_scene

options=argparse.ArgumentParser()
options.add_argument('model')
options.add_argument('--output',default='validation')
options=options.parse_args()

fmt=QSurfaceFormat();fmt.setVersion(2,1);fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile);fmt.setDepthBufferSize(24);fmt.setSamples(4)
QSurfaceFormat.setDefaultFormat(fmt)
app=QApplication(sys.argv);app.setStyleSheet(STYLE)
w=Window();w.show()
scene=load_scene(options.model);out=Path(options.output);out.mkdir(parents=True,exist_ok=True)
report={};stage=0
def capture():
    global stage
    if stage==0:
        w.loaded(scene);w.export_button.setEnabled(True)
    elif stage==1:
        w.grab().save(str(out/'ui_default.png'))
        report['gpu']=w.viewport.gpu;report['error']=w.viewport.error
        for arg in [0,3,5,38]:w.args[arg]=1.
        w.viewport.set_arguments(w.args)
    elif stage==2:
        w.grab().save(str(out/'ui_gear_canopy.png'))
        w.viewport.yaw=90;w.viewport.pitch=3;w.viewport.update()
    elif stage==3:
        w.grab().save(str(out/'ui_side.png'))
        report['mesh_count']=len(scene.meshes);report['arguments']=len(scene.limits)
        report['error']=w.viewport.error
        (out/'ui_smoke.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        app.quit();return
    stage+=1;QTimer.singleShot(1500,capture)
QTimer.singleShot(500,capture)
app.exec()
print(json.dumps(report),flush=True)
sys.exit(1 if report.get('error') else 0)
