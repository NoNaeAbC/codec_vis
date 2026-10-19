#include "../codec_gui_x265.hpp"
#include "scc_decode.hpp"
#include "decoder_registry.hpp"
#include "raw_image_utils.hpp"
#include "../codec_gui_image_io.hpp"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <cstring>

using namespace codec_gui;
using namespace codec_gui::gui;
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--fixtures") {
            const std::filesystem::path directory=argv[2];
            for(const std::string name:{"127-4-scc-8-420.h265","128-4-scc-8-444.h265","129-4-scc-10-420.h265","130-4-scc-10-444.h265"}) {
                const auto coded=read_file_bytes(directory/name);
                const auto expected=read_file_bytes(directory/(name+".decoded.yuv"));
                EncodedImage encoded;encoded.hevcAnnexB.resize(coded.size());std::memcpy(encoded.hevcAnnexB.data(),coded.data(),coded.size());
                const auto decoded=decode_scc_vulkan(encoded);
                if(!decoded.image) throw std::runtime_error(decoded.error);
                const auto imported=decode_input_image(coded,name);
                const int bps=bytes_per_sample(decoded.image->format);size_t offset=0;uint64_t differences=0;
                if(imported.format!=decoded.image->format || imported.width!=decoded.image->width || imported.height!=decoded.image->height)
                    throw std::runtime_error("SCC import changed the native pixel layout");
                for(int p=0;p<3;++p)for(int y=0;y<plane_height(*decoded.image,p);++y)for(int x=0;x<plane_width(*decoded.image,p);++x) {
                    if(offset+bps>expected.size()) throw std::runtime_error("SCC reference is truncated");
                    const unsigned reference=expected[offset]|(bps==2?unsigned(expected[offset+1])<<8:0);offset+=bps;
                    const auto sample=sample_at(decoded.image->planes[p],x,y,bps);
                    differences+=sample!=reference;
                    if(sample!=sample_at(imported.planes[p],x,y,bps)) throw std::runtime_error("SCC import differs from native decoder");
                }
                if(offset!=expected.size() || differences) throw std::runtime_error(name+": native/reference mismatch, samples="+std::to_string(differences));
                std::cout<<name<<": native decode and production import match every stored reference sample"<<std::endl;
            }
            return 0;
        }
        const bool large=argc>1 && std::string(argv[1])=="--large";
        const bool experimental=argc>1 && std::string(argv[1])=="--experimental";
        int failures=0,cases=0;
        for(int depth:{8,10}) for(bool fullChroma:{false,true}) for(int recipe:{1,2,3}) {
            if (!experimental && recipe!=1) continue;
            RawImage input;input.width=large?5600:640;input.height=large?3728:480;
            input.format=fullChroma?(depth==8?PixelFormat::YUV444P8:PixelFormat::YUV444P10LE):(depth==8?PixelFormat::YUV420P8:PixelFormat::YUV420P10LE);
            input.color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,.matrix=MatrixCoefficients::BT709,.range=ColorRange::Limited};
            if(!fullChroma) input.color.chroma420Location=Chroma420SampleLocation::LeftCenter;
            const int bps=bytes_per_sample(input.format);
            for(int p=0;p<3;++p) {
                auto& plane=input.planes[p];plane.strideBytes=plane_width(input,p)*bps+32;
                plane.bytes.resize(size_t(plane.strideBytes)*plane_height(input,p));
                for(int y=0;y<plane_height(input,p);++y)for(int x=0;x<plane_width(input,p);++x)
                    put_sample(plane,x,y,bps,(p?96+(x/31+y/17+p*11)%64:16+((x/8)^(y/8))*13%220)<<(depth-8),max_sample_value(input.format));
            }
            const std::vector<EncoderParam> params{{"scc",true},{"scc-palette",recipe!=2},{"scc-ibc",recipe!=1},
                {"capture-reconstruction",!fullChroma},{"rate-control",std::string("cqp")},{"qpi",int64_t(fullChroma?1:35)},
                {"num-tile-cols",int64_t(1)},{"num-tile-rows",int64_t(1)}};
            std::cout<<"SCC "<<depth<<"-bit "<<(fullChroma?444:420)<<" recipe="<<recipe<<" size="<<input.width<<'x'<<input.height<<std::endl;
            auto encoded=encode_vaapi_hevc_still_image(input,params);
            const auto reconstruction=encoded.previewImage;encoded.previewImage.reset();
            if((!fullChroma && !reconstruction) || !hevc_is_scc(encoded)) throw std::runtime_error("SCC encoder did not retain the expected reconstruction/profile");
            auto decoded=decode_scc_vulkan(encoded);
            if(!decoded.image) {std::cerr<<decoded.error<<std::endl;return 1;}
            if(decoded.image->width!=input.width || decoded.image->height!=input.height || decoded.image->format!=input.format)
                throw std::runtime_error("SCC decoder changed geometry or precision");
            uint64_t differences=0;unsigned maximum=0;
            for(int p=0;p<3;++p)for(int y=0;y<plane_height(input,p);++y)for(int x=0;x<plane_width(input,p);++x) {
                const auto& reference=fullChroma?input:*reconstruction;
                const unsigned delta=std::abs(int(sample_at(reference.planes[p],x,y,bps))-int(sample_at(decoded.image->planes[p],x,y,bps)));
                differences+=delta!=0;maximum=std::max(maximum,delta);
            }
            std::cout<<(fullChroma?"Vulkan/source QP1":"Vulkan/reconstruction")<<" differences="<<differences<<" maximum="<<maximum<<std::endl;
            failures+=fullChroma ? maximum>unsigned(8<<(depth-8)) : differences!=0;++cases;
            ImageDecodeInput query;query.codec="HEVC";query.bytes=std::make_shared<EncodedImage>(encoded);query.reference=decoded.image;
            query.width=input.width;query.height=input.height;query.format=input.format;
            bool offered=false;
            for(const auto& candidate:image_decoder_candidates(query)) {
                if(candidate.info.id.starts_with("ff:")) throw std::runtime_error("SCC was routed through libavcodec");
                if(candidate.info.id.starts_with("native:hevc:scc:vulkan:")) {
                    const auto actual=candidate.decode(encoded);
                    if(!decoder_output_matches(query,actual)) throw std::runtime_error("SCC decoder selection rejected its native output");
                    offered=true;
                }
            }
            if(!offered) throw std::runtime_error("Native SCC decoder is absent from production discovery");
        }
        std::cout<<"SCC native Vulkan cases="<<cases<<" sample-check failures="<<failures<<std::endl;
        return failures?1:0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
