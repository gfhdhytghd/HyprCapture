#pragma once
namespace hyprcapture {
inline constexpr const char* sdrVideoFilter = "scale=in_range=full:out_range=tv:out_color_matrix=bt709,setparams=range=limited:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709";
inline constexpr const char* sdrVaapiFilter = "format=rgba,hwupload,scale_vaapi=format=nv12:out_color_matrix=bt709:out_range=tv,setparams=range=limited:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709";
}
