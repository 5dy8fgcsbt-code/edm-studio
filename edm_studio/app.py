"""EDM Studio native desktop UI."""
import sys,json,time,traceback
from pathlib import Path
from collections import Counter
from PySide6.QtCore import Qt,Signal,QThread,QTimer,QSettings
from PySide6.QtGui import QAction,QKeySequence,QSurfaceFormat,QFont
from PySide6.QtWidgets import (QApplication,QMainWindow,QWidget,QVBoxLayout,QHBoxLayout,QLabel,QPushButton,
    QFileDialog,QMessageBox,QListWidget,QListWidgetItem,QSlider,QDoubleSpinBox,QCheckBox,QLineEdit,
    QSplitter,QPlainTextEdit,QGroupBox,QComboBox,QProgressBar,QInputDialog)
from .scene import load_scene
from .gltf import export_scene
from .viewer import Viewport
from .liveries import discover_liveries,read_livery,normalized
from .textures import preview_textures
from .numbering import bort_mapping,bort_arguments

STYLE='''
QWidget { background:#111923; color:#d8e2ee; font-family:"Microsoft YaHei UI"; font-size:13px; }
QMainWindow {background:#0b1018;} QLabel#brand {font-size:25px;font-weight:650;color:#f3f7fc;}
QLabel#sub {color:#8fa4bc;} QPushButton {background:#253246;border:1px solid #35465d;border-radius:6px;padding:9px 15px;}
QPushButton:hover {background:#31445d;} QPushButton:disabled {color:#53677f;background:#1b2533;}
QPushButton#primary {background:#2d73d8;border-color:#438cef;color:white;}
QLineEdit,QDoubleSpinBox,QComboBox {background:#0b121b;border:1px solid #334156;border-radius:4px;padding:6px;}
QListWidget,QPlainTextEdit {background:#0b121b;border:1px solid #29394c;border-radius:5px;}
QListWidget::item {padding:8px 5px;} QListWidget::item:selected {background:#234775;color:#fff;}
QGroupBox {border:1px solid #2b3a4f;border-radius:6px;margin-top:12px;padding:10px;}
QGroupBox::title {subcontrol-origin:margin;left:10px;padding:0 4px;color:#94aac3;}
QSlider::groove:horizontal {height:5px;background:#2e3e55;border-radius:2px;}
QSlider::handle:horizontal {background:#67a6ff;width:15px;margin:-6px 0;border-radius:7px;}
QStatusBar {color:#8ca3bf;} QProgressBar {border:0;background:#182536;max-height:4px;} QProgressBar::chunk {background:#438cef;}
'''

class Worker(QThread):
    done=Signal(object);failed=Signal(str);progress=Signal(str)
    def __init__(self,fn):super().__init__();self.fn=fn
    def run(self):
        try:self.done.emit(self.fn(self.progress.emit))
        except Exception:self.failed.emit(traceback.format_exc())

