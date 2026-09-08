"""DCS livery data and discovery in installed modules and Saved Games profiles."""
from dataclasses import dataclass,field
from pathlib import Path,PurePosixPath
from functools import lru_cache
import ctypes
import math
import os
import re
import uuid
import zipfile
from .lua_data import LuaDataError
from .lua_runtime import evaluate_config

SAVED_GAMES_ID='4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4'


@dataclass(frozen=True)
class TextureOverride:
    material: str
    slot: int
    name: str
    common: bool


@dataclass
class Livery:
    path: Path
    entry: str|None=None
    name: str=''
    unit: str=''
    origin: str='手动选择'
    textures: dict=field(default_factory=dict)
    custom_args: dict=field(default_factory=dict)
    countries: list=field(default_factory=list)
    warnings: list=field(default_factory=list)
    evaluation: dict=field(default_factory=dict)

    @property
    def identifier(self):return str(self.path)+(('::'+self.entry) if self.entry else '')
    @property
    def folder(self):return self.path.parent
    def override(self,material,slot=0):return self.textures.get((material.casefold(),slot))
    def metadata(self):
        return {'name':self.name,'source':self.identifier,'unit':self.unit,'origin':self.origin,
                'custom_args':self.custom_args,'countries':self.countries,'warnings':self.warnings,'lua':self.evaluation,
                'textures':[vars(t) for t in self.textures.values()]}


def decode_lua(raw):
    for encoding in ('utf-8-sig','cp1251'):
        try:return raw.decode(encoding)
        except UnicodeDecodeError:pass
    return raw.decode('utf-8',errors='replace')


def read_livery(path,entry=None,origin='手动选择',unit='',context=None):
    if isinstance(path,Livery):
        if context is None:return path
        path,entry,origin,unit=path.path,path.entry,path.origin,path.unit
    if entry is None and '::' in str(path):path,entry=str(path).split('::',1)
    path=Path(path)
    if path.is_dir():
        path=next((p for p in path.iterdir() if p.name.casefold()=='description.lua'),path/'description.lua')
    if path.suffix.casefold()=='.zip':
        with zipfile.ZipFile(path) as archive:
            if entry is None:
                descriptions=[n for n in archive.namelist() if PurePosixPath(n).name.casefold()=='description.lua']
                if len(descriptions)!=1:raise LuaDataError('ZIP 中必须选择一个明确的 description.lua')
                entry=descriptions[0]
            if archive.getinfo(entry).file_size>2_000_000:raise LuaDataError('description.lua exceeds 2 MB')
            raw=archive.read(entry)
        fallback=PurePosixPath(entry).parent.name or path.stem
    else:
        if path.stat().st_size>2_000_000:raise LuaDataError('description.lua exceeds 2 MB')
        raw=path.read_bytes();fallback=path.parent.name
    env,warnings,evaluation=evaluate_config(decode_lua(raw),path,entry,context);rows=env.get('livery')
    if not isinstance(rows,dict):raise LuaDataError('没有可读取的 livery 表')
    result=Livery(path.resolve(),entry,str(env.get('name') or fallback),unit,origin,warnings=warnings,evaluation=evaluation)
    for row in rows.values():
        if not isinstance(row,dict):result.warnings.append('跳过无效涂装记录。');continue
        material,slot,name,common=(row.get(i) for i in range(1,5))
        if not isinstance(material,str) or not isinstance(name,str) or not isinstance(slot,(int,float)) or not math.isfinite(slot) or int(slot)!=slot or not isinstance(common,bool):
            result.warnings.append(f'跳过无效涂装记录：{str(row)[:140]}');continue
        result.textures[(material.casefold(),int(slot))]=TextureOverride(material,int(slot),name,common)
    args=env.get('custom_args',{})
    if isinstance(args,dict):
        for key,value in args.items():
            if isinstance(key,(int,float)) and math.isfinite(key) and int(key)==key and key>=0 and isinstance(value,(int,float)) and math.isfinite(value):
                result.custom_args[int(key)]=float(value)
    countries=env.get('countries',{})
    if isinstance(countries,dict):result.countries=[v for v in countries.values() if isinstance(v,str)]
    if rows and not result.textures:raise LuaDataError('涂装中没有有效材质贴图映射')
    return result


def unique_paths(paths):
    seen=set();out=[]
    for p in paths:
        try:
            p=Path(p).expanduser().resolve();key=str(p).casefold()
            if p.is_dir() and key not in seen:seen.add(key);out.append(p)
        except (OSError,ValueError):pass
    return out


