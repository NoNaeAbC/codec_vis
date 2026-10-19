#include "test_check.hpp"
#include "metrics.hpp"
#include "image_list_model.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
using namespace codec_gui;
using namespace codec_gui::gui;

LinearBt2020Image image(int w=32,int h=32,float value=.25f) {
    LinearBt2020Image out{.width=w,.height=h};
    for(auto& p:out.planes)p.assign(std::size_t(w)*h,value);
    return out;
}
RawImage raw(uint16_t value) {
    RawImage out;out.width=8;out.height=8;out.format=PixelFormat::RGBP16LE;
    out.color={.primaries=ColorPrimaries::BT2020,.transfer=TransferCharacteristics::Linear,
               .matrix=MatrixCoefficients::Identity,.range=ColorRange::Full};
    for(auto& p:out.planes) {p.strideBytes=16;p.bytes.resize(128);for(int i=0;i<64;++i)std::memcpy(p.bytes.data()+i*2,&value,2);}
    return out;
}
void bt709_to_2020(float r,float g,float b,float* dst) {
    dst[0]=.627403895934699*r+.329283038377883*g+.043313065687418*b;
    dst[1]=.069097289358232*r+.919540395075459*g+.011362315566309*b;
    dst[2]=.01639143887515*r+.088013307877225*g+.895595253247625*b;
}
int main() {
    std::set<std::string_view> ids;
    for(const auto& d:metric_catalog()) {CODEC_VIS_CHECK(ids.insert(d.id).second);CODEC_VIS_CHECK(d.evaluate && !d.label.empty());}
    CODEC_VIS_CHECK(ids.size()==7);
    CODEC_VIS_CHECK(!ids.contains("ssimulacra2"));
    // Working-space identity, small images included; all offered metrics
    // must produce numbers instead of reporting SDR/HDR as unsupported.
    for(const auto size:{1,8,32}) {
        const auto a=image(size,size);
        for(const auto& d:metric_catalog()) {
            const auto score=compute_metric(d.id,a,a);
            if(!score.value) std::cerr<<d.id<<": "<<score.unavailableReason<<'\n';
            CODEC_VIS_CHECK(score.value && !std::isnan(*score.value));
            if(d.id=="psnr-y" || d.id=="psnr-all") CODEC_VIS_CHECK(std::isinf(*score.value));
            else if(d.id=="ssimulacra2") CODEC_VIS_CHECK(std::abs(*score.value-100)<.001);
            else if(d.id=="cvvdp") CODEC_VIS_CHECK(std::abs(*score.value-10)<.001);
            else if(d.id=="butteraugli" || d.id=="ciede2000") CODEC_VIS_CHECK(std::abs(*score.value)<1e-6);
            else CODEC_VIS_CHECK(std::abs(*score.value-1)<1e-6);
        }
    }
    // Native reference fixtures: original libjxl SSIMULACRA 2.1,
    // Google standalone Butteraugli and fcvvdp 0.4.0, all in linear BT.709.
    // These goldens were produced in process by unmodified implementations.
    constexpr double golden[3][3]={{96.7857967207,.421256303787,9.99855041504},
                                  {97.4788060725,.231041371822,9.99999904633},
                                  {99.0585121516,.0168737005442,9.99995231628}};
    for(int fixture=0;fixture<3;++fixture) {
        auto a=image(128,128),b=a;
        for(int y=0;y<128;++y) for(int x=0;x<128;++x) {
            float va[3],vb[3];
            for(int p=0;p<3;++p) {
                const float v=fixture==1?.2f:(.1f+float((x*3+y*5+p*17)%97)/128.f);
                const float delta=fixture==0?.005f:fixture==1?.001f:float((x+y+p)%5)*.0001f;
                va[p]=v;vb[p]=v+delta;
            }
            float aa[3],bb[3];bt709_to_2020(va[0],va[1],va[2],aa);bt709_to_2020(vb[0],vb[1],vb[2],bb);
            for(int p=0;p<3;++p) {a.planes[p][y*128+x]=aa[p];b.planes[p][y*128+x]=bb[p];}
        }
        int k=1;
        for(const auto id:{"butteraugli","cvvdp"}) {
            const auto score=compute_metric(id,a,b);
            CODEC_VIS_CHECK(score.value);
            const double error=std::abs(*score.value-golden[fixture][k]);
            std::cout<<"reference parity fixture="<<fixture<<" metric="<<id<<" value="<<*score.value<<" error="<<error<<std::endl;
            CODEC_VIS_CHECK(error<(k==1?.0001:.00001));
            ++k;
        }
        // Linear RGB PSNR agrees with the original SDR RGB error norm,
        // despite the core receiving only BT.2020 coordinates.
        double sdrError=0;
        for(int y=0;y<128;++y)for(int x=0;x<128;++x)for(int p=0;p<3;++p) {
            const float v=fixture==1?.2f:(.1f+float((x*3+y*5+p*17)%97)/128.f);
            const float d=fixture==0?.005f:fixture==1?.001f:float((x+y+p)%5)*.0001f;
            const double error=double(v+d)-v;sdrError+=error*error;
        }
        const double expected=-10*std::log10(sdrError/(128*128*3));
        const double measured=*compute_metric("psnr-all",a,b).value;
        // Transforming the fixture to FP32 BT.2020 rounds each input once.
        // Bound error in sample units rather than magnifying it in dB for
        // near-identical pairs: four FP32 unit-scale ulps, never an 8-bit step.
        CODEC_VIS_CHECK(std::abs(std::pow(10.,-measured/20)-std::pow(10.,-expected/20))<5e-7);
    }
    // HDR and colors outside BT.709, including channels above reference white.
    for(const float level:{-.125f,2.f,16.f,100.f,60000.f}) {
        auto a=image(16,16,0),b=a;
        for(std::size_t i=0;i<a.planes[0].size();++i) {a.planes[1][i]=level;b.planes[1][i]=level*1.1f;}
        for(const auto& d:metric_catalog()) {
            const auto score=compute_metric(d.id,a,b);
            if(!score.value) std::cerr<<d.id<<": "<<score.unavailableReason<<'\n';
            CODEC_VIS_CHECK(score.value && std::isfinite(*score.value));
            if(d.id=="butteraugli" || d.id=="ciede2000") CODEC_VIS_CHECK(*score.value>0);
        }
    }
    // Sub-8-bit differences survive all color/adapter boundaries.
    auto a=image(),b=a;for(auto& p:b.planes)for(auto& v:p)v+=1.f/4096;
    for(const auto id:{"psnr-all","butteraugli","ciede2000"}) {
        const auto score=compute_metric(id,a,b);CODEC_VIS_CHECK(score.value && std::isfinite(*score.value));
        if(std::string_view(id)=="ssimulacra2") CODEC_VIS_CHECK(*score.value<100);
        if(std::string_view(id)=="butteraugli" || std::string_view(id)=="ciede2000")CODEC_VIS_CHECK(*score.value>0);
    }
    // Precision, transfer decoding, range and HDR unit conversion at ingress.
    CODEC_VIS_CHECK(std::isinf(*compute_psnr(raw(16384),raw(16384)).psnrAll));
    CODEC_VIS_CHECK(std::isfinite(*compute_psnr(raw(16384),raw(16385)).psnrAll));
    auto pq=raw(49151);pq.color.transfer=TransferCharacteristics::PQ;
    const auto hdr=raw_image_to_linear_bt2020(pq);CODEC_VIS_CHECK(hdr.planes[0][0]>1);
    auto hlg=raw(65535);hlg.color.transfer=TransferCharacteristics::HLG;hlg.nominalPeakNits=1000;
    CODEC_VIS_CHECK(std::abs(raw_image_to_linear_bt2020(hlg).planes[0][0]-1000.f/203)<.001);
    CODEC_VIS_CHECK(compute_psnr(pq,raw(49151)).psnrAll);
    for(const auto primaries:{ColorPrimaries::BT709,ColorPrimaries::BT2020,ColorPrimaries::DisplayP3,
        ColorPrimaries::DCIP3,ColorPrimaries::BT470M,ColorPrimaries::BT470BG,ColorPrimaries::BT601_525,ColorPrimaries::Film,ColorPrimaries::XYZ}) {
        auto input=raw(32768);input.color.primaries=primaries;
        const auto working=raw_image_to_linear_bt2020(input);
        for(const auto& plane:working.planes) CODEC_VIS_CHECK(std::isfinite(plane.front()));
        CODEC_VIS_CHECK(std::isinf(*compute_psnr(input,input).psnrAll));
        if(primaries!=ColorPrimaries::XYZ)for(const auto& plane:working.planes)
            CODEC_VIS_CHECK(std::abs(plane.front()-32768.f/65535)<1e-5);
    }
    auto broken=raw(0);broken.planes[1].bytes.clear();CODEC_VIS_CHECK(!compute_psnr(broken,raw(0)).psnrAll);
    auto mismatch=image();mismatch.width+=1;CODEC_VIS_CHECK(!compute_metric("psnr-all",a,mismatch).value);
    auto black=image(1,1,0),white=image(1,1,1);
    CODEC_VIS_CHECK(std::abs(*compute_metric("psnr-all",black,white).value)<1e-10);
    // Sharma et al. CIEDE2000 supplemental reference pairs. Construct their
    // Lab colors as unclipped linear BT.2020 and recover the published ΔE00.
    auto from_lab=[](double l,double aa,double bb) {
        auto inverse=[](double v){return v>6.0/29?v*v*v:3*(6.0/29)*(6.0/29)*(v-4.0/29);};
        const double fy=(l+16)/116;
        const double x=.9504559270516716*inverse(fy+aa/500),y=inverse(fy),z=1.0890577507598784*inverse(fy-bb/200);
        auto out=image(1,1);
        out.planes[0][0]=1.716651187971268*x-.355670783776392*y-.253366281373660*z;
        out.planes[1][0]=-.666684351832489*x+1.616481236634939*y+.015768545813911*z;
        out.planes[2][0]=.017639857445311*x-.042770613257809*y+.942103121235474*z;
        return out;
    };
    const double pairs[][3]={{2.6772,-79.7751,2.0425},{3.1571,-77.2803,2.8615},{2.8361,-74.0200,3.4412}};
    for(const auto& pair:pairs) {
        const auto value=compute_metric("ciede2000",from_lab(50,pair[0],pair[1]),from_lab(50,0,-82.7485));
        CODEC_VIS_CHECK(value.value && std::abs(*value.value-pair[2])<.0001);
    }
    std::cout<<"All metric working-space and adapter checks passed\n";
}