class Window(QMainWindow):
    def __init__(self):
        super().__init__();self.setWindowTitle('EDM Studio 0.3.4 — 模型、涂装与动画');self.resize(1460,940);self.setAcceptDrops(True)
        self.scene=None;self.args={};self.worker=None;self.current_arg=None;self.playing=False;self.texture_dir=None
        self.livery=None;self.catalog=None;self.base_args={};self.preview=None;self.lua_context={}
        self.settings=QSettings('EDMStudio','EDMStudio');self.extra_roots=self.settings.value('livery_roots',[],type=list)
        container=QWidget();self.setCentralWidget(container);layout=QVBoxLayout(container);layout.setContentsMargins(20,15,20,12)
        header=QHBoxLayout();brand=QLabel('EDM Studio');brand.setObjectName('brand');header.addWidget(brand)
        sub=QLabel('DCS 模型 · 参数动画 · glTF 2.0');sub.setObjectName('sub');header.addWidget(sub);header.addStretch()
        self.open_button=QPushButton('打开 EDM');self.open_button.clicked.connect(self.open_dialog);header.addWidget(self.open_button)
        self.export_button=QPushButton('导出模型与动画');self.export_button.setObjectName('primary');self.export_button.setEnabled(False)
        self.export_button.clicked.connect(self.export_dialog);header.addWidget(self.export_button);layout.addLayout(header)
        self.file_label=QLabel('所有文件均在本机处理');self.file_label.setObjectName('sub');layout.addWidget(self.file_label)
        self.busy=QProgressBar();self.busy.setRange(0,0);self.busy.hide();layout.addWidget(self.busy)
        splitter=QSplitter();layout.addWidget(splitter,1)
        left=QWidget();ll=QVBoxLayout(left);ll.setContentsMargins(0,0,12,0)
        ll.addWidget(QLabel('动画参数'))
        self.search=QLineEdit();self.search.setPlaceholderText('搜索参数编号或部件名称');self.search.textChanged.connect(self.filter_args);ll.addWidget(self.search)
        self.arg_list=QListWidget();self.arg_list.currentItemChanged.connect(self.select_arg);ll.addWidget(self.arg_list,1)
        group=QGroupBox('播放与姿态');gl=QVBoxLayout(group)
        self.arg_label=QLabel('选择参数开始预览');gl.addWidget(self.arg_label)
        self.value=QDoubleSpinBox();self.value.setDecimals(5);self.value.setRange(-1,1);self.value.setSingleStep(.01);self.value.valueChanged.connect(self.value_changed);gl.addWidget(self.value)
        self.slider=QSlider(Qt.Orientation.Horizontal);self.slider.setRange(0,10000);self.slider.valueChanged.connect(self.slider_changed);gl.addWidget(self.slider)
        row=QHBoxLayout();self.play=QPushButton('▶ 播放');self.play.clicked.connect(self.toggle_play);row.addWidget(self.play)
        reset=QPushButton('归零');reset.clicked.connect(self.reset_args);row.addWidget(reset);gl.addLayout(row)
        hint=QLabel('参数独立控制，可叠加预览。\n归零保留涂装的默认参数。');hint.setWordWrap(True);hint.setObjectName('sub');gl.addWidget(hint);ll.addWidget(group)
        self.arg_names=QPlainTextEdit();self.arg_names.setReadOnly(True);self.arg_names.setMaximumHeight(125);ll.addWidget(self.arg_names)
        splitter.addWidget(left)
        middle=QWidget();ml=QVBoxLayout(middle);ml.setContentsMargins(0,0,0,0)
        self.viewport=Viewport();ml.addWidget(self.viewport,1)
        vr=QHBoxLayout();fit=QPushButton('适应视图');fit.clicked.connect(self.viewport.fit);vr.addWidget(fit)
        wire=QCheckBox('线框');wire.toggled.connect(self.set_wire);vr.addWidget(wire)
        self.textured=QCheckBox('显示贴图');self.textured.setChecked(True);self.textured.toggled.connect(self.toggle_textures);vr.addWidget(self.textured)
        rough=QCheckBox('RoughMet');rough.setChecked(True);rough.toggled.connect(self.toggle_roughmet);vr.addWidget(rough)
        vr.addStretch();tag=QLabel('Y 向上 · 米');tag.setObjectName('sub');vr.addWidget(tag);ml.addLayout(vr);splitter.addWidget(middle)
        right=QWidget();rl=QVBoxLayout(right);rl.setContentsMargins(12,0,0,0)
        self.stats=QLabel('尚未载入模型');self.stats.setWordWrap(True);rl.addWidget(self.stats)
        self.livery_group=QGroupBox('涂装');lg=QVBoxLayout(self.livery_group)
        self.livery_combo=QComboBox();self.livery_combo.setEditable(True);self.livery_combo.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        self.livery_combo.completer().setFilterMode(Qt.MatchFlag.MatchContains)
        self.livery_combo.setMinimumContentsLength(18);self.livery_combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.livery_combo.addItem('模型默认贴图',None);self.livery_combo.currentIndexChanged.connect(self.select_livery);lg.addWidget(self.livery_combo)
        buttons=QHBoxLayout();load=QPushButton('导入涂装…');load.clicked.connect(self.import_livery);buttons.addWidget(load)
        refresh=QPushButton('重新扫描');refresh.clicked.connect(self.rescan_liveries);buttons.addWidget(refresh);lg.addLayout(buttons)
        add=QPushButton('添加扫描目录…');add.clicked.connect(self.add_livery_root);lg.addWidget(add)
        self.livery_status=QLabel('自动检测保存的游戏和 DCS 本体');self.livery_status.setWordWrap(True);self.livery_status.setObjectName('sub');lg.addWidget(self.livery_status)
        nr=QHBoxLayout();nr.addWidget(QLabel('机身编号'))
        self.bort=QLineEdit();self.bort.setPlaceholderText('例如 408');self.bort.setMaxLength(6);nr.addWidget(self.bort)
        self.bort_button=QPushButton('应用');self.bort_button.clicked.connect(self.apply_bort);nr.addWidget(self.bort_button);lg.addLayout(nr)
        self.number_status=QLabel('模型载入后检测编号参数');self.number_status.setWordWrap(True);self.number_status.setObjectName('sub');lg.addWidget(self.number_status)
        context=QPushButton('Lua 变量 / 重新求值…');context.clicked.connect(self.configure_lua);lg.addWidget(context)
        rl.addWidget(self.livery_group)
        eg=QGroupBox('导出设置');el=QVBoxLayout(eg)
        self.export_mode=QComboBox();self.export_mode.addItems(['导出全部参数动画','仅导出当前参数动画','仅导出默认姿态的静态模型']);el.addWidget(self.export_mode)
        el.addWidget(QLabel('每个动画片段时长（秒）'))
        self.duration=QDoubleSpinBox();self.duration.setRange(.1,120);self.duration.setValue(3);self.duration.setSingleStep(.5);el.addWidget(self.duration)
        self.embed=QCheckBox('嵌入贴图、RoughMet 与编号');self.embed.setChecked(True);el.addWidget(self.embed)
        tex=QPushButton('指定贴图目录');tex.clicked.connect(self.choose_textures);el.addWidget(tex)
        self.tex_label=QLabel('预览与导出使用同一涂装');self.tex_label.setWordWrap(True);self.tex_label.setObjectName('sub');el.addWidget(self.tex_label)
        explain=QLabel('GLB 为单文件，适合 Blender、引擎和通用查看器。参数原始范围会保留；负值不会被截断。');explain.setWordWrap(True);explain.setObjectName('sub');el.addWidget(explain);rl.addWidget(eg)
        rl.addWidget(QLabel('读取与导出报告'))
        self.log=QPlainTextEdit();self.log.setReadOnly(True);rl.addWidget(self.log,1);splitter.addWidget(right)
        splitter.setSizes([260,880,310]);splitter.setCollapsible(0,False);splitter.setCollapsible(2,False)
        for text,key,fn in [('打开','Ctrl+O',self.open_dialog),('导出','Ctrl+E',self.export_dialog),('视角','F',self.viewport.fit)]:
            action=QAction(text,self);action.setShortcut(QKeySequence(key));action.triggered.connect(fn);self.addAction(action)
        self.timer=QTimer();self.timer.setInterval(33);self.timer.timeout.connect(self.tick)
        self.statusBar().showMessage('就绪 · 拖入 EDM 文件即可打开')

    def run_job(self,fn,done):
        if self.worker and self.worker.isRunning():return
        self.stop_play();self.busy.show();self.open_button.setEnabled(False);self.export_button.setEnabled(False)
        self.livery_group.setEnabled(False)
        self.worker=Worker(fn);self.worker.progress.connect(self.statusBar().showMessage)
        self.worker.done.connect(done);self.worker.failed.connect(self.failed);self.worker.finished.connect(self.job_finished);self.worker.start()

    def job_finished(self):
        self.busy.hide();self.open_button.setEnabled(True);self.export_button.setEnabled(self.scene is not None)
        self.livery_group.setEnabled(self.scene is not None)

    def failed(self,message):
        if self.catalog:self.fill_liveries(self.livery)
        self.log.appendPlainText(message);self.statusBar().showMessage('操作失败，详见报告')
        QMessageBox.warning(self,'无法完成操作',message.splitlines()[-1]+'\n\n详细信息已写入右侧报告。')

    def open_dialog(self):
        path,_=QFileDialog.getOpenFileName(self,'打开 EDM',str(self.scene.source.parent) if self.scene else '', 'DCS EDM (*.edm *.EDM)')
        if path:self.load_path(path)

    def load_path(self,path,livery_override=None,lua_context=None):
        self.statusBar().showMessage('正在解析 EDM…')
        extra=list(self.extra_roots);last=self.settings.value('selected_livery/'+normalized(Path(path).stem),'')
        def prepare(progress):
            scene=load_scene(path);catalog=discover_liveries(path,extra,progress=progress)
            selected=read_livery(livery_override,context=lua_context) if livery_override else next((l for l in catalog.liveries if l.identifier==last),None)
            if selected and not any(l.identifier==selected.identifier for l in catalog.liveries):catalog.liveries.insert(0,selected)
            preview=preview_textures(scene,self.texture_dir,selected,catalog.installations,progress)
            return scene,catalog,selected,preview
        self.run_job(prepare,self.loaded_bundle)

    def loaded_bundle(self,result):
        scene,self.catalog,selected,preview=result
        self.loaded(scene);self.lua_context=dict(selected.evaluation.get('context',{})) if selected else {}
        self.fill_liveries(selected);self.applied_livery((selected,preview),remember=False)
        if self.catalog.warnings:self.log.appendPlainText('\n扫描提示：\n'+'\n'.join(self.catalog.warnings[:15]))

    def fill_liveries(self,selected=None):
        self.livery_combo.blockSignals(True);self.livery_combo.clear();self.livery_combo.addItem('模型默认贴图',None)
        index=0
        for livery in self.catalog.liveries:
            self.livery_combo.addItem(livery.name+'  ['+livery.origin+']',livery)
            row=self.livery_combo.count()-1;self.livery_combo.setItemData(row,livery.identifier,Qt.ItemDataRole.ToolTipRole)
            if selected and livery.identifier==selected.identifier:index=row
        self.livery_combo.setCurrentIndex(index);self.livery_combo.blockSignals(False)
        self.livery_combo.lineEdit().setCursorPosition(0)
        self.livery_status.setText(f'发现 {len(self.catalog.liveries)} 个涂装 · 输入名称可搜索')
        self.livery_status.setToolTip('\n'.join(self.catalog.roots))

    def select_livery(self,index):
        if self.scene is None or index<0:return
        self.apply_livery(self.livery_combo.itemData(index))

    def apply_livery(self,selected):
        context=dict(self.lua_context)
        def prepare(progress):
            evaluated=read_livery(selected,context=context) if selected else None
            return evaluated,preview_textures(self.scene,self.texture_dir,evaluated,
                    self.catalog.installations if self.catalog else None,progress)
        self.run_job(prepare,self.applied_livery)

    def toggle_roughmet(self,value):
        self.viewport.show_roughmet=value;self.viewport.update()

    def apply_bort(self):
        if not self.scene:return
        try:values=bort_arguments(self.scene,self.bort.text().strip())
        except ValueError as error:
            self.number_status.setText(str(error));return
        self.base_args.update(values);self.args.update(values);self.viewport.set_arguments(self.args)
        self.value.blockSignals(True);self.value.setValue(self.args.get(self.current_arg,0.));self.value.blockSignals(False);self.sync_slider()
        self.number_status.setText('已应用：'+', '.join(f'参数 {k} = {v:g}' for k,v in values.items()))

    def configure_lua(self):
        text,ok=QInputDialog.getMultiLineText(self,'动态 Lua 配置','输入脚本需要的全局变量（JSON 对象），例如 {"country":"USA"}。\n确认后重新读取 description.lua 和引用文件。',json.dumps(self.lua_context,ensure_ascii=False,indent=2))
        if not ok:return
        try:
            context=json.loads(text)
            if not isinstance(context,dict):raise ValueError('需要 JSON 对象')
        except (ValueError,OSError) as error:
            QMessageBox.warning(self,'Lua 配置',str(error));return
        self.lua_context=context
        if self.livery:self.apply_livery(self.livery)
        else:self.statusBar().showMessage('Lua 变量已设置，下次导入涂装时使用。')

    def applied_livery(self,result,remember=True):
        self.livery,self.preview=result;self.base_args=dict(self.livery.custom_args) if self.livery else {}
        self.viewport.set_textures(self.preview);self.reset_args()
        order=bort_mapping(self.scene);self.bort.setEnabled(bool(order));self.bort_button.setEnabled(bool(order))
        self.bort.setText(''.join(str(max(0,min(9,round(self.args.get(a,0.)*10)))) for a in order))
        self.number_status.setText('从左到右：'+', '.join(str(a) for a in order) if order else
                                  ('编号参数：'+', '.join(map(str,self.scene.number_args)) if self.scene.number_args else '此模型没有 NumberNode 动态编号；涂装中印刷的编号会原样保留。'))
        self.log.appendPlainText(f'RoughMet：{len(self.preview.roughmet)} 个材质；编号贴图：{len(self.preview.decals)} 个材质。')
        if self.livery:self.log.appendPlainText('Lua：'+self.livery.evaluation.get('engine','')+'；引用 '+str(len(self.livery.evaluation.get('includes',[])))+' 个脚本。')
        title=self.livery.name if self.livery else '模型默认贴图'
        self.livery_combo.lineEdit().setCursorPosition(0);self.livery_combo.setToolTip(title+'\n'+(self.livery.identifier if self.livery else ''))
        self.livery_status.setText(f'{len(self.catalog.liveries) if self.catalog else 0} 个可选涂装 · {len(self.preview.materials)} 个材质已贴图')
        self.log.appendPlainText('\n涂装：'+title+f'\n已应用 {len(self.preview.materials)} 个材质贴图。')
        if self.preview.missing:self.log.appendPlainText('未找到：\n'+'\n'.join(self.preview.missing))
        warnings=self.preview.warnings+(self.livery.warnings if self.livery else [])
        if warnings:self.log.appendPlainText('\n'.join(warnings))
        self.statusBar().showMessage('涂装已加载 · '+title)
        if remember:self.settings.setValue('selected_livery/'+normalized(self.scene.source.stem),self.livery.identifier if self.livery else '')

    def import_livery(self):
        if self.scene is None:return
        path,_=QFileDialog.getOpenFileName(self,'选择涂装 ZIP 或涂装文件夹里的 description.lua','','DCS 涂装 (*.zip *.lua)')
        if not path:return
        try:selected=read_livery(path,context=self.lua_context)
        except Exception as error:self.failed(str(error));return
        if not any(l.identifier==selected.identifier for l in self.catalog.liveries):self.catalog.liveries.insert(0,selected)
        self.fill_liveries(selected);self.apply_livery(selected)

    def rescan_liveries(self):
        if self.scene is None:return
        def done(catalog):
            self.catalog=catalog
            if self.livery and not any(l.identifier==self.livery.identifier for l in catalog.liveries):catalog.liveries.insert(0,self.livery)
            self.fill_liveries(self.livery)
            self.log.appendPlainText(f'\n扫描完成：{len(catalog.liveries)} 个涂装，{len(catalog.roots)} 个扫描目录。')
            if catalog.warnings:self.log.appendPlainText('\n'.join(catalog.warnings[:15]))
        self.run_job(lambda progress:discover_liveries(self.scene.source,self.extra_roots,progress=progress),done)

    def add_livery_root(self):
        root=QFileDialog.getExistingDirectory(self,'添加保存的游戏、DCS 安装目录或 Liveries 目录')
        if root and root not in self.extra_roots:
            self.extra_roots.append(root);self.settings.setValue('livery_roots',self.extra_roots);self.rescan_liveries()

    def toggle_textures(self,value):self.viewport.show_textures=value;self.viewport.update()

    def loaded(self,scene):
        self.scene=scene;self.args={};self.base_args={};self.livery=None;self.current_arg=None;self.lua_context={}
        self.viewport.set_scene(scene);self.file_label.setText(str(scene.source));self.arg_list.clear()
        counts=Counter(tr.argument for tr in scene.tracks)
        for arg,(lo,hi) in scene.limits.items():
            item=QListWidgetItem(f'参数 {arg:03d}    {lo:g} → {hi:g}');item.setData(Qt.ItemDataRole.UserRole,arg)
            names=sorted(set(scene.nodes[tr.node]['name'].split(' / ')[0] for tr in scene.tracks if tr.argument==arg))
            if arg in scene.number_args:names.insert(0,'动态编号 / NumberNode')
            item.setData(Qt.ItemDataRole.UserRole+1,' '.join(names));item.setToolTip('\n'.join(names[:30]));self.arg_list.addItem(item)
        self.stats.setText(f'EDM v{scene.edm.version}\n{len(scene.meshes):,} 个网格 · {sum(len(m.indices)//3 for m in scene.meshes):,} 个三角面\n{len(scene.limits)} 个动画参数 · {sum(m.joints is not None for m in scene.meshes)} 个蒙皮网格')
        self.log.setPlainText('读取完成：所有字节已解析。\n\n'+'\n'.join(scene.warnings or ['没有检测到转换限制。']))
        self.log.appendPlainText('\n支持涂装、RoughMet 和 NumberNode 编号。编号片段按 0.1 参数步长导出；法线贴图与 DCS 专用光效未复现。')
        if self.arg_list.count():self.arg_list.setCurrentRow(0)
        self.statusBar().showMessage('已打开 · GPU: '+self.viewport.gpu)

    def select_arg(self,current,previous=None):
        if current is None:return
        self.stop_play();self.current_arg=current.data(Qt.ItemDataRole.UserRole)
        lo,hi=self.scene.limits[self.current_arg];self.value.blockSignals(True)
        self.value.setRange(min(lo,-10) if self.current_arg in self.scene.number_args else min(lo,0),
                            max(hi,10) if self.current_arg in self.scene.number_args else max(hi,0))
        self.value.setValue(self.args.get(self.current_arg,0));self.value.blockSignals(False)
        self.arg_label.setText(f'参数 {self.current_arg}  ·  原始范围 {lo:g} 至 {hi:g}')
        self.arg_names.setPlainText(current.data(Qt.ItemDataRole.UserRole+1))
        self.sync_slider()

    def filter_args(self,text):
        for i in range(self.arg_list.count()):
            item=self.arg_list.item(i);item.setHidden(text.lower() not in (item.text()+' '+item.data(Qt.ItemDataRole.UserRole+1)).lower())

    def value_changed(self,v):
        if self.scene is None or self.current_arg is None:return
        self.args[self.current_arg]=float(v);self.sync_slider();self.viewport.set_arguments(self.args)

    def sync_slider(self):
        if self.current_arg is None:return
        lo,hi=self.scene.limits[self.current_arg]
        self.slider.blockSignals(True);self.slider.setValue(round((self.value.value()-lo)/(hi-lo)*10000) if hi>lo else 0);self.slider.blockSignals(False)

    def slider_changed(self,v):
        if self.current_arg is None:return
        lo,hi=self.scene.limits[self.current_arg];self.value.setValue(lo+(hi-lo)*v/10000)

    def reset_args(self):
        self.stop_play();self.args=dict(self.base_args);self.value.blockSignals(True)
        self.value.setValue(self.args.get(self.current_arg,0));self.value.blockSignals(False);self.sync_slider()
        if self.scene:self.viewport.set_arguments(self.args)

    def toggle_play(self):
        if self.current_arg is None:return
        if self.playing:self.stop_play()
        else:self.playing=True;self.start_time=time.monotonic();self.play.setText('Ⅱ 暂停');self.timer.start()
    def stop_play(self):self.playing=False;self.timer.stop();self.play.setText('▶ 播放')
    def tick(self):
        lo,hi=self.scene.limits[self.current_arg];phase=((time.monotonic()-self.start_time)/self.duration.value())%2
        self.value.setValue(lo+(hi-lo)*(phase if phase<1 else 2-phase))
    def set_wire(self,value):self.viewport.wire=value;self.viewport.update()
    def choose_textures(self):
        path=QFileDialog.getExistingDirectory(self,'选择贴图目录')
        if path:
            self.texture_dir=path;self.tex_label.setText(path)
            if self.scene:self.apply_livery(self.livery)
    def export_dialog(self):
        if self.scene is None:return
        base=Path(sys.executable).parent if getattr(sys,'frozen',False) else Path(__file__).resolve().parents[1]
        path,_=QFileDialog.getSaveFileName(self,'导出模型与动画',str(base/(self.scene.source.stem+'.glb')),'GLB 模型 (*.glb);;glTF 模型 (*.gltf)')
        if not path:return
        mode=self.export_mode.currentIndex();args=None if mode==0 else [self.current_arg] if mode==1 and self.current_arg is not None else []
        baseline={**self.base_args,**{a:self.args.get(a,0.) for a in self.scene.number_args}}
        options=dict(arguments=args,duration=self.duration.value(),textures=self.texture_dir,embed_textures=self.embed.isChecked(),livery=self.livery,baseline_args=baseline)
        self.run_job(lambda progress: (path,export_scene(self.scene,path,progress=progress,**options)),self.exported)
    def exported(self,result):
        path,report=result;self.log.appendPlainText(f'\n导出完成：{path}\n{len(report["exported_arguments"])} 个动画片段\n{report["embedded_diffuse_materials"]} 个材质已嵌入贴图')
        if report['missing_textures']:self.log.appendPlainText('未找到贴图：'+', '.join(report['missing_textures']))
        self.statusBar().showMessage('导出完成 · '+path)
        QMessageBox.information(self,'导出完成',path+'\n\n动画与转换限制记录在同名 .report.json 文件中。')
    def dragEnterEvent(self,event):
        if any(u.isLocalFile() and u.toLocalFile().lower().endswith('.edm') for u in event.mimeData().urls()):event.acceptProposedAction()
    def dropEvent(self,event):
        for u in event.mimeData().urls():
            if u.isLocalFile() and u.toLocalFile().lower().endswith('.edm'):self.load_path(u.toLocalFile());break
    def closeEvent(self,event):
        if self.worker and self.worker.isRunning():
            QMessageBox.information(self,'正在处理','文件处理完成后可关闭窗口。');event.ignore()
        else:event.accept()

