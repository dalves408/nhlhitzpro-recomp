#version 430 core
// Raw vertex input layout is shared with Nv2aGpuVs in nv2a_pbx.h.
layout(std430,binding=0) readonly buffer Raw { uint rb[]; };
uniform vec2 surf, gaa;
uniform float zscale,gfpx,gfpy;
uniform uint gfogmode,gindexed;
out vec4 raw_position,v_color,v_d1;
out vec4 v_tc[4];
out float v_fog;
#define INF uintBitsToFloat(0x7f800000u)
float saturate(float x){return clamp(x,0.0,1.0);}
vec4 saturate(vec4 x){return clamp(x,0.0,1.0);}
uint ld8(uint a){return (rb[a>>2]>>((a&3u)*8u))&255u;}
uint ld16(uint a){return ld8(a)|(ld8(a+1u)<<8);}
uint ld32(uint a){if((a&3u)==0u)return rb[a>>2];return ld16(a)|(ld16(a+2u)<<16);}
uvec4 load4(uint a){return uvec4(ld32(a),ld32(a+4u),ld32(a+8u),ld32(a+12u));}
vec4 K(uint i){return uintBitsToFloat(load4(i*16u));}
vec4 K(int i){return K(uint(i));}
vec4 KR(int i,int a0){int ci=i+a0;if(ci<0||ci>=192)ci=0;return K(ci);}
vec4 fetch(uint i,uint vi){
    uvec4 d=load4(3072u+i*16u);
    vec4 o=uintBitsToFloat(load4(3328u+i*16u));
    if(d.w!=0u)return o;
    uint a=d.x+vi*d.y,ty=d.z&255u,sz=d.z>>8;
    if(ty==0u){uint v=ld32(a);return vec4(float((v>>16)&255u),float((v>>8)&255u),float(v&255u),float(v>>24))/255.0;}
    if(ty==6u){uint v=ld32(a);return vec4(float(int(v<<21)>>21)/1023.0,float(int(v<<10)>>21)/1023.0,float(int(v)>>22)/511.0,1.0);}
    for(uint k=0u;k<4u;k++)if(k<sz){
        if(ty==2u)o[k]=uintBitsToFloat(ld32(a+k*4u));
        else if(ty==4u)o[k]=float(ld8(a+k))/255.0;
        else if(ty==1u)o[k]=float(int(ld16(a+k*2u)<<16)>>16)/32767.0;
        else if(ty==5u)o[k]=float(int(ld16(a+k*2u)<<16)>>16);
    }
    return o;
}
vec4 fetch(int i,uint vi){return fetch(uint(i),vi);}
vec4 m_dp3(vec4 a,vec4 b){precise float s=a.x*b.x+a.y*b.y+a.z*b.z;return vec4(s);}
vec4 m_dph(vec4 a,vec4 b){precise float s=a.x*b.x+a.y*b.y+a.z*b.z+b.w;return vec4(s);}
vec4 m_dp4(vec4 a,vec4 b){precise float s=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;return vec4(s);}
vec4 m_min(vec4 a,vec4 b){return vec4(a.x<b.x?a.x:b.x,a.y<b.y?a.y:b.y,a.z<b.z?a.z:b.z,a.w<b.w?a.w:b.w);}
vec4 m_max(vec4 a,vec4 b){return vec4(a.x>=b.x?a.x:b.x,a.y>=b.y?a.y:b.y,a.z>=b.z?a.z:b.z,a.w>=b.w?a.w:b.w);}
vec4 m_slt(vec4 a,vec4 b){return vec4(lessThan(a,b));}
vec4 m_sge(vec4 a,vec4 b){return vec4(greaterThanEqual(a,b));}
vec4 i_rcp(float x){return vec4(x==0.0?INF:1.0/x);}
vec4 i_rcc(float x){float f=1.0/x;if(abs(f)<5.42101e-20)f=f<0.0?-5.42101e-20:5.42101e-20;if(abs(f)>1.884467e19)f=f<0.0?-1.884467e19:1.884467e19;return vec4(f);}
vec4 i_rsq(float x){return vec4(inversesqrt(abs(x)));}
vec4 i_exp(float x){float fl=floor(x);return vec4(exp2(fl),x-fl,exp2(x),1.0);}
vec4 i_log(float x){float ax=abs(x);if(ax==0.0)return vec4(-INF,1.0,-INF,1.0);float e=floor(log2(ax));return vec4(e,ax/exp2(e),log2(ax),1.0);}
vec4 i_lit(vec4 c){float nl=max(c.x,0.0),nh=max(c.y,0.0),p=clamp(c.w,-127.9961,127.9961);return vec4(1.0,nl,c.x>0.0?(p==0.0?1.0:pow(nh,p)):0.0,1.0);}
float fogf(float d){
    if(gfogmode==0u)return 1.0;
    float f;
    if(gfogmode==0x800u||gfogmode==0x802u)f=gfpx+exp2(d*gfpy*16.0)-1.5;
    else if(gfogmode==0x801u||gfogmode==0x803u)f=gfpx+exp2(-d*d*gfpy*gfpy*32.0)-1.5;
    else f=gfpx+d*gfpy-1.0;
    if(gfogmode==0x802u||gfogmode==0x803u||gfogmode==0x804u)f=abs(f);
    return isnan(f)?1.0:saturate(f);
}
void main(){
    uint vi=gindexed!=0u?uint(gl_VertexID):ld32(3584u+uint(gl_VertexID)*4u);
//@@BODY@@
    precise vec4 P=R[12];
    if(floatBitsToUint(P.w)==0u)P.w=1.0;
    P.xy*=gaa;
    raw_position=P;
    precise vec2 xy=(P.xy/surf*2.0-1.0)*P.w;
    // FARCLIP: match Windows RECOMP_D3D11_GPUVS_FARCLIP / GVS_FARCLIP.
    // Behind-eye (w<=0) keep unclamped z so NV2A far-plane clip can discard;
    // front-eye still clamps to far plane via min(...,1.0).
    precise float cz = P.w > 0.0 ? min(P.z*zscale,1.0) : P.z*zscale;
    precise float z=(cz*2.0-1.0)*P.w;
    gl_Position=vec4(xy,z,P.w);
    v_color=saturate(O[3]);v_d1=saturate(O[4]);
    v_tc[0]=O[9];v_tc[1]=O[10];v_tc[2]=O[11];v_tc[3]=O[12];v_fog=fogf(O[5].x);
}
