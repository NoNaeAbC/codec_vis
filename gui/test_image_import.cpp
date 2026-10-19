#include "../codec_gui_image_io.hpp"
#include "../codec_gui_extra.hpp"
#include "../codec_gui_jpegxs.hpp"
#include "raw_image_utils.hpp"
#include "raw_image_conversion.hpp"
#include "test_check.hpp"
#include <chrono>

#include <charls/charls_jpegls_encoder.h>
#include <openjpeg.h>
#include <lcms2.h>
#include <png.h>
#include <bit>
extern "C" {
#include <JXRGlue.h>
#include <windowsmediaphoto.h>
}
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace codec_gui;
using namespace codec_gui::gui;
namespace {
constexpr int width=64,height=48;
uint32_t sample(int x,int y,int c,int depth) {return (x*733+y*391+c*8191)&((1u<<depth)-1);}
std::vector<uint8_t> pixels(int depth,int channels,int layout=2) {
	const int bps=depth<=8 ? 1 : 2;
	std::vector<uint8_t> out(width*height*channels*bps);
	for(int y=0;y<height;++y) for(int x=0;x<width;++x) for(int c=0;c<channels;++c) {
		const std::size_t i=layout==0 ? (c*height+y)*width+x : layout==1 ? (y*channels+c)*width+x : (y*width+x)*channels+c;
		const uint32_t q=sample(x,y,c,depth); out[i*bps]=q&255;if(bps==2)out[i*bps+1]=q>>8;
	}
	return out;
}
void exact(const RawImage& image,int depth,int channels) {
	CODEC_VIS_CHECK(image.width==width && image.height==height && is_rgb(image.format));
	const uint32_t maximum=(1u<<depth)-1,outputMaximum=max_sample_value(image.format);
	for(int y=0;y<height;++y) for(int x=0;x<width;++x) for(int c=0;c<3;++c) {
		const uint32_t expected=(uint64_t(sample(x,y,channels==1 ? 0 : c,depth))*outputMaximum+maximum/2)/maximum;
		CODEC_VIS_CHECK(sample_at(image.planes[c],x,y,bytes_per_sample(image.format))==expected);
	}
}
RawImage decode(const std::vector<uint8_t>& bytes,std::string_view hint="wrong.extension") {
	std::optional<double> codecSeconds;
	const auto start=std::chrono::steady_clock::now();
	const auto image=decode_input_image(bytes,hint,&codecSeconds);
	const double total=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
	CODEC_VIS_CHECK(codecSeconds && std::isfinite(*codecSeconds) && *codecSeconds>0 && *codecSeconds<=total);
	CODEC_VIS_CHECK(image.width>0 && image.height>0);
	for(const auto& plane:raw_image_to_linear_bt2020(image).planes) for(float v:plane) CODEC_VIS_CHECK(std::isfinite(v));
	bool rejected=false;
	try {(void)decode_input_image(std::span(bytes).first(std::min<std::size_t>(bytes.size(),4)),hint);} catch(const std::exception&){rejected=true;}
	CODEC_VIS_CHECK(rejected);
	return image;
}
std::vector<uint8_t> jls(int depth,int channels,int layout) {
	charls::jpegls_encoder encoder;
	encoder.frame_info({width,height,depth,channels}).interleave_mode(static_cast<charls::interleave_mode>(layout));
	std::vector<uint8_t> bytes(encoder.estimated_destination_size()); encoder.destination(bytes);
	bytes.resize(encoder.encode(pixels(depth,channels,layout)));
	return bytes;
}
std::vector<uint8_t> png(int depth,bool hdr=false,std::span<const uint8_t> icc={}) {
	std::vector<uint8_t> bytes;
	png_structp writer=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
	png_infop info=png_create_info_struct(writer);CODEC_VIS_CHECK(writer && info);
	CODEC_VIS_CHECK(setjmp(png_jmpbuf(writer))==0);
	png_set_write_fn(writer,&bytes,[](png_structp p,png_bytep src,png_size_t n){auto& b=*static_cast<std::vector<uint8_t>*>(png_get_io_ptr(p));b.insert(b.end(),src,src+n);},nullptr);
	png_set_IHDR(writer,info,width,height,depth,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
	if(!icc.empty()) png_set_iCCP(writer,info,"test",PNG_COMPRESSION_TYPE_BASE,icc.data(),icc.size());
#ifdef PNG_cICP_SUPPORTED
	if(hdr) png_set_cICP(writer,info,9,16,0,1);
#else
	CODEC_VIS_CHECK(!hdr);
#endif
	png_write_info(writer,info);
	if(depth==16 && std::endian::native==std::endian::little) png_set_swap(writer);
	auto rgb=pixels(depth,3);std::vector<png_bytep> rows(height);
	for(int y=0;y<height;++y)rows[y]=rgb.data()+y*width*3*(depth/8);
	png_write_image(writer,rows.data());png_write_end(writer,info);png_destroy_write_struct(&writer,&info);
	return bytes;
}
std::vector<uint8_t> png_with_invalid_icc() {
    // Correct PNG CRC and zlib stream, but only eight decompressed ICC bytes.
    std::vector<uint8_t> chunk{'i','C','C','P','I','C','C',0,0,
        120,156,99,100,98,102,97,101,99,231,0,0,0,128,0,37};
    uint32_t crc=0xffffffffu;
    for(uint8_t byte:chunk) {
        crc^=byte;
        for(int i=0;i<8;++i) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    crc^=0xffffffffu;
    const uint32_t length=chunk.size()-4;
    chunk.insert(chunk.begin(),{uint8_t(length>>24),uint8_t(length>>16),uint8_t(length>>8),uint8_t(length)});
    for(int n=24;n>=0;n-=8) chunk.push_back(uint8_t(crc>>n));
    auto bytes=png(8);bytes.insert(bytes.begin()+33,chunk.begin(),chunk.end());return bytes;
}
std::vector<uint8_t> jp2(int depth,int channels,bool raw) {
	opj_cparameters_t cfg{};opj_set_default_encoder_parameters(&cfg);
	cfg.tcp_numlayers=1;cfg.cp_disto_alloc=1;cfg.tcp_rates[0]=0;cfg.numresolution=3;cfg.irreversible=0;
	opj_image_cmptparm_t component[3]{};
	for(auto& c:component){c.dx=1;c.dy=1;c.w=width;c.h=height;c.prec=depth;}
	std::unique_ptr<opj_image_t,decltype(&opj_image_destroy)> image(opj_image_create(channels,component,channels==1 ? OPJ_CLRSPC_GRAY : OPJ_CLRSPC_SRGB),opj_image_destroy);
	CODEC_VIS_CHECK(image);image->x1=width;image->y1=height;
	for(int c=0;c<channels;++c) for(int y=0;y<height;++y) for(int x=0;x<width;++x) image->comps[c].data[y*width+x]=sample(x,y,c,depth);
	std::unique_ptr<opj_codec_t,decltype(&opj_destroy_codec)> encoder(opj_create_compress(raw ? OPJ_CODEC_J2K : OPJ_CODEC_JP2),opj_destroy_codec);
	CODEC_VIS_CHECK(encoder && opj_setup_encoder(encoder.get(),&cfg,image.get()));
	struct Writer {std::vector<uint8_t> bytes;std::size_t pos=0;} writer;
	std::unique_ptr<opj_stream_t,decltype(&opj_stream_destroy)> stream(opj_stream_default_create(OPJ_FALSE),opj_stream_destroy);
	opj_stream_set_user_data(stream.get(),&writer,nullptr);
	opj_stream_set_write_function(stream.get(),[](void* data,OPJ_SIZE_T n,void* user)->OPJ_SIZE_T {
		auto& w=*static_cast<Writer*>(user);w.bytes.resize(std::max(w.bytes.size(),w.pos+n));std::memcpy(w.bytes.data()+w.pos,data,n);w.pos+=n;return n;
	});
	opj_stream_set_seek_function(stream.get(),[](OPJ_OFF_T at,void* user)->OPJ_BOOL {
		auto& w=*static_cast<Writer*>(user);if(at<0)return OPJ_FALSE;w.pos=at;return OPJ_TRUE;
	});
	opj_stream_set_skip_function(stream.get(),[](OPJ_OFF_T n,void* user)->OPJ_OFF_T {
		auto& w=*static_cast<Writer*>(user);if(n < -OPJ_OFF_T(w.pos))return -1;w.pos+=n;return n;
	});
	CODEC_VIS_CHECK(opj_start_compress(encoder.get(),image.get(),stream.get()) && opj_encode(encoder.get(),stream.get()) && opj_end_compress(encoder.get(),stream.get()));
	return writer.bytes;
}
std::vector<uint8_t> jxr(int depth) {
	std::vector<uint8_t> bytes(1024*1024);
	WMPStream* stream=nullptr;CODEC_VIS_CHECK(CreateWS_Memory(&stream,bytes.data(),bytes.size())==0);
	// The encoder seeks back to rewrite the TIFF directory. Track the highest
	// written byte rather than mistaking its final cursor for the stream size.
	static const auto writeMemory=stream->Write;
	stream->state.buf.cbBufCount=0;
	stream->Write=[](WMPStream* s,const void* data,size_t n)->ERR {
		const auto error=writeMemory(s,data,n);
		if(!error) s->state.buf.cbBufCount=std::max(s->state.buf.cbBufCount,s->state.buf.cbCur);
		return error;
	};
	PKImageEncode* encoder=nullptr;CODEC_VIS_CHECK(PKImageEncode_Create_WMP(&encoder)==0);
	CWMIStrCodecParam cfg{};cfg.uiDefaultQPIndex=1;cfg.olOverlap=OL_NONE;cfg.bfBitstreamFormat=SPATIAL;cfg.sbSubband=SB_ALL;cfg.cfColorFormat=YUV_444;
	CODEC_VIS_CHECK(encoder->Initialize(encoder,stream,&cfg,sizeof(cfg))==0);
	CODEC_VIS_CHECK(encoder->SetPixelFormat(encoder,depth==8 ? GUID_PKPixelFormat24bppRGB : GUID_PKPixelFormat48bppRGB)==0);
	CODEC_VIS_CHECK(encoder->SetSize(encoder,width,height)==0);
	auto rgb=pixels(depth,3);
	CODEC_VIS_CHECK(encoder->WritePixels(encoder,height,rgb.data(),width*3*(depth==8 ? 1 : 2))==0);
	const size_t size=stream->state.buf.cbBufCount;
	CODEC_VIS_CHECK(size>0 && size<=bytes.size());
	CODEC_VIS_CHECK(encoder->Release(&encoder)==0); // Also closes the memory stream.
	bytes.resize(size);return bytes;
}
std::vector<uint8_t> bytes(const EncodedImage& encoded) {
	const auto* p=reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data());return {p,p+encoded.hevcAnnexB.size()};
}
}

int main() {
	try {
		for(int depth:{8,16}) {
            const auto image=decode(png(depth));exact(image,depth,3);
            CODEC_VIS_CHECK(image.importNote.find("assuming sRGB")!=std::string::npos);
            std::cout<<"PASS PNG depth="<<depth<<" and explicit color assumption"<<std::endl;
        }
        bool invalidIccRejected=false;
        try {(void)decode_input_image(png_with_invalid_icc(),"invalid-icc.png");}
        catch(const std::exception& e) {invalidIccRejected=std::string(e.what()).find("ICC")!=std::string::npos;}
        CODEC_VIS_CHECK(invalidIccRejected);
        std::cout<<"PASS malformed PNG ICC rejected with profile error"<<std::endl;
#ifdef PNG_cICP_SUPPORTED
		const auto hdrPng=decode(png(16,true));exact(hdrPng,16,3);
		CODEC_VIS_CHECK(hdrPng.color.primaries==ColorPrimaries::BT2020 && hdrPng.color.transfer==TransferCharacteristics::PQ);
		std::cout<<"PASS PNG 16-bit PQ/BT.2020 color signaling"<<std::endl;
#endif
		for(int depth:{2,8,10,12,14,16}) for(int channels:{1,3}) for(int layout=0;layout<(channels==1 ? 1 : 3);++layout) {
			const auto image=decode(jls(depth,channels,layout),"mislabeled.jpg");exact(image,depth,channels);
			CODEC_VIS_CHECK(!image.importNote.empty());
			std::cout<<"PASS JPEG-LS depth="<<depth<<" channels="<<channels<<" interleave="<<layout<<std::endl;
		}
		for(int depth:{8,10,12,14,16}) for(int channels:{1,3}) for(bool raw:{false,true}) {
			exact(decode(jp2(depth,channels,raw)),depth,channels);
			std::cout<<"PASS "<<(raw ? "J2K" : "JP2")<<" depth="<<depth<<" channels="<<channels<<std::endl;
		}
		for(int depth:{8,16}) {
			exact(decode(jxr(depth)),depth,3);
			std::cout<<"PASS JPEG XR depth="<<depth<<std::endl;
		}
		RawImage rgb;rgb.width=width;rgb.height=height;rgb.format=PixelFormat::RGBP8;
		rgb.color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,.matrix=MatrixCoefficients::Identity,.range=ColorRange::Full};
		for(int c=0;c<3;++c){rgb.planes[c].strideBytes=width;rgb.planes[c].bytes.resize(width*height);for(int y=0;y<height;++y)for(int x=0;x<width;++x)rgb.planes[c].bytes[y*width+x]=sample(x,y,c,8);}
		const std::vector<EncoderParam> lossless{{.name="lossless",.value=true}};
		auto webp=bytes(encode_webp_still_image(rgb,lossless));exact(decode(webp),8,3);
		// Exercise ICC chunk handling with a valid native sRGB profile.
		cmsHPROFILE profile=cmsCreate_sRGBProfile();cmsUInt32Number size=0;CODEC_VIS_CHECK(cmsSaveProfileToMem(profile,nullptr,&size));
		std::vector<uint8_t> icc(size);CODEC_VIS_CHECK(cmsSaveProfileToMem(profile,icc.data(),&size));cmsCloseProfile(profile);
        const auto managedPng=decode(png(8,false,icc));
        CODEC_VIS_CHECK(managedPng.color.primaries==ColorPrimaries::BT2020 && managedPng.color.transfer==TransferCharacteristics::Linear);
        CODEC_VIS_CHECK(managedPng.importNote.find("ICC")!=std::string::npos);
        std::cout<<"PASS valid PNG ICC conversion and note"<<std::endl;
		std::vector<uint8_t> tagged(webp.begin(),webp.begin()+12);
		auto le=[&](uint32_t v,int n){for(int i=0;i<n;++i)tagged.push_back((v>>(8*i))&255);};
		for(char c:std::string_view("VP8X"))tagged.push_back(c);le(10,4);le(0x20,4);le(width-1,3);le(height-1,3);
		for(char c:std::string_view("ICCP"))tagged.push_back(c);le(size,4);tagged.insert(tagged.end(),icc.begin(),icc.end());if(size&1)tagged.push_back(0);
		tagged.insert(tagged.end(),webp.begin()+12,webp.end());for(int i=0;i<4;++i)tagged[4+i]=((tagged.size()-8)>>(8*i))&255;
		const auto managed=decode(tagged);CODEC_VIS_CHECK(managed.color.primaries==ColorPrimaries::BT2020 && managed.color.transfer==TransferCharacteristics::Linear);
		std::cout<<"PASS WebP lossless and embedded ICC"<<std::endl;
		ColorTransformOptions transform;transform.target=rgb.color;transform.target.matrix=MatrixCoefficients::BT709;transform.target.range=ColorRange::Limited;
		const auto yuv=transform_raw_image(rgb,PixelFormat::YUV444P8,transform);
		const auto h264=decode(bytes(encode_x264_intra_still_image(yuv,{})),"source.H264");
		CODEC_VIS_CHECK(h264.width==width && h264.height==height && !h264.importNote.empty());
		std::cout<<"PASS H.264 Annex B import"<<std::endl;
		for(auto encode:{encode_png_still_image,encode_jpeg_still_image,encode_jpegxl_still_image,encode_jpegxs_still_image}) {
			const auto image=decode(bytes(encode(yuv,{})));CODEC_VIS_CHECK(image.width==width && image.height==height);
		}
		auto jpeg=bytes(encode_jpeg_still_image(yuv,{}));
		const std::vector<uint8_t> app{0xff,0xe1,0,6,0xff,0xf7,0,0};
		jpeg.insert(jpeg.begin()+2,app.begin(),app.end());
		CODEC_VIS_CHECK(decode(jpeg,"misleading.jls").width==width);
		std::cout<<"PASS existing PNG/JPEG/JXL and JPEG XS signature dispatch"<<std::endl;
		for(const auto& ext:supported_input_extensions()) if(ext!=".nef") {
			bool rejected=false;try{(void)decode_input_image(std::vector<uint8_t>{0,1,2,3},"bad"+ext);}catch(const std::exception&){rejected=true;}
			CODEC_VIS_CHECK(rejected);
		}
		bool rejected=false;try{(void)decode_input_image({});}catch(const std::exception&){rejected=true;}CODEC_VIS_CHECK(rejected);
	} catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
