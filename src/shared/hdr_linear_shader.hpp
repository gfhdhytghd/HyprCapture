#pragma once
namespace hyprcapture::color {
inline constexpr const char *hdrLinearVertex = R"(#version 300 es
void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0,1);}
)";
#define HYPRCAPTURE_LINEAR_FUNCTIONS                                           \
  R"(
uniform float referenceNits;uniform bool premultiplied;
vec3 prepare(vec4 v,ivec2 p){
 float a=isnan(v.a)||isinf(v.a)?0.:clamp(v.a,0.,1.);
 if(premultiplied)v.rgb=a>0.?v.rgb/a:vec3(0.);
 vec3 rgb=mat3(.627404,.069097,.016391,.329283,.919540,.088013,.043313,.011362,.895595)*v.rgb;
 if(a<1.){float s=((p.x/12+p.y/12)%2)==0?94./255.:72./255.;float checker=pow((s+.055)/1.055,2.4)*referenceNits/80.;rgb=rgb*a+vec3(checker*(1.-a));}
 for(int i=0;i<3;++i)rgb[i]=isnan(rgb[i])||isinf(rgb[i])?0.:clamp(rgb[i],0.,125.);
 return rgb;
}
)"
inline constexpr const char *hdrLinearFragment = R"(#version 300 es
precision highp float;uniform highp sampler2D source;out vec4 linearColor;
)" HYPRCAPTURE_LINEAR_FUNCTIONS R"(
void main(){ivec2 p=ivec2(gl_FragCoord.xy);vec3 rgb=prepare(texelFetch(source,p,0),p);
 linearColor=vec4(rgb,1.);
})";
inline constexpr const char *hdrPeakReduceFragment = R"(#version 300 es
precision highp float;uniform highp sampler2D source;uniform ivec2 sourceSize;uniform bool firstPass;out vec4 result;
)" HYPRCAPTURE_LINEAR_FUNCTIONS R"(
void main(){ivec2 origin=ivec2(gl_FragCoord.xy)*4;float peak=0.;for(int y=0;y<4;++y)for(int x=0;x<4;++x){ivec2 p=origin+ivec2(x,y);if(all(lessThan(p,sourceSize))){vec4 v=texelFetch(source,p,0);vec3 rgb=firstPass?prepare(v,p):vec3(v.r);peak=max(peak,max(max(rgb.r,rgb.g),rgb.b));}}result=vec4(peak,0.,0.,1.);})";
#undef HYPRCAPTURE_LINEAR_FUNCTIONS
} // namespace hyprcapture::color