def saved_games_paths():
    paths=[]
    if os.name=='nt':
        try:
            shell=ctypes.WinDLL('shell32');ole=ctypes.WinDLL('ole32')
            guid=(ctypes.c_byte*16).from_buffer_copy(uuid.UUID(SAVED_GAMES_ID).bytes_le)
            pointer=ctypes.c_void_p()
            shell.SHGetKnownFolderPath.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
            shell.SHGetKnownFolderPath.restype=ctypes.c_long
            ole.CoTaskMemFree.argtypes=[ctypes.c_void_p]
            if shell.SHGetKnownFolderPath(ctypes.byref(guid),0,None,ctypes.byref(pointer))==0:
                try:paths.append(Path(ctypes.wstring_at(pointer)))
                finally:ole.CoTaskMemFree(pointer)
        except (OSError,ValueError):pass
    paths.extend([Path.home()/'Saved Games',Path(os.environ.get('USERPROFILE',str(Path.home())))/'Saved Games'])
    # Also find old/multiple profiles on fixed drives without scanning unrelated trees.
    if os.name=='nt':
        kernel=ctypes.WinDLL('kernel32');kernel.GetDriveTypeW.argtypes=[ctypes.c_wchar_p]
        for letter in 'CDEFGHIJKLMNOPQRSTUVWXYZ':
            drive=Path(letter+':/')
            if kernel.GetDriveTypeW(str(drive)+'\\')!=3:continue
            try:
                paths.extend(p for p in drive.iterdir() if p.is_dir() and
                             (re.search(r'saved[ _-]*games',p.name,re.I) or p.name=='保存的游戏'))
            except OSError:pass
    return unique_paths(paths)


def is_install(path):
    return (path/'Bazar').is_dir() and ((path/'CoreMods').is_dir() or (path/'bin'/'DCS.exe').is_file())


def installation_paths(source=None):
    candidates=[]
    if source:
        candidates.extend(p for p in Path(source).resolve().parents if is_install(p))
    steam=[]
    if os.name=='nt':
        import winreg
        for hive,key in [(winreg.HKEY_CURRENT_USER,r'Software\Eagle Dynamics'),(winreg.HKEY_LOCAL_MACHINE,r'Software\Eagle Dynamics')]:
            try:
                with winreg.OpenKey(hive,key) as root:
                    for i in range(winreg.QueryInfoKey(root)[0]):
                        with winreg.OpenKey(root,winreg.EnumKey(root,i)) as game:
                            for name in ('Path','InstallPath','InstallationPath'):
                                try:candidates.append(Path(winreg.QueryValueEx(game,name)[0]))
                                except OSError:pass
            except OSError:pass
        for hive,key in [(winreg.HKEY_CURRENT_USER,r'Software\Valve\Steam'),(winreg.HKEY_LOCAL_MACHINE,r'Software\WOW6432Node\Valve\Steam')]:
            try:
                with winreg.OpenKey(hive,key) as registry:
                    for name in ('SteamPath','InstallPath'):
                        try:steam.append(Path(winreg.QueryValueEx(registry,name)[0]))
                        except OSError:pass
            except OSError:pass
        for letter in 'CDEFGHIJKLMNOPQRSTUVWXYZ':
            base=Path(letter+':/')
            steam.append(base/'SteamLibrary')
            candidates.extend([base/'DCS World',base/'DCSWorld'])
        for key in ('ProgramFiles','ProgramFiles(x86)'):
            if os.environ.get(key):
                base=Path(os.environ[key]);steam.append(base/'Steam')
                candidates.extend([base/'Eagle Dynamics'/'DCS World',base/'Eagle Dynamics'/'DCS World OpenBeta'])
    for root in unique_paths(steam):
        libraries=[root]
        try:
            text=(root/'steamapps'/'libraryfolders.vdf').read_text(encoding='utf-8')
            libraries.extend(Path(p.replace('\\\\','\\')) for p in re.findall(r'"path"\s*"([^"]+)"',text))
        except OSError:pass
        for lib in libraries:
            candidates.extend([lib/'steamapps'/'common'/'DCSWorld',lib/'steamapps'/'common'/'DCS World'])
    return [p for p in unique_paths(candidates) if is_install(p)]


def module_path(source):
    for p in Path(source).resolve().parents:
        if p.parent.name.casefold() in ('aircraft','tech') and (p/'Textures').is_dir():return p
    return None


def normalized(name):return re.sub('[^a-z0-9]','',name.casefold())


