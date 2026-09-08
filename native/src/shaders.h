#pragma once
static constexpr const char* ModelShader = R"HLSL(
cbuffer Frame:register(b0){row_major float4x4 viewProjection;row_major float4x4 inverseVP;float4 eyeExposure;float4 settings;float4 ground;};
cbuffer SurfaceDecal:register(b1){row_major float4x4 surfaceDecalVP;float4 surfaceCenterWidth;float4 surfaceRightHeight;float4 surfaceUpDepth;float4 surfaceNormalOpacity;float4 surfaceOptions;float4 surfaceShadowSettings;};
struct Mesh{uint transform;uint skinned;uint number;uint pad;float4 color;uint blending;uint decal;uint twoSided;uint material;};
struct Pose{row_major float4x4 world;row_major float4x4 normal;};
StructuredBuffer<Mesh> meshes:register(t0);StructuredBuffer<Pose> poses:register(t1);StructuredBuffer<float4> numberOffsets:register(t2);
Texture2D diffuseMap:register(t3);Texture2D roughMap:register(t4);Texture2D decalMap:register(t5);SamplerState texSampler:register(s0);
Texture2D surfaceImage:register(t6);Texture2D<float> surfaceDepth:register(t7);
struct Input{float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float2 roughUV:TEXCOORD1;float2 decalUV:TEXCOORD2;uint4 joints0:BLENDINDICES0;uint4 joints1:BLENDINDICES1;float4 weights0:BLENDWEIGHT0;float4 weights1:BLENDWEIGHT1;uint selector:TEXCOORD3;uint instance:TEXCOORD4;};
struct Output{float4 position:SV_Position;float3 world:POSITION0;float3 normal:NORMAL0;float3 surfaceNormal:NORMAL1;float2 uv:TEXCOORD0;float2 roughUV:TEXCOORD1;float2 decalUV:TEXCOORD2;nointerpolation uint mesh:TEXCOORD3;};
Output VS(Input i){Mesh mesh=meshes[i.instance];float4 p=float4(i.position,1),world=0;float3 normal=0;if(mesh.skinned){[unroll]for(uint k=0;k<4;k++){Pose a=poses[mesh.transform+i.joints0[k]],b=poses[mesh.transform+i.joints1[k]];world+=mul(a.world,p)*i.weights0[k]+mul(b.world,p)*i.weights1[k];normal+=mul((float3x3)a.normal,i.normal)*i.weights0[k]+mul((float3x3)b.normal,i.normal)*i.weights1[k];}}else{Pose pose=poses[mesh.transform];world=mul(pose.world,p);normal=mul((float3x3)pose.normal,i.normal);}Output o;o.position=mul(viewProjection,float4(world.xyz,1));o.world=world.xyz;o.normal=normal;o.surfaceNormal=dot(normal,normal)>1e-24?normalize(normal):0;o.uv=i.uv;o.roughUV=i.roughUV;o.decalUV=i.decalUV;if(mesh.decal)o.decalUV+=numberOffsets[mesh.number+i.selector].xy;o.mesh=i.instance;return o;}
float4 SurfaceDepthVS(Input i):SV_Position{return mul(surfaceDecalVP,float4(VS(i).world,1));}
float3 surfaceSrgb(float3 color){color=max(color,0);return lerp(color*12.92,1.055*pow(color,1/2.4)-.055,step(.0031308,color));}
float3 surfaceLinear(float3 color){color=saturate(color);return lerp(color/12.92,pow((color+.055)/1.055,2.4),step(.04045,color));}
float4 surfaceSample(float2 uv){
 uint width,height;surfaceImage.GetDimensions(width,height);
 float2 texel=clamp(uv*float2(width,height)-.5,0,float2(width-1,height-1));
 int2 first=int2(floor(texel)),last=min(first+1,int2(width-1,height-1));float2 f=frac(texel);
 int2 points[4]={first,int2(last.x,first.y),int2(first.x,last.y),last};
 float weights[4]={(1-f.x)*(1-f.y),f.x*(1-f.y),(1-f.x)*f.y,f.x*f.y};float4 sum=0;
 [unroll]for(int k=0;k<4;k++){float4 sample=surfaceImage.Load(int3(points[k],0));if(surfaceShadowSettings.z<.5)sample.rgb=surfaceSrgb(sample.rgb);sum.rgb+=sample.rgb*sample.a*weights[k];sum.a+=sample.a*weights[k];}
 return float4(sum.a>1e-12?sum.rgb/sum.a:0,sum.a*surfaceNormalOpacity.w);
}
float4 surfacePreview(float4 base,Output i){
 Mesh mesh=meshes[i.mesh];if(surfaceOptions.x<.5||(mesh.pad&2)==0||mesh.decal)return base;
 float3 delta=i.world-surfaceCenterWidth.xyz;
 float2 uv=float2(dot(delta,surfaceRightHeight.xyz)/surfaceCenterWidth.w+.5,.5-dot(delta,surfaceUpDepth.xyz)/surfaceRightHeight.w);
 float localZ=dot(delta,surfaceNormalOpacity.xyz),depth=mul(surfaceDecalVP,float4(i.world,1)).z;
 float3 dx=ddx(float3(uv,depth)),dy=ddy(float3(uv,depth));float2 du=ddx(i.uv),dv=ddy(i.uv);
 if(any(uv<0)||any(uv>1)||abs(localZ)>surfaceUpDepth.w||abs(du.x*dv.y-du.y*dv.x)<1e-20)return base;
 if(surfaceOptions.y>.5&&dot(i.surfaceNormal,surfaceNormalOpacity.xyz)<=1e-8)return base;
 if(surfaceOptions.z>.5){
  float det=dx.x*dy.y-dx.y*dy.x;float2 slope=abs(det)>1e-20?float2(dx.z*dy.y-dy.z*dx.y,dy.z*dx.x-dx.z*dy.x)/det:0;
  uint width,height;surfaceDepth.GetDimensions(width,height);float2 size=float2(width,height);int2 first=int2(floor(uv*size-.5));
  [unroll]for(int y=0;y<2;y++){[unroll]for(int x=0;x<2;x++){
   int2 p=clamp(first+int2(x,y),0,int2(width-1,height-1));float receiver=depth+dot(slope,(float2(p)+.5)/size-uv);
   if(receiver-surfaceShadowSettings.x>surfaceDepth.Load(int3(p,0)))return base;
  }}
 }
 float4 ink=surfaceSample(uv);if(ink.a<=0)return base;float3 destination=surfaceSrgb(base.rgb);
 if(surfaceOptions.w>.5)base.rgb=surfaceLinear(lerp(destination,ink.rgb,ink.a));
 else{float destinationAlpha=mesh.color.a>1e-12?saturate(base.a/mesh.color.a):0;float alpha=ink.a+destinationAlpha*(1-ink.a);base.rgb=surfaceLinear((ink.rgb*ink.a+destination*destinationAlpha*(1-ink.a))/max(alpha,1e-20));base.a=alpha*mesh.color.a;}
 return base;
}
float3 tone(float3 c){c*=eyeExposure.w;return saturate((c*(2.51*c+.03))/(c*(2.43*c+.59)+.14));}
float4 PS(Output i,bool front:SV_IsFrontFace):SV_Target{Mesh mesh=meshes[i.mesh];float4 base=mesh.color;bool textureOn=settings.x>.5;if(textureOn){float4 tex=diffuseMap.Sample(texSampler,i.uv);uint tw,th;diffuseMap.GetDimensions(tw,th);if(tw>1||th>1)base.rgb=tex.rgb; base.a*=tex.a;if(mesh.decal){base=decalMap.Sample(texSampler,i.decalUV);clip(base.a-.1);}}
 if(textureOn)base=surfacePreview(base,i);if(base.a<.025)discard;float3 N=normalize(i.normal);if(mesh.twoSided&&!front)N=-N;float3 V=normalize(eyeExposure.xyz-i.world);float3 orm=settings.y>.5&&textureOn?roughMap.Sample(texSampler,i.roughUV).rgb:float3(1,.65,0);float roughness=clamp(orm.g,.08,1),metal=saturate(orm.b);float3 F0=lerp(.04.xxx,base.rgb,metal);float3 result=base.rgb*(1-metal)*lerp(.18,.42,saturate(N.y*.5+.5))*orm.r;float3 light[3]={normalize(float3(-.6,1,-.4)),normalize(float3(.6,.35,.7)),normalize(float3(.25,-.4,-.8))};float3 colors[3]={float3(2.6,2.5,2.35),float3(.65,.82,1.1),float3(.22,.25,.3)};
 [unroll]for(int k=0;k<3;k++){float3 L=light[k],H=normalize(L+V);float NL=saturate(dot(N,L)),NV=max(.001,saturate(dot(N,V))),NH=saturate(dot(N,H)),VH=saturate(dot(V,H));float a=roughness*roughness,a2=a*a;float den=NH*NH*(a2-1)+1;float D=a2/max(.00001,3.14159265*den*den);float g=(roughness+1)*(roughness+1)/8;float G=NV/(NV*(1-g)+g)*NL/max(.001,NL*(1-g)+g);float3 F=F0+(1-F0)*pow(1-VH,5);float3 spec=D*G*F/max(.001,4*NV*NL);result+=((1-F)*(1-metal)*base.rgb/3.14159265+spec)*colors[k]*NL;}
 float3 reflection=lerp(float3(.15,.2,.28),float3(.48,.52,.57),saturate(reflect(-V,N).y*.5+.5));result+=reflection*F0*(1-roughness*.7)*orm.r;if((mesh.pad&1)!=0)result=lerp(result,float3(.08,.6,1.8),.65);return float4(pow(tone(result),1/2.2),mesh.decal?1:base.a);}
