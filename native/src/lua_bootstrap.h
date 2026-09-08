#pragma once
static constexpr const char* LuaBootstrap = R"EDMLUA(
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
)EDMLUA";