def unit_aliases(source):
    name=re.sub(r'[_-](lod.*|collision.*)$','',Path(source).stem,flags=re.I)
    value=normalized(name);out={value}
    aliases={'fa18c':{'fa18chornet'},'f16cbl50':{'f16c50'},'su25t':{'su25t'},
             'a10c':{'a10c','a10cii'},'a10c2':{'a10c','a10cii'},'ka50':{'ka50','ka503'}}
    out.update(aliases.get(value,set()))
    if value.startswith('hb') and 'f14' in value:out.update({'f14b','f14a135gr'})
    return out


def walk_files(root,depth=5):
    try:
        for current,dirs,files in os.walk(root,followlinks=False):
            if len(Path(current).relative_to(root).parts)>=depth:dirs[:]=[]
            dirs.sort(key=str.casefold)
            for name in sorted(files,key=str.casefold):yield Path(current)/name
    except OSError:return


@lru_cache(maxsize=512)
def zip_entries(path,mtime):
    with zipfile.ZipFile(path) as archive:return tuple(archive.namelist())


@dataclass
class LiveryCatalog:
    liveries: list
    roots: list
    installations: list
    warnings: list


def discover_liveries(source,extra_roots=(),saved_roots=None,installs=None,progress=None):
    installs=installation_paths(source) if installs is None else unique_paths(installs)
    installs=unique_paths([*installs,*[p for p in unique_paths(extra_roots) if is_install(p)]])
    saved=saved_games_paths() if saved_roots is None else unique_paths(saved_roots)
    roots=[]
    def add(root,label):
        if root.is_dir():roots.append((root,label))
    for save in saved:
        try:profiles=[p for p in save.iterdir() if p.is_dir() and p.name.casefold().startswith('dcs')]
        except OSError:profiles=[]
        if save.name.casefold().startswith('dcs'):profiles.insert(0,save)
        for profile in profiles:
            label='保存的游戏 / '+profile.parent.name+'/'+profile.name
            add(profile/'Liveries',label)
            for category in ('aircraft','tech'):
                folder=profile/'Mods'/category
                if folder.is_dir():
                    for module in folder.iterdir():add(module/'Liveries',label)
    for install in installs:
        label='DCS 本体 / '+install.name
        add(install/'Bazar'/'Liveries',label)
        for base in ('CoreMods','Mods'):
            folder=install/base
            if folder.is_dir():
                for category in folder.iterdir():
                    if not category.is_dir():continue
                    add(category/'Liveries',label)
                    for module in category.iterdir():
                        if module.is_dir():add(module/'Liveries',label)
    for root in unique_paths(extra_roots):
        if not is_install(root):
            add(root,'添加目录');add(root/'Liveries','添加目录')
            for child in root.iterdir():
                if child.is_dir() and child.name.casefold().startswith('dcs'):add(child/'Liveries','添加目录')
    aliases=unit_aliases(source);targets=[];seen=set();scanned=[]
    for root,label in roots:
        key=str(root.resolve()).casefold()
        if key in seen:continue
        seen.add(key);scanned.append(str(root))
        if normalized(root.name) in aliases:targets.append((root,label));continue
        try:
            units=[p for p in root.iterdir() if p.is_dir() and normalized(p.name) in aliases]
            targets.extend((p,label) for p in units)
            if any(p.name.casefold()=='description.lua' for p in root.iterdir()):targets.append((root,label))
        except OSError:pass
    found=[];warnings=[];seen=set()
    for unit,label in targets:
        if progress:progress('扫描涂装：'+str(unit))
        for file in walk_files(unit):
            entries=[None] if file.name.casefold()=='description.lua' else []
            if file.suffix.casefold()=='.zip':
                try:entries=[n for n in zip_entries(str(file),file.stat().st_mtime_ns) if PurePosixPath(n).name.casefold()=='description.lua']
                except (OSError,zipfile.BadZipFile) as e:warnings.append(f'{file.name}: {e}')
            for entry in entries:
                key=(str(file.resolve()).casefold(),entry)
                if key in seen:continue
                seen.add(key)
                try:found.append(read_livery(file,entry,label,unit.name))
                except (OSError,ValueError,zipfile.BadZipFile,KeyError) as e:warnings.append(f'{file}'+(('::'+entry) if entry else '')+f': {e}')
    found.sort(key=lambda l:(0 if l.origin.startswith('保存') else 1,l.name.casefold(),l.identifier))
    return LiveryCatalog(found,scanned,installs,warnings)
