#include "test_check.hpp"
#include "../codec_gui_jpegxs.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {
RawImage source(PixelFormat format) {
    RawImage image;
    image.width=128; image.height=96; image.format=format;
    image.color={.primaries=ColorPrimaries::BT2020,.transfer=TransferCharacteristics::HLG,
                 .matrix=MatrixCoefficients::BT2020NonConstant,.range=ColorRange::Full};
    const int bytes=bytes_per_sample(format);
    const auto maximum=static_cast<uint32_t>(max_sample_value(format));
    for (int p=0;p<3;++p) {
        auto& plane=image.planes[p];
        plane.strideBytes=plane_width(image,p)*bytes+16;
        plane.bytes.resize(static_cast<std::size_t>(plane.strideBytes)*plane_height(image,p),0xa5);
        for (int y=0;y<plane_height(image,p);++y) for (int x=0;x<plane_width(image,p);++x)
            put_sample(plane,x,y,bytes,static_cast<uint32_t>((x*271+y*139+p*977)&maximum),maximum);
    }
    return image;
}
void rejects(const std::function<void()>& action) {
    bool rejected=false;
    try { action(); } catch (const std::exception&) { rejected=true; }
    CODEC_VIS_CHECK(rejected);
}
}

int main() {
    EncodedImage example;
    for (const auto format : {PixelFormat::YUV420P8,PixelFormat::YUV422P8,PixelFormat::YUV444P8,
                              PixelFormat::YUV420P10LE,PixelFormat::YUV422P10LE,PixelFormat::YUV444P10LE,
                              PixelFormat::YUV420P12LE,PixelFormat::YUV422P12LE,PixelFormat::YUV444P12LE}) {
        const auto input=source(format);
        const std::vector<EncoderParam> params{{.name="lossless",.value=true}};
        const auto before=input;
        auto encoded=encode_jpegxs_still_image(input,params);
        for (int p=0;p<3;++p) CODEC_VIS_CHECK(input.planes[p].bytes==before.planes[p].bytes);
        // A fake reconstruction cannot satisfy this test: decoding must parse bytes.
        auto fake=std::make_shared<RawImage>();fake->width=1;fake->height=1;
        encoded.previewImage=fake;
        const auto decoded=decode_jpegxs_image(encoded);
        const auto parallel=decode_jpegxs_image(encoded,4);
        CODEC_VIS_CHECK(decoded && decoded->width==input.width && decoded->height==input.height && decoded->format==format);
        CODEC_VIS_CHECK(decoded->color.transfer==input.color.transfer && decoded->color.range==ColorRange::Full);
        for (int p=0;p<3;++p) for (int y=0;y<plane_height(input,p);++y) for (int x=0;x<plane_width(input,p);++x)
            {
                CODEC_VIS_CHECK(sample_at(input.planes[p],x,y,bytes_per_sample(format))==sample_at(decoded->planes[p],x,y,bytes_per_sample(format)));
                CODEC_VIS_CHECK(sample_at(input.planes[p],x,y,bytes_per_sample(format))==sample_at(parallel->planes[p],x,y,bytes_per_sample(format)));
            }
        example=std::move(encoded);
    }
    auto noContext=example;noContext.codedColor.reset();
    const auto unqualified=decode_jpegxs_image(noContext);
    CODEC_VIS_CHECK(unqualified->color.primaries==ColorPrimaries::Unspecified);
    CODEC_VIS_CHECK(unqualified->color.transfer==TransferCharacteristics::Unspecified);
    rejects([&]{ (void)decode_jpegxs_image(EncodedImage{}); });
    auto truncated=example;truncated.hevcAnnexB.resize(example.hevcAnnexB.size()/2);
    rejects([&]{ (void)decode_jpegxs_image(truncated); });
    auto corrupt=example;corrupt.hevcAnnexB[0]=std::byte{0};
    rejects([&]{ (void)decode_jpegxs_image(corrupt); });
    auto trailing=example;trailing.hevcAnnexB.push_back(std::byte{0});
    rejects([&]{ (void)decode_jpegxs_image(trailing); });
    auto input=source(PixelFormat::YUV444P10LE);
    const std::vector<EncoderParam> invalidSlice{{.name="slice-height",.value=int64_t{6}}};
    rejects([&]{ (void)encode_jpegxs_still_image(input,invalidSlice); });
    const std::vector<EncoderParam> unknown{{.name="misspelled-bpp",.value=double{6}}};
    rejects([&]{ (void)encode_jpegxs_still_image(input,unknown); });
    input.planes[0].strideBytes=2;
    rejects([&]{ (void)encode_jpegxs_still_image(input,{}); });
    input=source(PixelFormat::YUV444P14LE);
    rejects([&]{ (void)encode_jpegxs_still_image(input,{}); });
    input=source(PixelFormat::YUV444P10LE);input.format=PixelFormat::Gray10LE;
    rejects([&]{ (void)encode_jpegxs_still_image(input,{}); });
    std::cout << "JPEG XS: 9 exact padded-stride/full-range round trips and invalid-input contracts passed\n";
}