struct GridOutput{float4 position:SV_Position;float2 uv:TEXCOORD0;};
GridOutput GridVS(uint id:SV_VertexID){GridOutput o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
struct GridPixel{float4 color:SV_Target;float depth:SV_Depth;};
GridPixel GridPS(GridOutput i){float2 xy=i.uv*float2(2,-2)+float2(-1,1);float4 n=mul(inverseVP,float4(xy,0,1)),f=mul(inverseVP,float4(xy,1,1));n/=n.w;f/=f.w;float3 ray=f.xyz-n.xyz;float t=(ground.x-n.y)/ray.y;clip(t);float3 p=n.xyz+t*ray;float scale=ground.y;float2 coord=p.xz/scale;float2 fw=max(fwidth(coord),.0001);float2 grid=abs(frac(coord-.5)-.5)/fw;float gridLine=1-min(min(grid.x,grid.y),1);float2 major=abs(frac(coord/5-.5)-.5)/max(fwidth(coord/5),.0001);float thick=1-min(min(major.x,major.y),1);float fade=exp(-length(p.xz-ground.zw)/max(scale*70,.1));float alpha=(gridLine*.09+thick*.11)*fade;clip(alpha-.003);GridPixel o;o.color=float4(.4,.51,.64,alpha);float4 clipP=mul(viewProjection,float4(p,1));o.depth=clipP.z/clipP.w;return o;}
)HLSL";

