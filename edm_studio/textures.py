"""Shared livery texture resolution for the OpenGL viewer and glTF exporter."""
from dataclasses import dataclass,field
from pathlib import Path,PurePosixPath,PureWindowsPath
import io
import posixpath
import zipfile
from PIL import Image
from .liveries import read_livery,module_path,installation_paths,unique_paths,walk_files,zip_entries

IMAGE_TYPES={'.dds','.png','.jpg','.jpeg','.tga','.bmp'}


@dataclass(frozen=True)
class ImageSource:
    path: Path|None
    entry: str|None=None

    @property
    def key(self):return str(self.path)+(('::'+self.entry) if self.entry else '')
    def image(self):
        if self.path is None:return Image.new('RGBA',(1,1),(0,0,0,0))
        if self.entry:
            with zipfile.ZipFile(self.path) as archive:
                if archive.getinfo(self.entry).file_size>256*1024*1024:raise ValueError('Texture exceeds 256 MB')
                raw=archive.read(self.entry)
        else:
            if self.path.stat().st_size>256*1024*1024:raise ValueError('Texture exceeds 256 MB')
            raw=self.path.read_bytes()
        with Image.open(io.BytesIO(raw)) as image:
            image.load();return image.convert('RGBA')


def texture_key(name):
    path=PurePosixPath(name.replace('\\','/'))
    return str(path.with_suffix('') if path.suffix.casefold() in IMAGE_TYPES else path).casefold()


