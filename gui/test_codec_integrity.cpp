#include "../codec_gui_image_io.hpp"
#include "../codec_gui_x265.hpp"
#include "encoder_param_state.hpp"
#include "preview_decoders.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"
#include "test_check.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

using namespace codec_gui;
using namespace codec_gui::gui;

static RawImage pattern(int depth, bool mono, int width=128, int height=128) {
    RawImage image;
    image.width=width; image.height=height;
    image.format=mono ? (depth==8 ? PixelFormat::Gray8 : depth==10 ? PixelFormat::Gray10LE : PixelFormat::Gray12LE)
                      : (depth==8 ? PixelFormat::YUV420P8 : PixelFormat::YUV420P10LE);
    image.color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,
        .matrix=MatrixCoefficients::BT709,.range=ColorRange::Limited};
    if (!mono) image.color.chroma420Location=Chroma420SampleLocation::LeftCenter;
    const int bps=bytes_per_sample(image.format);
    for (int p=0;p<plane_count(image.format);++p) {
        auto& plane=image.planes[p];
        plane.strideBytes=plane_width(image,p)*bps+32;
        plane.bytes.resize(size_t(plane.strideBytes)*plane_height(image,p),0xcd);
        for(int y=0;y<plane_height(image,p);++y) for(int x=0;x<plane_width(image,p);++x)
            put_sample(plane,x,y,bps,(p ? 120+(x*7+y*3+p*11)%16 : ((x/8+y/8)&1)?64:191)<<(depth-8),max_sample_value(image.format));
    }
    return image;
}

static void same_color(const ColorDescription& a,const ColorDescription& b,bool mono,bool checkLocation=true) {
    CODEC_VIS_CHECK(a.primaries==b.primaries && a.transfer==b.transfer && a.matrix==b.matrix && a.range==b.range);
    if(!mono && checkLocation) {
        if(a.chroma420Location!=b.chroma420Location)
            std::cerr<<"chroma location: decoded="<<(a.chroma420Location ? int(*a.chroma420Location) : -1)
                     <<" expected="<<(b.chroma420Location ? int(*b.chroma420Location) : -1)<<'\n';
        CODEC_VIS_CHECK(a.chroma420Location==b.chroma420Location);
    }
}

static double rgb_error(const RawImage& a,const RawImage& b) {
    const auto x=raw_image_to_rgba8(a),y=raw_image_to_rgba8(b);
    CODEC_VIS_CHECK(x.size()==y.size()); double error=0;
    for(size_t i=0;i<x.size();++i) if(i%4!=3) error+=std::abs(int(x[i])-int(y[i]));
    return error/(x.size()/4*3);
}

