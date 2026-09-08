"""Evaluate livery Lua in a disposable, quota-limited Lua 5.4 process.

The Lua environment contains only Lua values/functions. No Python objects,
filesystem APIs, native modules or operating-system libraries are exposed.
Includes are restricted to Lua text in the livery unit folder or its ZIP.
"""
import math
import multiprocessing as mp
import posixpath
from pathlib import Path, PurePosixPath
import zipfile
from .lua_data import LuaDataError, DataParser

BOOTSTRAP = r'''
local reader, context = ...
local compile, hook = load, debug.sethook
local env = {
  assert=assert, error=error, ipairs=ipairs, pairs=pairs, next=next,
  select=select, tonumber=tonumber, tostring=tostring, type=type, pcall=pcall, xpcall=xpcall,
  unpack=table.unpack, _VERSION=_VERSION,
  DIFFUSE=0, NORMAL_MAP=1, SPECULAR=2, DECAL=3, ROUGHNESS_METALLIC=13,
}
for _, name in ipairs({'math','string','table','utf8'}) do
  env[name]={}
  for k,v in pairs(_G[name]) do
    if k~='dump' then env[name][k]=v end
  end
end
env.math.randomseed(0)
env._G=env
env._=function(x) return x end
env.EDM_STUDIO=context
for k,v in pairs(context) do
  if env[k]~=nil then error('Reserved Lua context key: '..k) end
  env[k]=v
end
env.current_mod_path=env.current_mod_path or '.'
env.get_livery_name=function() return context.livery_name or '' end
local stack={''}
local loaded={}
local function run(text, name)
  local fn,err=compile(text, '@'..name, 't', env)
  if not fn then error(err,0) end
  stack[#stack+1]=name
  local result=fn()
  stack[#stack]=nil
  return result
end
env.dofile=function(name)
  assert(type(name)=='string','dofile expects a Lua filename')
  local text, resolved=reader(name, stack[#stack])
  return run(text,resolved)
end
env.require=function(name)
  assert(type(name)=='string','require expects a module name')
  if name=='i_18n' then return {translate=env._} end
  if loaded[name]~=nil then return loaded[name] end
  loaded[name]=true
  local result=env.dofile((name:gsub('%.','/'))..'.lua')
  if result~=nil then loaded[name]=result end
  return loaded[name]
end
env.loadstring=function(text)
  assert(type(text)=='string','loadstring expects text')
  return compile(text,'=livery expression','t',env)
end
-- debug and coroutines are absent. Protected calls can catch an instruction
-- error, so the parent ALSO enforces a hard process timeout.
local ticks=0
hook(function()
  ticks=ticks+1
  if ticks>500 then error('Lua instruction budget exceeded',0) end
end,'',1000)
return function(text)
  run(text,'description.lua')
  hook()
  return env
end
'''


class IncludeReader:
    def __init__(self,path,entry):
        self.path=Path(path) if path else None;self.entry=entry
        self.bytes=0;self.calls=0;self.files=[]

    def __call__(self,name,caller):
        self.calls+=1
        if self.calls>64:raise LuaDataError('Lua include limit exceeded (64)')
        if '\x00' in name:raise LuaDataError('Invalid Lua include path')
        name=name.replace('\\','/')
        if PurePosixPath(name).suffix.lower()!='.lua':raise LuaDataError('Only Lua text includes are allowed')
        if self.path is None:raise LuaDataError('This configuration has no include directory')
        if self.entry:
            base=str(PurePosixPath(self.entry).parent)
            location=posixpath.normpath(posixpath.join(base,posixpath.dirname(caller),name))
            if location.startswith(('../','/')) or ':' in location:raise LuaDataError('Lua include escapes livery ZIP')
            with zipfile.ZipFile(self.path) as z:
                actual=next((n for n in z.namelist() if n.casefold()==location.casefold()),None)
                if actual is None:raise LuaDataError('Lua include not found: '+name)
                if z.getinfo(actual).file_size>2_000_000:raise LuaDataError('Lua include exceeds 2 MB')
                raw=z.read(actual)
            resolved=posixpath.relpath(actual,base)
        else:
            root=self.path.parent.resolve();allowed=root.parent
            target=(root/posixpath.dirname(caller)/name).resolve()
            if not target.is_relative_to(allowed):raise LuaDataError('Lua include escapes livery unit directory')
            if target.stat().st_size>2_000_000:raise LuaDataError('Lua include exceeds 2 MB')
            raw=target.read_bytes();resolved=target.relative_to(allowed).as_posix()
            resolved=posixpath.relpath(resolved,root.name)
        self.bytes+=len(raw)
        if self.bytes>8_000_000:raise LuaDataError('Lua includes exceed 8 MB')
        self.files.append(resolved)
        from .liveries import decode_lua
        return decode_lua(raw),resolved