def main():
    fmt=QSurfaceFormat();fmt.setVersion(2,1);fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile);fmt.setDepthBufferSize(24);fmt.setSamples(4)
    QSurfaceFormat.setDefaultFormat(fmt)
    app=QApplication(sys.argv);app.setStyleSheet(STYLE);app.setFont(QFont('Microsoft YaHei UI',10))
    window=Window();window.show()
    livery_arg=sys.argv[sys.argv.index('--livery')+1] if '--livery' in sys.argv else None
    context=json.loads(sys.argv[sys.argv.index('--lua-context')+1]) if '--lua-context' in sys.argv else None
    if len(sys.argv)>1 and sys.argv[1].lower().endswith('.edm'):QTimer.singleShot(250,lambda:window.load_path(sys.argv[1],livery_arg,context))
    if '--diagnostic-output' in sys.argv:
        output=Path(sys.argv[sys.argv.index('--diagnostic-output')+1]);deadline=time.monotonic()+120;number_applied=False
        def diagnostic():
            nonlocal number_applied
            pending=window.scene is None or window.busy.isVisible()
            if pending and time.monotonic()<deadline:
                QTimer.singleShot(250,diagnostic);return
            if not pending and '--bort' in sys.argv and not number_applied:
                window.bort.setText(sys.argv[sys.argv.index('--bort')+1]);window.apply_bort();number_applied=True
                QTimer.singleShot(500,diagnostic);return
            result={'frozen':bool(getattr(sys,'frozen',False)),'gpu':window.viewport.gpu,'viewport_error':window.viewport.error,
                    'loaded':window.scene is not None,'ready':not pending}
            if window.scene:
                result.update({'meshes':len(window.scene.meshes),'arguments':len(window.scene.limits),
                               'winding_normalized_meshes':window.scene.summary()['winding_normalized_meshes'],
                               'livery':window.livery.name if window.livery else None,
                               'catalog_count':len(window.catalog.liveries) if window.catalog else 0,
                               'preview_materials':len(window.preview.materials) if window.preview else 0,
                               'roughmet_materials':len(window.preview.roughmet) if window.preview else 0,
                               'number_arguments':window.scene.number_args,'baseline_arguments':window.base_args,
                               'lua':window.livery.evaluation if window.livery else None})
                output.parent.mkdir(parents=True,exist_ok=True)
                window.grab().save(str(output.with_suffix('.png')))
                export_scene(window.scene,output.with_suffix('.glb'),arguments=list(window.scene.limits)[:1],embed_textures=False,livery=window.livery,baseline_args=window.base_args)
                result['exported']=True
            output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(result,indent=2),encoding='utf-8')
            app.exit(0 if result['ready'] and not result['viewport_error'] else 1)
        QTimer.singleShot(1500,diagnostic)
    return app.exec()
