"""Verify readable Qt text over the actual GPU viewport; capture own widgets."""
import argparse,json,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))

def main():
    from PySide6.QtCore import QTimer,QSettings,Qt,QPoint,QPointF
    from PySide6.QtGui import QSurfaceFormat,QMouseEvent
    from PySide6.QtWidgets import QApplication
    from edm_studio.app import Window,STYLE
    parser=argparse.ArgumentParser();parser.add_argument('model');parser.add_argument('--output',default='validation/overlay-ui')
    opts=parser.parse_args();out=Path(opts.output).resolve();out.mkdir(parents=True,exist_ok=True)
    fmt=QSurfaceFormat();fmt.setVersion(2,1);fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile)
    fmt.setDepthBufferSize(24);fmt.setSamples(4);QSurfaceFormat.setDefaultFormat(fmt)
    app=QApplication(sys.argv);app.setStyleSheet(STYLE);w=Window()
    w.settings=QSettings(str(out/'settings.ini'),QSettings.Format.IniFormat);w.settings.clear();w.show()
    state={'stage':0};report={'states':[]};start=time.monotonic()
    def finish(error=None):
        report.update(gpu=w.viewport.gpu,error=error,elapsed=time.monotonic()-start)
        (out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
        print(json.dumps(report,ensure_ascii=True),flush=True);app.exit(1 if error else 0)
    w.failed=finish
    def capture(name):
        v=w.viewport;v.grab().save(str(out/(name+'.png')))
        report['states'].append({'name':name,'text':v.overlay.text(),'error':v.error,'size':[v.width(),v.height()]})
    def step():
        try:
            if time.monotonic()-start>100:return finish('timeout')
            v=w.viewport
            if state['stage']==0:
                capture('empty');w.load_path(opts.model)
            elif state['stage']==1:
                if not w.scene or w.busy.isVisible():QTimer.singleShot(250,step);return
                assert not v.error,v.error
                capture('loaded');before=v.yaw
                point=QPoint(30,v.height()-25)
                assert w.childAt(v.mapTo(w,point)) is v,'Overlay intercepted mouse hit testing'
                def mouse(kind,x,button,buttons):
                    local=QPointF(x,point.y());global_pos=QPointF(v.mapToGlobal(local.toPoint()))
                    QApplication.sendEvent(v,QMouseEvent(kind,local,global_pos,button,buttons,Qt.KeyboardModifier.NoModifier))
                mouse(QMouseEvent.Type.MouseButtonPress,point.x(),Qt.MouseButton.LeftButton,Qt.MouseButton.LeftButton)
                mouse(QMouseEvent.Type.MouseMove,point.x()+40,Qt.MouseButton.NoButton,Qt.MouseButton.LeftButton)
                mouse(QMouseEvent.Type.MouseButtonRelease,point.x()+40,Qt.MouseButton.LeftButton,Qt.MouseButton.NoButton)
                assert v.yaw!=before,'Camera drag did not reach viewport'
                report['camera_drag_passed']=True
                v.wire=True;v.distance*=.75;v.update();w.resize(1600,1000)
            elif state['stage']==2:
                assert not v.error,v.error
                capture('resized-wire');v.wire=False;v.error='测试错误提示';v.update()
            elif state['stage']==3:
                capture('error');v.error=None;v.update()
            else:
                capture('recovered');assert not v.error,v.error;return finish()
            state['stage']+=1;QTimer.singleShot(650,step)
        except Exception as exc:finish(str(exc))
    QTimer.singleShot(500,step)
    return app.exec()

if __name__=='__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    raise SystemExit(main())