int main() {
    try {
        const auto caps=query_uvg266_parameters();
        const auto depthParam=std::ranges::find_if(caps,[](const auto& p){return p.name=="bit-depth";});
        CODEC_VIS_CHECK(depthParam!=caps.end());
        const int depth=std::stoi(std::get<std::string>(depthParam->defaultValue));
        const auto alf=std::ranges::find_if(caps,[](const auto& p){return p.name=="alf";});
        CODEC_VIS_CHECK(alf!=caps.end());
        BackendInfo backend; backend.id={6}; backend.params=caps;
        for(int rdo:{0,1,2}) for(bool mono:{false,true}) {
            const std::vector<EncoderParam> params{{"rdo",int64_t(rdo)},{"alf",true}};
            AppState state; state.encoderConfigs.push_back({.backend={6},.params=params});
            CODEC_VIS_CHECK(!parameter_is_enabled(state,backend,*alf));
            bool rejected=false;
            try {(void)encode_uvg266_still_image(pattern(depth,mono),params);}
            catch(const std::invalid_argument& e) {rejected=std::string(e.what()).find("ALF")!=std::string::npos;}
            CODEC_VIS_CHECK(rejected);
        }
        std::cout<<"PASS ALF rejected before encoding at RDO 0/1/2, color and monochrome\n";

        int cases=0,missingIndependentLocations=0;
        for(bool mono:{false,true})
            for(auto primaries:{ColorPrimaries::BT709,ColorPrimaries::DisplayP3,ColorPrimaries::BT2020})
                for(auto transfer:{TransferCharacteristics::SRGB,TransferCharacteristics::PQ,TransferCharacteristics::HLG})
                    for(auto range:{ColorRange::Limited,ColorRange::Full}) {
                        auto input=pattern(depth,mono);
                        input.color.primaries=primaries;input.color.transfer=transfer;input.color.range=range;
                        input.color.matrix=primaries==ColorPrimaries::BT2020 ? MatrixCoefficients::BT2020NonConstant : MatrixCoefficients::BT709;
                        if(!mono) input.color.chroma420Location=Chroma420SampleLocation::TopLeft;
                        const std::vector<EncoderParam> params{{"preset",std::string("medium")},{"qp",int64_t(20)},
                            {"rdo",int64_t(cases%3)},{"alf",false}};
                        auto encoded=encode_uvg266_still_image(input,params);
                        CODEC_VIS_CHECK(!encoded.hevcAnnexB.empty());
                        encoded.previewImage.reset();
                        // Neither decoder is allowed to take its color description
                        // from context. The second decoder is libavcodec in process.
                        encoded.codedColor=ColorDescription{};
                        const auto native=decode_vvc_preview(encoded); CODEC_VIS_CHECK(native.image);
                        same_color(native.image->color,input.color,mono);
                        auto bytesOnly=encoded; bytesOnly.codedColor.reset(); bytesOnly.requiredStreamColor.reset();
                        const auto independent=decode_libavcodec(bytesOnly,"vvc"); CODEC_VIS_CHECK(independent.image);
                        // This libavcodec VVC adapter may omit chroma location.
                        // Check any reported value; VVdeC must always report it.
                        const bool hasLocation=independent.image->color.chroma420Location.has_value();
                        if(!mono && !hasLocation) ++missingIndependentLocations;
                        same_color(independent.image->color,input.color,mono,hasLocation);
                        CODEC_VIS_CHECK(independent.image->format==native.image->format);
                        for(int p=0;p<plane_count(native.image->format);++p)
                            for(int y=0;y<plane_height(*native.image,p);++y)
                                for(int x=0;x<plane_width(*native.image,p);++x)
                                    CODEC_VIS_CHECK(sample_at(native.image->planes[p],x,y,bytes_per_sample(native.image->format))==
                                        sample_at(independent.image->planes[p],x,y,bytes_per_sample(independent.image->format)));
                        const auto imported=decode_input_image({reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data()),encoded.hevcAnnexB.size()},"result.vvc");
                        same_color(imported.color,input.color,mono);
                        encoded.requiredStreamColor->range=range==ColorRange::Full ? ColorRange::Limited : ColorRange::Full;
                        const auto mismatch=decode_vvc_preview(encoded);
                        CODEC_VIS_CHECK(!mismatch.image && mismatch.error.find("color verification")!=std::string::npos);
                        ++cases;
                    }
        std::cout<<"PASS "<<cases<<" VVC color cases: native VVdeC/libavcodec samples agree, metadata survives reimport\n";
        if(missingIndependentLocations)
            std::cout<<"LIMIT libavcodec omitted VVC chroma location in "<<missingIndependentLocations<<" cases; checked through VVdeC and reimport\n";
        for(unsigned location=0;location<=5;++location) {
            auto input=pattern(depth,false); input.color.chroma420Location=Chroma420SampleLocation(location);
            auto encoded=encode_uvg266_still_image(input,{});
            const auto decoded=decode_vvc_preview(encoded); CODEC_VIS_CHECK(decoded.image);
            same_color(decoded.image->color,input.color,false);
        }
        std::cout<<"PASS all six chroma sample locations\n";

        for(int precision:{8,10,12}) {
            const auto input=pattern(precision,true,96,64);
            const auto encoded=encode_jpeg2000_still_image(input,{});
            const auto decoded=decode_jpeg2000_preview(encoded); CODEC_VIS_CHECK(decoded.image);
            const double error=rgb_error(input,*decoded.image); CODEC_VIS_CHECK(error<10);
            const std::vector<EncoderParam> aggressive{{"quality",int64_t(50)}};
            const auto low=decode_jpeg2000_preview(encode_jpeg2000_still_image(input,aggressive)); CODEC_VIS_CHECK(low.image);
            CODEC_VIS_CHECK(error*2<rgb_error(input,*low.image));
            const std::vector<EncoderParam> lossless{{"lossless",true}};
            const auto exact=decode_jpeg2000_preview(encode_jpeg2000_still_image(input,lossless)); CODEC_VIS_CHECK(exact.image);
            CODEC_VIS_CHECK(rgb_error(input,*exact.image)<1);
            std::cout<<"PASS JPEG 2000 default "<<precision<<"-bit gray edges: mean RGB8 error "<<error<<"; lossless preserved\n";
        }
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
