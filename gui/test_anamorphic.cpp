#include "../codec_gui_image_io.hpp"
#include "encoder_backends.hpp"
#include "preview_decoders.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"
#include "test_check.hpp"
#include <vvdec/vvdec.h>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace codec_gui;
using namespace codec_gui::gui;

static RawImage pattern(PixelAspectRatio sar) {
    RawImage image;
    image.width = 128; image.height = 96; image.format = PixelFormat::YUV420P8;
    image.pixelAspectRatio = sar;
    image.color = {.primaries=ColorPrimaries::BT709, .transfer=TransferCharacteristics::SRGB,
        .matrix=MatrixCoefficients::BT709, .range=ColorRange::Limited};
    for (int p=0;p<3;++p) {
        auto& plane=image.planes[p];
        plane.strideBytes=plane_width(image,p);
        plane.bytes.resize(plane.strideBytes*plane_height(image,p),p ? 128 : 100);
    }
    return image;
}

// Inspect native VUI fields, independently of the application's image metadata.
static void check_vvc_vui(const EncodedImage& coded, PixelAspectRatio sar) {
    vvdecParams params; vvdec_params_default(&params); params.threads=1;
    auto* decoder=vvdec_decoder_open(&params); CODEC_VIS_CHECK(decoder);
    auto* au=vvdec_accessUnit_alloc(); CODEC_VIS_CHECK(au);
    vvdec_accessUnit_alloc_payload(au, int(coded.hevcAnnexB.size()));
    std::memcpy(au->payload,coded.hevcAnnexB.data(),coded.hevcAnnexB.size());
    au->payloadUsedSize=int(coded.hevcAnnexB.size());
    vvdecFrame* frame=nullptr;
    const int status=vvdec_decode(decoder,au,&frame);
    CODEC_VIS_CHECK(status==VVDEC_OK || status==VVDEC_TRY_AGAIN);
    if(!frame) vvdec_flush(decoder,&frame);
    CODEC_VIS_CHECK(frame && frame->picAttributes && frame->picAttributes->vui);
    const auto& vui=*frame->picAttributes->vui;
    CODEC_VIS_CHECK(vui.aspectRatioInfoPresentFlag && vui.aspectRatioConstantFlag);
    CODEC_VIS_CHECK(vui.aspectRatioIdc==255 && vui.sarWidth==sar.width && vui.sarHeight==sar.height);
    vvdec_frame_unref(decoder,frame); vvdec_accessUnit_free(au); vvdec_decoder_close(decoder);
}

int main(int argc,char** argv) try {
    const auto backends=initial_encoder_backends(std::span<const VaapiDeviceInfo>{});
    for (const auto sar : {PixelAspectRatio(4,3),PixelAspectRatio(3,4),PixelAspectRatio(1,1)}) {
        auto input=pattern(sar);
        for (const auto id : {1,2,6,9,13,14,16}) {
            const auto* backend=find_backend(backends,BackendId{uint64_t(id)}); CODEC_VIS_CHECK(backend);
            std::vector<EncoderParam> params;
            if(id==1 || id==2 || id==6 || id==14 || id==16) params.push_back({"preset",std::string("ultrafast")});
            if(id==2) params.back().value=std::string("faster");
            if(id==6) params.push_back({"alf",false});
            auto encoded=run_backend_encode(*backend,input,params);
            CODEC_VIS_CHECK(encoded.comparisonReference->pixelAspectRatio==sar);
            auto bytesOnly=encoded.encoded;
            bytesOnly.previewImage.reset(); bytesOnly.codedColor.reset(); bytesOnly.requiredStreamColor.reset();
            const auto decoded=decode_backend_preview(*backend,bytesOnly);
            if(!decoded.image) throw std::runtime_error(backend->name+": "+decoded.error);
            if(decoded.image->pixelAspectRatio!=sar) throw std::runtime_error(backend->name+": SAR mismatch "+std::to_string(decoded.image->pixelAspectRatio.width)+":"+std::to_string(decoded.image->pixelAspectRatio.height));
            CODEC_VIS_CHECK(decoded.image->width==128 && decoded.image->height==96);
            const char* suffix=id==1 || id==16 ? ".hevc" : id==2 || id==6 ? ".vvc" : id==14 ? ".h264" : id==9 ? ".jpg" : ".png";
            const auto imported=decode_input_image({reinterpret_cast<const unsigned char*>(bytesOnly.hevcAnnexB.data()),bytesOnly.hevcAnnexB.size()},std::string("result")+suffix);
            CODEC_VIS_CHECK(imported.pixelAspectRatio==sar && imported.width==128 && imported.height==96);
            if(id==2 || id==6) check_vvc_vui(bytesOnly,sar);
            std::cout<<"PASS "<<backend->name<<" SAR "<<sar.width<<":"<<sar.height<<" signaling, decode and reimport\n";
        }
    }
    // Unsupported paths retain native dimensions and record the loss of signaling.
    const auto* webp=find_backend(backends,BackendId{17}); CODEC_VIS_CHECK(webp);
    auto result=run_backend_encode(*webp,pattern({4,3}),{});
    CODEC_VIS_CHECK(result.metadata.keyParams.back().name=="Warning");
    CODEC_VIS_CHECK(!backend_supports_pixel_aspect_ratio(webp->id));
    CODEC_VIS_CHECK(decode_backend_preview(*webp,result.encoded).image->pixelAspectRatio.square());
    if(argc>1) {
        auto source=load_input_image(argv[1]);
        source.pixelAspectRatio={4,3};
        auto converted=convert_raw_image_format(source,PixelFormat::YUV444P10LE);
        CODEC_VIS_CHECK(converted.pixelAspectRatio==PixelAspectRatio(4,3));
        CODEC_VIS_CHECK(converted.width==source.width && converted.height==source.height);
        std::cout<<"PASS real source "<<source.width<<"x"<<source.height<<": sample dimensions retained\n";
    }
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