class TextureResolver:
    def __init__(self,source,extra=None,livery=None,installs=None):
        self.source=Path(source);self.livery=read_livery(livery) if livery else None
        self.index={};self.cache={};self.missing=[];self.resolved={};self.warnings=[];self.local={}
        module=module_path(source);roots=[]
        if extra:roots.append(Path(extra))
        if module:roots.append(module/'Textures')
        roots.extend([self.source.parent/'Textures',self.source.parent.parent/'Textures',self.source.parent])
        installs=installation_paths(source) if installs is None else installs
        if module:
            for install in installs:
                for category in ('CoreMods/aircraft','Mods/aircraft'):
                    roots.append(Path(install)/category/module.name/'Textures')
        if module is None:
            for install in installs:
                for category in ('CoreMods/aircraft','Mods/aircraft'):
                    folder=Path(install)/category
                    if folder.is_dir():roots.extend(m/'Textures' for m in folder.iterdir() if (m/'Shapes'/self.source.name).is_file())
        for install in installs:roots.extend([Path(install)/'Bazar'/'TempTextures',Path(install)/'Bazar'/'Textures'])
        self.roots=unique_paths(roots);self.pending=list(self.roots)
        if self.livery:
            if self.livery.entry:self._archive(self.livery.path,self.local,relative_to=str(PurePosixPath(self.livery.entry).parent))
            else:self._directory(self.livery.folder,self.local,recursive=True)

    def _put(self,index,name,source):
        key=texture_key(name);index.setdefault(key,source);index.setdefault(PurePosixPath(key).name,source)

    def _archive(self,path,index,relative_to=None):
        try:
            for name in zip_entries(str(path),path.stat().st_mtime_ns):
                if PurePosixPath(name).suffix.casefold() not in IMAGE_TYPES:continue
                relative=name
                if relative_to and relative_to!='.':
                    relative=posixpath.relpath(name,relative_to)
                    if relative.startswith('../'):continue
                self._put(index,relative,ImageSource(path,name))
        except (OSError,zipfile.BadZipFile) as error:self.warnings.append(f'{path.name}: {error}')

    def _directory(self,root,index,recursive=True):
        paths=walk_files(root,depth=8) if recursive else root.iterdir()
        for path in paths:
            if path.suffix.casefold() in IMAGE_TYPES:self._put(index,path.relative_to(root).as_posix(),ImageSource(path))
            elif path.suffix.casefold()=='.zip':self._archive(path,index)

    def common(self,name):
        key=texture_key(name)
        while key not in self.index and self.pending:
            root=self.pending.pop(0);self._directory(root,self.index,recursive=root!=self.source.parent)
        found=self.index.get(key)
        if found is None and key=='empty':return ImageSource(None)
        return found

    def livery_local(self,name):
        raw=name.replace('\\','/');key=texture_key(raw)
        if PureWindowsPath(raw).is_absolute() or raw.startswith('/') or ':' in raw:return None
        if key in self.local:return self.local[key]
        if self.livery.entry:
            candidate=posixpath.normpath(posixpath.join(str(PurePosixPath(self.livery.entry).parent),raw))
            if candidate.startswith('../'):return None
            names={texture_key(n):n for n in zip_entries(str(self.livery.path),self.livery.path.stat().st_mtime_ns)
                   if PurePosixPath(n).suffix.casefold() in IMAGE_TYPES}
            if texture_key(candidate) in names:return ImageSource(self.livery.path,names[texture_key(candidate)])
        else:
            parent=(self.livery.folder/raw).parent.resolve();allowed=self.livery.folder.parent.resolve()
            if not parent.is_relative_to(allowed):return None
            if parent.is_dir():
                stem=PurePosixPath(key).name
                for p in parent.iterdir():
                    if p.suffix.casefold() in IMAGE_TYPES and p.stem.casefold()==stem:return ImageSource(p)
        return None

    def material(self,material,slot=0):
        if slot==13:return self.roughmet(material)
        override=self.livery.override(material.name,slot) if self.livery else None
        if override:
            name=override.name;found=self.common(name) if override.common else self.livery_local(name)
        else:
            reference=material.texture_by_index(slot)
            if reference is None:return None
            name=reference.name;found=self.common(name)
        label=f'{material.name} [{slot}] → {name}'
        if found:self.resolved[label]=found.key
        else:self.missing.append(label)
        return found

    def roughmet(self,material):
        override=self.livery.override(material.name,13) if self.livery else None
        legacy_override=self.livery.override(material.name,2) if self.livery else None
        if not override and legacy_override and 'roughmet' in legacy_override.name.casefold():override=legacy_override
        if override:
            found=self.common(override.name) if override.common else self.livery_local(override.name)
            label=f'{material.name} [13] → {override.name}'
            if found:self.resolved[label]=found.key
            else:self.missing.append(label)
            return found
        reference=material.texture_by_index(13)
        legacy=material.texture_by_index(2)
        if reference is None and legacy and 'roughmet' in legacy.name.casefold():reference=legacy
        if reference:
            found=self.common(reference.name);label=f'{material.name} [13] → {reference.name}'
            if found:self.resolved[label]=found.key
            else:self.missing.append(label)
            return found
        # DCS also discovers implicit <diffuse>_RoughMet maps (e.g. F-14).
        diffuse=self.livery.override(material.name,0) if self.livery else None
        base=material.texture_by_index(0);candidates=[]
        if diffuse:candidates.append((texture_key(diffuse.name)+'_roughmet',not diffuse.common))
        if base:candidates.append((texture_key(base.name)+'_roughmet',False))
        for name,local in candidates:
            found=self.livery_local(name) if local else self.common(name)
            if found:
                self.resolved[f'{material.name} [13 auto] → {name}']=found.key
                return found
        return None

    def png_source(self,found):
        if found is None:return None
        if found.key in self.cache:return self.cache[found.key]
        try:
            image=found.image();stream=io.BytesIO();image.save(stream,format='PNG');raw=stream.getvalue()
        except (OSError,ValueError,NotImplementedError,Image.DecompressionBombError) as error:
            self.missing.append(found.key);self.warnings.append(str(error));raw=None
        self.cache[found.key]=raw;return raw

    def png(self,name):
        found=self.common(name)
        if not found:self.missing.append(name)
        return self.png_source(found)


@dataclass
class PreviewTextures:
    images: dict=field(default_factory=dict)
    materials: dict=field(default_factory=dict)
    roughmet: dict=field(default_factory=dict)
    decals: dict=field(default_factory=dict)
    missing: list=field(default_factory=list)
    warnings: list=field(default_factory=list)
    resolved: dict=field(default_factory=dict)


def preview_textures(scene,textures=None,livery=None,installs=None,progress=None,max_size=2048):
    resolver=TextureResolver(scene.source,textures,livery,installs);out=PreviewTextures()
    for i,material in enumerate(scene.materials):
        if progress:progress(f'读取贴图 {i+1}/{len(scene.materials)}：{material.name}')
        for slot,mapping in [(0,out.materials),(13,out.roughmet),(3,out.decals)]:
            found=resolver.material(material,slot)
            if found is None:continue
            if found.key not in out.images:
                try:
                    image=found.image();image.thumbnail((max_size,max_size),Image.Resampling.LANCZOS)
                    out.images[found.key]=(image.width,image.height,image.tobytes())
                except (OSError,ValueError,NotImplementedError,Image.DecompressionBombError) as error:
                    resolver.missing.append(found.key);resolver.warnings.append(str(error));continue
            mapping[i]=found.key
    out.missing=sorted(set(resolver.missing));out.warnings=resolver.warnings;out.resolved=resolver.resolved
    return out
