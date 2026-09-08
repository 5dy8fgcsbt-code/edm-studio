"""Exercise the real Qt loading/selection pipeline and capture only its own widgets."""
import argparse,json,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QTimer,QSettings,Qt
from PySide6.QtGui import QSurfaceFormat
from edm_studio.app import Window,STYLE

def main():
    p=argparse.ArgumentParser();p.add_argument('model');p.add_argument('--livery',default='VF-103 Jolly Rogers Hi Viz');p.add_argument('--output',default='validation/livery-ui')
    options=p.parse_args();out=Path(options.output).resolve();out.mkdir(parents=True,exist_ok=True)
    fmt=QSurfaceFormat();fmt.setVersion(2,1);fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile);fmt.setDepthBufferSize(24);fmt.setSamples(4)
    QSurfaceFormat.setDefaultFormat(fmt);app=QApplication(sys.argv);app.setStyleSheet(STYLE)
    w=Window();w.settings=QSettings(str(out/'test-settings.ini'),QSettings.Format.IniFormat);w.settings.clear();w.show()
    state={'stage':0,'started':time.monotonic()};report={}
    def failed(message):
        report['error']=message;finish()
    def finish():
        (out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps({k:report.get(k) for k in ('gpu','catalog_count','preview_materials','gpu_textures','error','elapsed')},ensure_ascii=True),flush=True)
        app.exit(1 if report.get('error') else 0)
    w.failed=failed
    def step():
        if time.monotonic()-state['started']>120:failed('UI test timeout');return
        if w.scene is None or (w.worker and w.worker.isRunning()):QTimer.singleShot(250,step);return
        if state['stage']==0:
            report.update({'gpu':w.viewport.gpu,'catalog_count':len(w.catalog.liveries),'scan_roots':w.catalog.roots,
                           'default_materials':len(w.preview.materials)})
            w.viewport.yaw=35;w.viewport.pitch=15;w.viewport.distance=w.viewport.radius*2.3;w.viewport.update()
            selected=next((i for i in range(1,w.livery_combo.count()) if options.livery.casefold() in w.livery_combo.itemData(i).name.casefold()
                           and str(w.livery_combo.itemData(i).path).startswith(str(Path(options.model).anchor))),None)
            if selected is None:selected=next((i for i in range(1,w.livery_combo.count()) if options.livery.casefold() in w.livery_combo.itemData(i).name.casefold()),None)
            if selected is None:failed('Requested livery not discovered');return
            state['stage']=1;w.livery_combo.setCurrentIndex(selected);QTimer.singleShot(250,step);return
        if state['stage']==1:
            for arg in (0,3,5,38):
                if arg in w.scene.limits:w.args[arg]=1.
            if w.current_arg==0:w.value.setValue(1.)
            w.viewport.set_arguments(w.args);state['stage']=2;QTimer.singleShot(1800,step);return
        report.update({'livery':w.livery.metadata(),'preview_materials':len(w.preview.materials),'gpu_textures':len(w.viewport.texture_ids),
                       'missing':w.preview.missing,'error':w.viewport.error,'elapsed':time.monotonic()-state['started']})
        w.grab().save(str(out/'preview.png'));finish()
    QTimer.singleShot(100,lambda:w.load_path(options.model));QTimer.singleShot(500,step)
    raise SystemExit(app.exec())

if __name__=="__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    main()
