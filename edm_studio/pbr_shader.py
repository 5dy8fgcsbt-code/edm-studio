"""GGX material preview using an approximate neutral studio environment."""
VERTEX = '''#version 120
varying vec3 positionEye, normalEye;
varying vec2 uvBase, uvRM, uvDecal;
void main() {
    vec4 p=gl_ModelViewMatrix*gl_Vertex;
    positionEye=p.xyz; normalEye=gl_NormalMatrix*gl_Normal;
    uvBase=gl_MultiTexCoord0.xy; uvRM=gl_MultiTexCoord1.xy; uvDecal=gl_MultiTexCoord2.xy;
    gl_Position=gl_ProjectionMatrix*p;
}
'''
FRAGMENT = '''#version 120
uniform sampler2D baseMap, rmMap, decalMap;
uniform bool hasBase, hasRM, hasDecal, isNumber;
uniform vec4 baseColor;
varying vec3 positionEye, normalEye;
varying vec2 uvBase, uvRM, uvDecal;
const float PI=3.14159265;
vec3 light(vec3 n, vec3 v, vec3 l, vec3 albedo, float rough, float metal) {
    vec3 h=normalize(v+l);
    float nl=max(dot(n,l),0.0),nv=max(dot(n,v),0.001),nh=max(dot(n,h),0.0);
    float a=rough*rough,a2=a*a,k=(rough+1.0)*(rough+1.0)/8.0;
    float d=a2/(PI*pow(nh*nh*(a2-1.0)+1.0,2.0)+0.00001);
    float g=(nv/(nv*(1.0-k)+k))*(nl/(nl*(1.0-k)+k));
    vec3 f0=mix(vec3(0.04),albedo,metal);
    vec3 f=f0+(1.0-f0)*pow(1.0-max(dot(h,v),0.0),5.0);
    return ((1.0-f)*(1.0-metal)*albedo/PI + d*g*f/(4.0*nv*nl+0.0001))*nl;
}
void main() {
    vec4 c=hasBase ? texture2D(baseMap,uvBase)*vec4(1,1,1,baseColor.a) : baseColor;
    vec3 orm=hasRM ? texture2D(rmMap,uvRM).rgb : vec3(1.0,0.65,0.0);
    if (hasDecal) {
        vec4 decal=texture2D(decalMap,uvDecal);
        if (isNumber && decal.a<0.1) discard;
        c.rgb=mix(c.rgb,decal.rgb,decal.a);
        if (isNumber) c.a=1.0;
        orm.b=mix(orm.b,0.0,decal.a);
    }
    vec3 albedo=pow(max(c.rgb,vec3(0)),vec3(2.2));
    float rough=clamp(orm.g,0.045,1.0),metal=clamp(orm.b,0.0,1.0);
    vec3 n=normalize(normalEye); if (!gl_FrontFacing) n=-n;
    vec3 v=normalize(-positionEye);
    vec3 color=light(n,v,normalize(vec3(-0.4,0.8,0.6)),albedo,rough,metal)*3.8;
    color+=light(n,v,normalize(vec3(0.8,0.3,-0.5)),albedo,rough,metal)*vec3(1.0,1.15,1.4);
    vec3 f0=mix(vec3(0.04),albedo,metal);
    color+=(albedo*(1.0-metal)*0.38+f0*(0.3+0.3*(1.0-rough)))*orm.r;
    color=vec3(1.0)-exp(-color*1.3);
    gl_FragColor=vec4(pow(max(color,vec3(0.0)),vec3(1.0/2.2)),c.a);
}
'''
