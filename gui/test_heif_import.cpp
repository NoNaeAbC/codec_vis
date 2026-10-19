#include "../codec_gui_image_io.hpp"
#include "raw_image_utils.hpp"
#include "raw_image_conversion.hpp"
#include "decoder_registry.hpp"
#include "test_check.hpp"
#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <lcms2.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
using namespace codec_gui;
using namespace codec_gui::gui;
#define CHECK CODEC_VIS_CHECK
static void ok(heif_error e){if(e.code!=heif_error_Ok)throw std::runtime_error(e.message?e.message:"HEIF error");}
struct Library{Library(){ok(heif_init(nullptr));}~Library(){heif_deinit();}};
using Context=std::unique_ptr<heif_context,decltype(&heif_context_free)>;
using Image=std::unique_ptr<heif_image,decltype(&heif_image_release)>;
using Handle=std::unique_ptr<heif_image_handle,decltype(&heif_image_handle_release)>;
using Encoder=std::unique_ptr<heif_encoder,decltype(&heif_encoder_release)>;
struct Fixture{
    Context context{heif_context_alloc(),heif_context_free};
    Encoder encoder{nullptr,heif_encoder_release};
    heif_chroma chroma;
    int depth;
    bool rgb;
    Fixture(heif_compression_format codec,heif_chroma c,int bits,bool isRgb=false):chroma(c),depth(bits),rgb(isRgb){
        heif_encoder* p=nullptr;ok(heif_context_get_encoder_for_format(context.get(),codec,&p));encoder.reset(p);
        heif_encoder_set_logging_level(encoder.get(),0);
        ok(heif_encoder_set_lossless(encoder.get(),1));
        ok(heif_encoder_set_parameter_string(encoder.get(),"chroma",c==heif_chroma_420?"420":c==heif_chroma_422?"422":"444"));
    }
    Handle add(bool pq=false,heif_orientation orientation=heif_orientation_normal,bool icc=false,int variant=0,bool limited=false,bool hlg=false){
        constexpr int w=64,h=48;
        heif_image* p=nullptr;
        ok(heif_image_create(w,h,rgb?heif_colorspace_RGB:heif_colorspace_YCbCr,chroma,&p));
        Image image(p,heif_image_release);
        for(int c=0;c<3;++c){
            const auto channel=rgb?heif_channel(int(heif_channel_R)+c):heif_channel(c);
            const int pw=c&&!rgb&&chroma!=heif_chroma_444?w/2:w;
            const int ph=c&&!rgb&&chroma==heif_chroma_420?h/2:h;
            ok(heif_image_add_plane(image.get(),channel,pw,ph,depth));
            size_t stride=0;auto* plane=heif_image_get_plane2(image.get(),channel,&stride);
            for(int y=0;y<ph;++y)for(int x=0;x<pw;++x){
                uint16_t v=uint16_t((x*13+y*7+c*53+variant*29)&((1u<<depth)-1));
                if(depth<=8)plane[y*stride+x]=uint8_t(v);else std::memcpy(plane+y*stride+x*2,&v,2);
            }
        }
        std::unique_ptr<heif_color_profile_nclx,decltype(&heif_nclx_color_profile_free)> profile(heif_nclx_color_profile_alloc(),heif_nclx_color_profile_free);
        profile->color_primaries=pq?heif_color_primaries_ITU_R_BT_2020_2_and_2100_0:heif_color_primaries_ITU_R_BT_709_5;
        profile->transfer_characteristics=hlg?heif_transfer_characteristic_ITU_R_BT_2100_0_HLG:pq?heif_transfer_characteristic_ITU_R_BT_2100_0_PQ:heif_transfer_characteristic_IEC_61966_2_1;
        profile->matrix_coefficients=rgb?heif_matrix_coefficients_RGB_GBR:pq?heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance:heif_matrix_coefficients_ITU_R_BT_709_5;
        profile->full_range_flag=limited?0:1;
        ok(heif_image_set_nclx_color_profile(image.get(),profile.get()));
        if(icc){
            auto srgb=cmsCreate_sRGBProfile();cmsUInt32Number size=0;CHECK(cmsSaveProfileToMem(srgb,nullptr,&size));
            std::vector<uint8_t> data(size);CHECK(cmsSaveProfileToMem(srgb,data.data(),&size));cmsCloseProfile(srgb);
            ok(heif_image_set_raw_color_profile(image.get(),"prof",data.data(),size));
        }
        std::unique_ptr<heif_encoding_options,decltype(&heif_encoding_options_free)> options(heif_encoding_options_alloc(),heif_encoding_options_free);
        options->image_orientation=orientation;
        options->output_nclx_profile=profile.get();
        options->save_two_colr_boxes_when_ICC_and_nclx_available=1;
        heif_image_handle* result=nullptr;
        ok(heif_context_encode_image(context.get(),image.get(),encoder.get(),options.get(),&result));
        return Handle(result,heif_image_handle_release);
    }
    std::vector<uint8_t> bytes(){
        std::vector<uint8_t> out;
        heif_writer writer{1,[](heif_context*,const void* data,size_t size,void* opaque)->heif_error{
            auto& bytes=*static_cast<std::vector<uint8_t>*>(opaque);
            const auto* p=static_cast<const uint8_t*>(data);bytes.insert(bytes.end(),p,p+size);
            return {heif_error_Ok,heif_suberror_Unspecified,nullptr};
        }};
        ok(heif_context_write(context.get(),&writer,&out));return out;
    }
};
static RawImage decode(const std::vector<uint8_t>& bytes){
    std::optional<double> seconds;
    auto image=decode_input_image(bytes,"wrong.extension",&seconds);
    CHECK(seconds&&*seconds>0);CHECK(image.compressedCodec=="HEIF");
    CHECK(image.compressedSource&&image.compressedSource->hevcAnnexB.size()==bytes.size());
    return image;
}
static void compare(const RawImage& actual,const std::vector<uint8_t>& bytes){
    Context ctx(heif_context_alloc(),heif_context_free);
    ok(heif_context_read_from_memory_without_copy(ctx.get(),bytes.data(),bytes.size(),nullptr));
    heif_image_handle* h=nullptr;ok(heif_context_get_primary_image_handle(ctx.get(),&h));Handle handle(h,heif_image_handle_release);
    std::unique_ptr<heif_decoding_options,decltype(&heif_decoding_options_free)> options(heif_decoding_options_alloc(),heif_decoding_options_free);
    options->output_image_nclx_profile_passthrough=1;options->strict_decoding=1;
    heif_image* p=nullptr;ok(heif_decode_image(handle.get(),&p,is_rgb(actual.format)?heif_colorspace_RGB:heif_colorspace_undefined,
        is_rgb(actual.format)?heif_chroma_444:heif_chroma_undefined,options.get()));Image image(p,heif_image_release);
    CHECK(actual.width==heif_image_get_primary_width(image.get()));CHECK(actual.height==heif_image_get_primary_height(image.get()));
    for(int c=0;c<plane_count(actual.format);++c){
        auto channel=is_rgb(actual.format)?heif_channel(int(heif_channel_R)+c):heif_channel(c);
        const int bits=heif_image_get_bits_per_pixel_range(image.get(),channel);
        size_t stride=0;const auto* source=heif_image_get_plane_readonly2(image.get(),channel,&stride);CHECK(source);
        for(int y=0;y<plane_height(actual,c);++y)for(int x=0;x<plane_width(actual,c);++x){
            uint16_t v=0;if(bits<=8)v=source[y*stride+x];else std::memcpy(&v,source+y*stride+x*2,2);
            const auto expected=is_rgb(actual.format)?(uint64_t(v)*uint32_t(max_sample_value(actual.format))+((1u<<bits)-1)/2)/((1u<<bits)-1):v;
            const auto got=sample_at(actual.planes[c],x,y,bytes_per_sample(actual.format));
            if(got!=expected)std::cerr<<"plane "<<c<<" x "<<x<<" y "<<y<<" bits "<<bits<<" actual "<<got<<" expected "<<expected<<"\n";
            CHECK(got==expected);
        }
    }
}
int main()try{
    Library library;
    int count=0;
    for(auto codec:{heif_compression_HEVC,heif_compression_AV1})
        for(int depth:{8,10,12})for(auto chroma:{heif_chroma_420,heif_chroma_422,heif_chroma_444}){
            Fixture fixture(codec,chroma,depth);auto handle=fixture.add(depth>8);auto bytes=fixture.bytes();
            auto actual=decode(bytes);CHECK(actual.width==64&&actual.height==48);
            CHECK(bit_depth(actual.format)==depth);CHECK(is_420(actual.format)==(chroma==heif_chroma_420));
            CHECK(is_422(actual.format)==(chroma==heif_chroma_422));
            CHECK(actual.color.primaries==(depth>8?ColorPrimaries::BT2020:ColorPrimaries::BT709));
            CHECK(actual.color.transfer==(depth>8?TransferCharacteristics::PQ:TransferCharacteristics::SRGB));
            CHECK(actual.color.range==ColorRange::Full);compare(actual,bytes);
            for(int c=0;c<3;++c)for(int y=0;y<plane_height(actual,c);++y)for(int x=0;x<plane_width(actual,c);++x)
                CHECK(sample_at(actual.planes[c],x,y,bytes_per_sample(actual.format))==uint32_t((x*13+y*7+c*53)&((1u<<depth)-1)));
            ++count;
            if(codec==heif_compression_AV1&&depth==8&&chroma==heif_chroma_420){
                std::ofstream file("build/qa-heif-import.avif",std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            }
            if(count==1){
                const auto path=std::filesystem::path("build/qa-heif-import.heic");
                std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();
                auto truncated=bytes;truncated.resize(bytes.size()/2);
                bool rejected=false;try{(void)decode(truncated);}catch(const std::exception&){rejected=true;}CHECK(rejected);
                auto loaded=load_input_image(path);CHECK(loaded.planes[0].bytes==actual.planes[0].bytes);
                ImageObject object;object.id={1};object.width=actual.width;object.height=actual.height;
                object.pixelFormat=actual.format;object.decoded=std::make_shared<RawImage>(actual);
                auto input=image_decode_input(object);auto decoders=image_decoder_candidates(input);CHECK(decoders.size()==1);
                auto result=decoders[0].decode(*input.bytes);CHECK(result.image&&decoder_output_matches(input,result));
                CHECK(result.image->planes[0].bytes==actual.planes[0].bytes);
            }
        }
    {
        const auto bytes=read_file_bytes("gui/fixtures/vvc32.vvic");
        auto actual=decode_input_image(bytes,"source.vvic");
        CHECK(actual.width==32&&actual.height==32);CHECK(actual.compressedCodec=="HEIF");
        compare(actual,bytes);
        auto loaded=load_input_image("gui/fixtures/vvc32.vvic");CHECK(loaded.planes[0].bytes==actual.planes[0].bytes);
    }
    for(int depth:{10,12}){
        Fixture fixture(heif_compression_HEVC,heif_chroma_444,depth,true);
        auto handle=fixture.add();auto bytes=fixture.bytes();auto actual=decode(bytes);
        CHECK(actual.format==PixelFormat::RGBP16LE&&actual.color.matrix==MatrixCoefficients::Identity);
        CHECK(actual.importNote.find("expanded to 16-bit")!=std::string::npos);compare(actual,bytes);
        const uint32_t maximum=(1u<<depth)-1;
        for(int c=0;c<3;++c)for(int y=0;y<48;++y)for(int x=0;x<64;++x)
            CHECK(sample_at(actual.planes[c],x,y,2)==(uint64_t((x*13+y*7+c*53)&maximum)*65535+maximum/2)/maximum);
    }
    {
        Fixture fixture(heif_compression_HEVC,heif_chroma_420,10);
        auto handle=fixture.add(true,heif_orientation_normal,false,0,true,true);
        auto bytes=fixture.bytes();auto actual=decode(bytes);
        CHECK(actual.color.range==ColorRange::Limited);
        CHECK(actual.color.primaries==ColorPrimaries::BT2020&&actual.color.transfer==TransferCharacteristics::HLG);
        compare(actual,bytes);
    }
    {
        Fixture fixture(heif_compression_HEVC,heif_chroma_444,10);
        auto handle=fixture.add(true,heif_orientation_rotate_90_cw);auto bytes=fixture.bytes();auto actual=decode(bytes);
        CHECK(actual.width==48&&actual.height==64);CHECK(actual.color.transfer==TransferCharacteristics::PQ);compare(actual,bytes);
    }
    {
        Fixture fixture(heif_compression_HEVC,heif_chroma_444,8,true);
        auto handle=fixture.add(false,heif_orientation_normal,true);auto actual=decode(fixture.bytes());
        CHECK(is_rgb(actual.format));CHECK(actual.color.primaries==ColorPrimaries::BT2020&&actual.color.transfer==TransferCharacteristics::Linear);
        CHECK(actual.importNote.find("ICC profile converted")!=std::string::npos);
    }
    {
        Fixture fixture(heif_compression_HEVC,heif_chroma_420,8);
        auto first=fixture.add();auto second=fixture.add(false,heif_orientation_normal,false,1);
        ok(heif_context_set_primary_image(fixture.context.get(),second.get()));
        auto bytes=fixture.bytes();auto actual=decode(bytes);compare(actual,bytes);
        CHECK(sample_at(actual.planes[0],0,0,1)==29);
    }
    {
        Fixture fixture(heif_compression_HEVC,heif_chroma_420,8);
        auto single=fixture.add();heif_image_handle* p=nullptr;
        ok(heif_context_add_grid_image(fixture.context.get(),64,48,1,1,nullptr,&p));Handle grid(p,heif_image_handle_release);
        heif_image* pixels=nullptr;ok(heif_image_create(64,48,heif_colorspace_YCbCr,heif_chroma_420,&pixels));
        Image tile(pixels,heif_image_release);
        for(int c=0;c<3;++c){const int w=c?32:64,h=c?24:48;
            ok(heif_image_add_plane(tile.get(),heif_channel(c),w,h,8));
            size_t stride=0;auto* plane=heif_image_get_plane2(tile.get(),heif_channel(c),&stride);
            for(int y=0;y<h;++y)std::memset(plane+y*stride,c?128:80,w);}
        ok(heif_context_add_image_tile(fixture.context.get(),grid.get(),0,0,tile.get(),fixture.encoder.get()));
        ok(heif_context_set_primary_image(fixture.context.get(),grid.get()));
        bool rejected=false;try{(void)decode(fixture.bytes());}catch(const std::exception& e){rejected=std::string(e.what()).find("grid images are not supported")!=std::string::npos;}
        CHECK(rejected);
    }
    for(const auto bytes:{std::vector<uint8_t>{0,0,0,24,'f','t','y','p','h','e','i','c'},std::vector<uint8_t>{'x'}}){
        bool rejected=false;try{(void)decode_input_image(bytes,"broken.heic");}catch(const std::exception&){rejected=true;}CHECK(rejected);
    }
    std::cout<<"HEIF import passed: "<<count<<" HEVC/AV1 precision/chroma/color cases; VVIC/VVC, native planes, primary selection, rotation, ICC, decoder replay, malformed input and grid rejection\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
