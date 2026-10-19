#include "metric_perceptual.hpp"
#include "../third_party/butteraugli/butteraugli.h"
#include "../third_party/fcvvdp/cvvdp.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace codec_gui::gui {
double metric_butteraugli(const LinearBt2020Image& a,const LinearBt2020Image& b) {
    auto input=[](const LinearBt2020Image& in) {
        // Butteraugli's API uses floating linear intensity on a 255 scale.
        // Multiplying by 255 changes units, not precision or gamut.
        auto out=butteraugli::CreatePlanes<float>(std::max(8,in.width),std::max(8,in.height),3);
        for(int p=0;p<3;++p) for(int y=0;y<int(out[p].ysize());++y) for(int x=0;x<int(out[p].xsize());++x)
            out[p].Row(y)[x]=255.0f*in.planes[p][std::size_t(std::min(y,in.height-1))*in.width+std::min(x,in.width-1)];
        return out;
    };
    const auto reference=input(a),candidate=input(b);
    butteraugli::ImageF diff;
    double score=0;
    if(!butteraugli::ButteraugliInterface(reference,candidate,1.0f,diff,score))
        throw std::runtime_error("Butteraugli comparison failed");
    if(!std::isfinite(score)) throw std::runtime_error("Butteraugli produced a non-finite score");
    return score;
}
double metric_cvvdp(const LinearBt2020Image& a,const LinearBt2020Image& b) {
    const float* reference[]={a.planes[0].data(),a.planes[1].data(),a.planes[2].data()};
    const float* candidate[]={b.planes[0].data(),b.planes[1].data(),b.planes[2].data()};
    const FcvvdpImage ref{a.width,a.height,a.width*int(sizeof(float)),reference,CVVDP_PIXEL_FORMAT_RGB_PLANAR_FLOAT,CVVDP_COLORSPACE_LINEAR};
    const FcvvdpImage dist{b.width,b.height,b.width*int(sizeof(float)),candidate,CVVDP_PIXEL_FORMAT_RGB_PLANAR_FLOAT,CVVDP_COLORSPACE_LINEAR};
    FcvvdpResult result{};
    const auto status=cvvdp_compare_images(&ref,&dist,CVVDP_DISPLAY_STANDARD_4K,1,nullptr,&result);
    if(status!=CVVDP_OK) throw std::runtime_error(std::string("ColorVideoVDP: ")+cvvdp_error_string(status));
    if(!std::isfinite(result.jod)) throw std::runtime_error("ColorVideoVDP produced a non-finite score");
    return result.jod;
}
}
