#include "metrics.hpp"
#include "metric_perceptual.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace codec_gui::gui {
namespace {
void validate(const LinearBt2020Image& a,const LinearBt2020Image& b) {
    if(a.width<=0 || a.height<=0 || a.width!=b.width || a.height!=b.height)
        throw std::invalid_argument("metric image dimensions differ or are invalid");
    const auto count=std::size_t(a.width)*a.height;
    for(const auto* image:{&a,&b}) for(const auto& plane:image->planes) {
        if(plane.size()!=count) throw std::invalid_argument("linear metric plane is incomplete");
        if(!std::ranges::all_of(plane,[](float v){return std::isfinite(v);}))
            throw std::invalid_argument("non-finite linear metric input");
    }
}
double luma(const LinearBt2020Image& image,std::size_t i) {
    return .2627002120112671*image.planes[0][i]+.6779980715188708*image.planes[1][i]+.059301716469862*image.planes[2][i];
}
double psnr(const LinearBt2020Image& a,const LinearBt2020Image& b,bool rgb) {
    double error=0;
    const auto size=a.planes[0].size();
    for(std::size_t i=0;i<size;++i) {
        if(rgb) {
            // The calibrated RGB error norm as a metric tensor in BT.2020.
            // Q=T^T T, T=(BT.2020 -> XYZ -> BT.709). This preserves the
            // linear SDR reference score without constructing RGB709 pixels.
            constexpr double q[3][3]={{2.7730726389969917,-1.1150504594233994,-.1402324153416541},
                {-1.1150504594233994,1.6389003996292189,-.0791700798764168},
                {-.1402324153416541,-.0791700798764168,1.2569328706567298}};
            const double d[]={double(a.planes[0][i])-b.planes[0][i],double(a.planes[1][i])-b.planes[1][i],double(a.planes[2][i])-b.planes[2][i]};
            for(int r=0;r<3;++r) for(int c=0;c<3;++c)error+=d[r]*q[r][c]*d[c];
        }
        else {const double d=luma(a,i)-luma(b,i);error+=d*d;}
    }
    return error==0?std::numeric_limits<double>::infinity():-10*std::log10(error/(size*(rgb?3:1)));
}
double psnr_y(const LinearBt2020Image& a,const LinearBt2020Image& b) {return psnr(a,b,false);}
double psnr_rgb(const LinearBt2020Image& a,const LinearBt2020Image& b) {return psnr(a,b,true);}
using Plane=std::vector<double>;
int reflect(int x,int size) {
    while(x<0 || x>=size) x=x<0?-x-1:2*size-x-1;
    return x;
}
Plane blur(const Plane& in,int w,int h) {
    std::array<double,11> kernel{};
    double sum=0;
    for(int k=-5;k<=5;++k) sum+=(kernel[k+5]=std::exp(-double(k*k)/4.5));
    for(auto& v:kernel) v/=sum;
    Plane temp(in.size()),out(in.size());
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int k=-5;k<=5;++k)
        temp[std::size_t(y)*w+x]+=kernel[k+5]*in[std::size_t(y)*w+reflect(x+k,w)];
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int k=-5;k<=5;++k)
        out[std::size_t(y)*w+x]+=kernel[k+5]*temp[std::size_t(reflect(y+k,h))*w+x];
    return out;
}
std::pair<double,double> ssim_scale(const Plane& a,const Plane& b,int w,int h) {
    const auto ma=blur(a,w,h),mb=blur(b,w,h);
    Plane aa(a.size()),bb(a.size()),ab(a.size());
    for(std::size_t i=0;i<a.size();++i) {aa[i]=a[i]*a[i];bb[i]=b[i]*b[i];ab[i]=a[i]*b[i];}
    aa=blur(aa,w,h);bb=blur(bb,w,h);ab=blur(ab,w,h);
    double score=0,cs=0;
    for(std::size_t i=0;i<a.size();++i) {
        const double structure=(2*(ab[i]-ma[i]*mb[i])+.0009)/
            (std::max(0.0,aa[i]-ma[i]*ma[i])+std::max(0.0,bb[i]-mb[i]*mb[i])+.0009);
        const double brightness=(2*ma[i]*mb[i]+.0001)/(ma[i]*ma[i]+mb[i]*mb[i]+.0001);
        cs+=structure;score+=brightness*structure;
    }
    return {score/a.size(),cs/a.size()};
}
double structural(const LinearBt2020Image& a,const LinearBt2020Image& b,bool multi) {
    Plane aa(a.planes[0].size()),bb(aa.size());
    for(std::size_t i=0;i<aa.size();++i) {aa[i]=luma(a,i);bb[i]=luma(b,i);}
    int w=a.width,h=a.height;
    constexpr double weights[]={.0448,.2856,.3001,.2363,.1333};
    std::array<double,5> scores{},contrasts{};
    int levels=0;
    for(;levels<5;++levels) {
        const auto [s,c]=ssim_scale(aa,bb,w,h);scores[levels]=s;contrasts[levels]=c;
        if(!multi) return s;
        if(std::min(w,h)<2 || levels==4) {++levels;break;}
        const int nw=(w+1)/2,nh=(h+1)/2;
        auto downsample=[&](const Plane& in) {
            Plane out(std::size_t(nw)*nh);
            for(int y=0;y<nh;++y) for(int x=0;x<nw;++x) for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx)
                out[std::size_t(y)*nw+x]+=.25*in[std::size_t(std::min(2*y+dy,h-1))*w+std::min(2*x+dx,w-1)];
            return out;
        };
        aa=downsample(aa);bb=downsample(bb);w=nw;h=nh;
    }
    double product=1,weightSum=0;
    for(int i=0;i<levels;++i) weightSum+=weights[i];
    for(int i=0;i<levels;++i) product*=std::pow(std::max(0.0,i+1==levels?scores[i]:contrasts[i]),weights[i]/weightSum);
    return product;
}
double ssim(const LinearBt2020Image& a,const LinearBt2020Image& b) {return structural(a,b,false);}
double ms_ssim(const LinearBt2020Image& a,const LinearBt2020Image& b) {return structural(a,b,true);}
struct Lab {double l,a,b;};
Lab lab(const LinearBt2020Image& image,std::size_t i) {
    const double r=image.planes[0][i],g=image.planes[1][i],b=image.planes[2][i];
    auto f=[](double t){return t>216.0/24389?std::cbrt(t):(24389.0/27*t+16)/116;};
    // BT.2020 -> XYZ D65 -> Lab D65. No display RGB or gamut clipping.
    const double x=f((.6369580483012914*r+.14461690358620832*g+.1688809751641721*b)/.9504559270516716);
    const double y=f(.2627002120112671*r+.6779980715188708*g+.059301716469862*b);
    const double z=f((.028072693049087428*g+1.060985057710791*b)/1.0890577507598784);
    return {116*y-16,500*(x-y),200*(y-z)};
}
double delta_e(Lab a,Lab b) {
    constexpr double pi=std::numbers::pi;
    auto rad=[](double d){return d*pi/180;};
    const double c1=std::hypot(a.a,a.b),c2=std::hypot(b.a,b.b),c=(c1+c2)/2;
    const double c7=std::pow(c,7),g=.5*(1-std::sqrt(c7/(c7+6103515625.0)));
    const double ap=a.a*(1+g),bp=b.a*(1+g),ac=std::hypot(ap,a.b),bc=std::hypot(bp,b.b);
    auto hue=[](double y,double x) {double v=std::atan2(y,x);return v<0?v+2*pi:v;};
    const double ah=hue(a.b,ap),bh=hue(b.b,bp);
    double dh=bh-ah;
    if(ac*bc==0) dh=0;else if(dh>pi) dh-=2*pi;else if(dh<-pi) dh+=2*pi;
    const double dl=b.l-a.l,dc=bc-ac,dH=2*std::sqrt(ac*bc)*std::sin(dh/2);
    const double ml=(a.l+b.l)/2,mc=(ac+bc)/2;
    double mh=ah+bh;
    if(ac*bc!=0) {
        if(std::abs(ah-bh)<=pi) mh/=2;
        else mh=(mh+(mh<2*pi?2*pi:-2*pi))/2;
    }
    const double t=1-.17*std::cos(mh-rad(30))+.24*std::cos(2*mh)+.32*std::cos(3*mh+rad(6))-.20*std::cos(4*mh-rad(63));
    const double sl=1+.015*(ml-50)*(ml-50)/std::sqrt(20+(ml-50)*(ml-50)),sc=1+.045*mc,sh=1+.015*mc*t;
    const double mc7=std::pow(mc,7);
    const double rt=-2*std::sqrt(mc7/(mc7+6103515625.0))*std::sin(rad(60)*std::exp(-std::pow((mh-rad(275))/rad(25),2)));
    return std::sqrt(std::max(0.0,(dl/sl)*(dl/sl)+(dc/sc)*(dc/sc)+(dH/sh)*(dH/sh)+rt*(dc/sc)*(dH/sh)));
}
double ciede2000(const LinearBt2020Image& a,const LinearBt2020Image& b) {
    double sum=0;
    for(std::size_t i=0;i<a.planes[0].size();++i) sum+=delta_e(lab(a,i),lab(b,i));
    return sum/a.planes[0].size();
}
constexpr std::string_view structuralNote="Linear BT.2020 luminance, fixed reference white. SDR calibration; HDR extrapolated without clipping.";
constexpr MetricDescriptor catalog[]={
    // SSIMULACRA2 candidate is not admitted: its SDR reference parity check
    // fails. Do not offer a subtly different score under the upstream name.
    {"butteraugli","Butteraugli","","Google standalone Butteraugli; BT.2020 opsin transform; SDR calibration, HDR response extrapolated.",false,metric_butteraugli},
    {"cvvdp","ColorVideoVDP","JOD","fcvvdp 0.4.0, 30-inch 4K 200-nit SDR display model; BT.2020 XYZ transform, unclipped HDR extension.",true,metric_cvvdp},
    {"psnr-y","PSNR linear Y","dB","Linear BT.2020 luminance; fixed peak 1 = 203 cd/m², including HDR.",true,psnr_y},
    {"psnr-all","PSNR linear RGB","dB","SDR RGB error norm composed into a BT.2020 metric tensor; fixed peak 1 = 203 cd/m², including HDR.",true,psnr_rgb},
    {"ssim","SSIM linear Y","",structuralNote,true,ssim},
    {"ms-ssim","MS-SSIM linear Y","",structuralNote,true,ms_ssim},
    {"ciede2000","CIEDE2000","ΔE00","Mean ΔE00 in Lab D65, kL=kC=kH=1. SDR-calibrated; HDR extrapolated without clipping.",false,ciede2000},
};
}
std::span<const MetricDescriptor> metric_catalog() {return catalog;}
QualityMetricRecord compute_metric(std::string_view id,const LinearBt2020Image& a,const LinearBt2020Image& b) {
    const auto it=std::ranges::find_if(catalog,[&](const auto& d){return d.id==id;});
    if(it==std::end(catalog)) throw std::invalid_argument("unknown metric: "+std::string(id));
    QualityMetricRecord record{.id=std::string(it->id),.label=std::string(it->label),.unit=std::string(it->unit),
                              .higherIsBetter=it->higherIsBetter,.accuracyNote=std::string(it->accuracyNote)};
    try {
        validate(a,b);
        const double value=it->evaluate(a,b);
        if(std::isnan(value)) throw std::runtime_error("metric produced NaN");
        record.value=value;
    } catch(const std::exception& e) {record.unavailableReason=e.what();}
    return record;
}
MetricResult compute_quality_metrics(const RawImage& a,const RawImage& b) {
    MetricResult result;
    try {
        const auto reference=raw_image_to_linear_bt2020(a),candidate=raw_image_to_linear_bt2020(b);
        for(const auto& d:catalog) result.metrics.push_back(compute_metric(d.id,reference,candidate));
        for(const auto& r:result.metrics) {
            if(r.id=="psnr-y") result.psnrY=r.value;
            if(r.id=="psnr-all") result.psnrAll=r.value;
        }
    } catch(const std::exception& e) {result.unavailableReason=e.what();}
    return result;
}
MetricResult compute_psnr(const RawImage& a,const RawImage& b) {
    MetricResult result;
    try {
        const auto reference=raw_image_to_linear_bt2020(a),candidate=raw_image_to_linear_bt2020(b);
        result.metrics={compute_metric("psnr-y",reference,candidate),compute_metric("psnr-all",reference,candidate)};
        result.psnrY=result.metrics[0].value;result.psnrAll=result.metrics[1].value;
        if(!result.psnrY) result.unavailableReason=result.metrics[0].unavailableReason;
    } catch(const std::exception& e) {result.unavailableReason=e.what();}
    return result;
}
const QualityMetricRecord* metric_by_id(const EncodedMetadata& m,std::string_view id) {
    const auto it=std::ranges::find_if(m.metrics,[&](const auto& r){return r.id==id;});
    return it==m.metrics.end()?nullptr:&*it;
}
const QualityMetricRecord* primary_metric(const EncodedMetadata& m) {
    for(const auto& descriptor:catalog)
        if(const auto* r=metric_by_id(m,descriptor.id);r && r->value) return r;
    return nullptr;
}
}
