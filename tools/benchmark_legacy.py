"""Development-only benchmark of the unchanged Python/Qt 0.3.4 viewport."""
import argparse,json,time
from pathlib import Path
import numpy as np
from PySide6.QtCore import QTimer
from PySide6.QtGui import QSurfaceFormat
from PySide6.QtWidgets import QApplication
from OpenGL.GL import glFinish
from edm_studio.scene import load_scene
from edm_studio.textures import preview_textures
from edm_studio.viewer import Viewport

ap=argparse.ArgumentParser();ap.add_argument('model');ap.add_argument('--output',required=True);ap.add_argument('--frames',type=int,default=240);ap.add_argument('--width',type=int,default=914);ap.add_argument('--height',type=int,default=630);a=ap.parse_args()
fmt=QSurfaceFormat();fmt.setDepthBufferSize(24);fmt.setSamples(0);fmt.setSwapInterval(0);fmt.setVersion(2,1);QSurfaceFormat.setDefaultFormat(fmt)
app=QApplication([]);view=Viewport();view.resize(a.width,a.height);view.show()
def run():
    try:
        start=time.perf_counter();scene=load_scene(a.model);view.set_scene(scene);geometry=time.perf_counter()-start
        texture_start=time.perf_counter();preview=preview_textures(scene,max_size=2048);view.set_textures(preview);textures=time.perf_counter()-texture_start
        view.makeCurrent();view.paintGL();glFinish();view.doneCurrent();samples=[]
        # Include the GPU completion cost; exclude event-loop / vsync waiting.
        for frame in range(a.frames+30):
            begin=time.perf_counter();view.yaw+=.458366;view.makeCurrent();view.paintGL();glFinish();view.doneCurrent()
            if frame>=30:samples.append((time.perf_counter()-begin)*1000)
        assert not view.error,view.error
        report={'model':a.model,'viewport_pixels':[int(view.width()*view.devicePixelRatioF()),int(view.height()*view.devicePixelRatioF())],'frames':len(samples),'model_ready_seconds':geometry,'texture_load_and_upload_seconds':textures,'total_load_seconds':geometry+textures,'preview_max_size':2048,'frame_p50_ms':float(np.percentile(samples,50)),'frame_p95_ms':float(np.percentile(samples,95)),'gpu':view.gpu,'texture_count':len(preview.images),'texture_rgba_bytes':sum(w*h*4 for w,h,p in preview.images.values()),'method':'synchronous paintGL + glFinish; unchanged legacy viewport; camera-only'}
        Path(a.output).write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report,indent=2),flush=True)
    finally:app.quit()
QTimer.singleShot(300,run);app.exec()
