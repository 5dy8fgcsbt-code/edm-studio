"""Exercise the app's actual number controls and RoughMet toggle, then grab its UI."""
import sys,json,time,argparse
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))

def main():
    from PySide6.QtWidgets import QApplication
    from PySide6.QtGui import QSurfaceFormat
    from PySide6.QtCore import QTimer,QSettings
    from edm_studio.app import Window,STYLE
    p=argparse.ArgumentParser();p.add_argument('model');p.add_argument('--livery');p.add_argument('--output',default='validation/material-ui')
    options=p.parse_args();out=Path(options.output).resolve();out.mkdir(parents=True,exist_ok=True)
    fmt=QSurfaceFormat();fmt.setVersion(2,1);fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile)
    fmt.setDepthBufferSize(24);fmt.setSamples(4);QSurfaceFormat.setDefaultFormat(fmt)
    app=QApplication(sys.argv);app.setStyleSheet(STYLE)
    w=Window();w.settings=QSettings(str(out/'settings.ini'),QSettings.Format.IniFormat);w.settings.clear();w.show()
    start=time.monotonic();state={'stage':0};report={}
    def finish(error=None):
        report.update({'error':error or w.viewport.error,'elapsed':time.monotonic()-start,'gpu':w.viewport.gpu,
                       'roughmet_materials':len(w.preview.roughmet) if w.preview else 0,
                       'lua':w.livery.evaluation if w.livery else None})
        (out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
        print(json.dumps(report,ensure_ascii=True),flush=True);app.exit(1 if report['error'] else 0)
    w.failed=finish
    def step():
        try:
            if time.monotonic()-start>120:return finish('Material UI test timed out')
            if w.scene is None or (w.worker and w.worker.isRunning()):QTimer.singleShot(250,step);return
            stage=state['stage']
            if stage==0:
                w.viewport.yaw=35;w.viewport.pitch=15;w.viewport.distance=w.viewport.radius*1.9
                w.bort.setText('408');w.bort_button.click()
                report['number_arguments']=w.scene.number_args;report['args_408']=dict(w.args)
            elif stage==1:
                w.grab().save(str(out/'number-408-pbr.png'));w.toggle_roughmet(False)
            elif stage==2:
                w.grab().save(str(out/'number-408-no-roughmet.png'));w.toggle_roughmet(True)
                w.bort.setText('123');w.bort_button.click();report['args_123']=dict(w.args)
            else:
                w.grab().save(str(out/'number-123-pbr.png'));return finish()
            state['stage']+=1;QTimer.singleShot(900,step)
        except Exception as error:finish(str(error))
    QTimer.singleShot(100,lambda:w.load_path(options.model,options.livery));QTimer.singleShot(500,step)
    return app.exec()

if __name__=='__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    raise SystemExit(main())