def _evaluate(text,path,entry,context):
    from lupa.lua54 import LuaRuntime,lua_type
    def deny(*args):raise AttributeError('Python attributes are unavailable in livery Lua')
    lua=LuaRuntime(max_memory=32*1024*1024,register_eval=False,register_builtins=False,
                   attribute_filter=deny,unpack_returned_tuples=True)
    reader=IncludeReader(path,entry)
    def table(value,depth=0):
        if depth>20:raise LuaDataError('Lua context too deeply nested')
        if isinstance(value,dict):return lua.table_from({k:table(v,depth+1) for k,v in value.items()})
        if isinstance(value,list):return lua.table_from([table(v,depth+1) for v in value])
        if value is None or isinstance(value,(str,int,float,bool)):return value
        raise LuaDataError('Invalid Lua context value')
    env=lua.execute(BOOTSTRAP,reader,table(context))(text)
    count=0
    def plain(value,depth=0):
        nonlocal count
        count+=1
        if count>100000 or depth>32:raise LuaDataError('Lua result is too large or cyclic')
        if lua_type(value)=='table':
            result={}
            for k,v in value.items():
                if not isinstance(k,(str,int,float,bool)):raise LuaDataError('Invalid Lua result key')
                result[k]=plain(v,depth+1)
            return result
        if value is None or isinstance(value,(str,int,float,bool)):
            if isinstance(value,float) and not math.isfinite(value):raise LuaDataError('Non-finite Lua result')
            return value
        raise LuaDataError('Livery result must contain data, not functions or userdata')
    result={k:plain(env[k]) for k in ('livery','name','custom_args','countries','order','info') if env[k] is not None}
    return result,{'engine':'Lua 5.4','includes':reader.files,'context':context}


def _worker(send,text,path,entry,context):
    try:send.send((True,_evaluate(text,path,entry,context)))
    except BaseException as error:send.send((False,str(error)[:2000]))
    finally:send.close()


def evaluate_config(text,path=None,entry=None,context=None,timeout=5.):
    if len(text)>2_000_000:raise LuaDataError('description.lua exceeds 2 MB')
    # Existing declarative liveries take the fast path during automatic scans.
    # Any unsupported syntax re-evaluates the WHOLE file in Lua; no partial result.
    if not context:
        try:
            parser=DataParser(text.replace('\r\n','\n').replace('\r','\n'))
            result,warnings=parser.parse()
            outputs={'livery','name','custom_args','countries','order','info'}
            local_output=any(t.value=='local' and parser.tokens[i+1].value in outputs for i,t in enumerate(parser.tokens[:-1]))
            if not warnings and not local_output:return result,[],{'engine':'declarative Lua','includes':[],'context':{}}
        except (LuaDataError,RecursionError):pass
    ctx=mp.get_context('spawn');receive,send=ctx.Pipe(duplex=False)
    process=ctx.Process(target=_worker,args=(send,text,str(path) if path else None,entry,context or {}),daemon=True)
    try:
        process.start();send.close()
        if not receive.poll(timeout):raise LuaDataError('Lua evaluation timed out')
        ok,payload=receive.recv()
        if not ok:raise LuaDataError('动态 Lua 求值失败：'+payload)
        result,metadata=payload
        return result,[],metadata
    except EOFError as error:raise LuaDataError('Lua worker exited without a result') from error
    finally:
        send.close();receive.close()
        if process.pid:
            process.join(.1)
            if process.is_alive():process.terminate();process.join(1.)
