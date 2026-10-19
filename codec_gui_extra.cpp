#include "codec_gui_extra.hpp"
#include "gui/raw_image_utils.hpp"
#include "gui/codec_timer.hpp"

#include <kvazaar.h>
#include <rav1e.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <webp/encode.h>
#include <webp/decode.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace codec_gui {
namespace {
using namespace gui;

template<class T> T value(std::span<const EncoderParam> params, std::string_view name, T fallback) {
	for (const auto& p : params) if (p.name == name) {
		if (const auto* v = std::get_if<T>(&p.value)) return *v;
		throw std::invalid_argument("Incorrect parameter type: " + p.name);
	}
	return fallback;
}
int integer(std::span<const EncoderParam> params, std::string_view name, int fallback, int low, int high) {
	const auto n = value<int64_t>(params, name, fallback);
	if (n < low || n > high) throw std::invalid_argument("Parameter out of range: " + std::string(name));
	return static_cast<int>(n);
}
void validate(const RawImage& image, bool rgb = false) {
	if (image.width <= 0 || image.height <= 0 || image.width > 65535 || image.height > 65535)
		throw std::invalid_argument("Image dimensions must be between 1 and 65535");
	if (is_rgb(image.format) != rgb) throw std::invalid_argument(rgb ? "Encoder requires planar RGB input" : "Encoder requires planar YUV input");
	if (((is_420(image.format) || is_422(image.format)) && (image.width & 1)) || (is_420(image.format) && (image.height & 1)))
		throw std::invalid_argument("Subsampled input requires chroma-aligned dimensions");
	for (int p = 0; p < plane_count(image.format); ++p) {
		const int row = plane_width(image,p) * bytes_per_sample(image.format);
		const auto& plane = image.planes[p];
		if (plane.strideBytes < row || plane.bytes.size() < std::size_t(plane.strideBytes) * (plane_height(image,p)-1) + row)
			throw std::invalid_argument("Incomplete input plane");
	}
}
void known(std::span<const EncoderParam> params, std::initializer_list<std::string_view> names) {
	for (const auto& p : params) if (std::find(names.begin(), names.end(), p.name) == names.end())
		throw std::invalid_argument("Unknown encoder parameter: " + p.name);
}
EncoderParamInfo number(std::string name, std::string label, std::string group, int def, int low, int high) {
	EncoderParamInfo p;
	p.name = std::move(name); p.label = std::move(label); p.group = std::move(group);
	p.kind = ParamKind::Int; p.defaultValue = int64_t(def); p.intRange = IntRange{.min=low,.max=high,.step=1};
	return p;
}
EncoderParamInfo toggle(std::string name, std::string label, std::string help) {
	EncoderParamInfo p;
	p.name=std::move(name); p.label=std::move(label); p.group="Rate Control";
	p.kind=ParamKind::Bool; p.defaultValue=false; p.help=std::move(help);
	return p;
}
void vpx_check(vpx_codec_err_t error, const vpx_codec_ctx_t* context, const char* op) {
	if (error != VPX_CODEC_OK) throw std::runtime_error(std::string("libvpx ") + op + ": " +
		(context ? vpx_codec_error(context) : vpx_codec_err_to_string(error)));
}
struct VpxContext {
	vpx_codec_ctx_t ctx{};
	~VpxContext() { if (ctx.priv) vpx_codec_destroy(&ctx); }
};

EncodedImage encode_vpx(const RawImage& image, std::span<const EncoderParam> params, bool vp9) {
	known(params, vp9 ? std::initializer_list<std::string_view>{"quality","speed","threads","lossless"} : std::initializer_list<std::string_view>{"quality","speed","threads"});
	validate(image);
	const int depth = bit_depth(image.format);
	if (is_gray(image.format) || (depth != 8 && depth != 10 && depth != 12) || (!vp9 && image.format != PixelFormat::YUV420P8))
		throw std::invalid_argument("libvpx input format is not supported by this backend");
	const int quality = integer(params,"quality",32,0,63);
	const int speed = integer(params,"speed",vp9 ? 4 : 6,0,vp9 ? 9 : 16);
	const int threads = integer(params,"threads",4,1,64);
	const bool lossless = vp9 && value<bool>(params,"lossless",false);
	vpx_codec_iface_t* iface = vp9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();
	vpx_codec_enc_cfg_t cfg{};
	vpx_check(vpx_codec_enc_config_default(iface,&cfg,0),nullptr,"defaults");
	cfg.g_w=image.width; cfg.g_h=image.height; cfg.g_timebase={1,1}; cfg.g_threads=threads;
	cfg.g_lag_in_frames=0; cfg.g_pass=VPX_RC_ONE_PASS;
	cfg.rc_end_usage=VPX_Q; cfg.rc_min_quantizer=lossless ? 0 : quality; cfg.rc_max_quantizer=lossless ? 0 : quality;
	cfg.kf_mode=VPX_KF_DISABLED;
	cfg.g_profile=vp9 ? (depth > 8 ? 2 : 0) + (is_420(image.format) ? 0 : 1) : 0;
	cfg.g_bit_depth=static_cast<vpx_bit_depth_t>(depth); cfg.g_input_bit_depth=depth;
	VpxContext encoder;
	vpx_check(vpx_codec_enc_init(&encoder.ctx,iface,&cfg,depth > 8 ? VPX_CODEC_USE_HIGHBITDEPTH : 0),&encoder.ctx,"initialize encoder");
	vpx_check(vpx_codec_control(&encoder.ctx,VP8E_SET_CPUUSED,speed),&encoder.ctx,"set speed");
	vpx_check(vpx_codec_control(&encoder.ctx,VP8E_SET_CQ_LEVEL,quality),&encoder.ctx,"set quality");
	if (vp9) {
		vpx_check(vpx_codec_control(&encoder.ctx,VP9E_SET_LOSSLESS,unsigned(lossless)),&encoder.ctx,"set lossless");
		const int cs=image.color.matrix == MatrixCoefficients::BT2020NonConstant ? VPX_CS_BT_2020 : image.color.matrix == MatrixCoefficients::BT601 ? VPX_CS_BT_601 : VPX_CS_BT_709;
		vpx_check(vpx_codec_control(&encoder.ctx,VP9E_SET_COLOR_SPACE,cs),&encoder.ctx,"set color space");
		vpx_check(vpx_codec_control(&encoder.ctx,VP9E_SET_COLOR_RANGE,int(image.color.range == ColorRange::Full)),&encoder.ctx,"set range");
		vpx_check(vpx_codec_control(&encoder.ctx,VP9E_SET_ROW_MT,1u),&encoder.ctx,"set row threading");
	}
	vpx_img_fmt_t fmt=is_420(image.format) ? VPX_IMG_FMT_I420 : is_422(image.format) ? VPX_IMG_FMT_I422 : VPX_IMG_FMT_I444;
	if (depth > 8) fmt=static_cast<vpx_img_fmt_t>(fmt | VPX_IMG_FMT_HIGHBITDEPTH);
	std::unique_ptr<vpx_image_t,decltype(&vpx_img_free)> input(vpx_img_alloc(nullptr,fmt,image.width,image.height,32),vpx_img_free);
	if (!input) throw std::bad_alloc();
	input->bit_depth=depth;
	for(int p=0;p<3;++p) for(int y=0;y<plane_height(image,p);++y)
		std::memcpy(input->planes[p]+std::ptrdiff_t(y)*input->stride[p],image.planes[p].bytes.data()+std::size_t(y)*image.planes[p].strideBytes,plane_width(image,p)*bytes_per_sample(image.format));
	vpx_check(vpx_codec_encode(&encoder.ctx,input.get(),0,1,VPX_EFLAG_FORCE_KF,VPX_DL_GOOD_QUALITY),&encoder.ctx,"encode");
	std::vector<uint8_t> bytes;
	auto drain=[&] {
		vpx_codec_iter_t iter=nullptr;
		while(const auto* packet=vpx_codec_get_cx_data(&encoder.ctx,&iter)) if(packet->kind==VPX_CODEC_CX_FRAME_PKT) {
			if(!bytes.empty()) throw std::runtime_error("libvpx returned more than one still frame");
			const auto* data=static_cast<const uint8_t*>(packet->data.frame.buf);
			bytes.assign(data,data+packet->data.frame.sz);
		}
	};
	drain();
	vpx_check(vpx_codec_encode(&encoder.ctx,nullptr,1,1,0,VPX_DL_GOOD_QUALITY),&encoder.ctx,"flush");
	drain();
	if(bytes.empty()) throw std::runtime_error("libvpx produced no frame");
	return vp9 ? wrap_still_ivf(bytes,image,"VP90") : wrap_still_ivf(bytes,image,"VP80");
}
} // namespace

EncodedImage wrap_still_ivf(std::span<const uint8_t> bytes, const RawImage& image, const char (&fourcc)[5]) {
	if(bytes.empty() || bytes.size()>std::numeric_limits<uint32_t>::max() || image.width<=0 || image.height<=0 || image.width>65535 || image.height>65535)
		throw std::runtime_error("Invalid still IVF payload or dimensions");
	EncodedImage out;
	auto le=[&](uint64_t n,int count) {for(int i=0;i<count;++i) out.hevcAnnexB.push_back(std::byte((n>>(8*i))&255));};
	for(char c:std::string_view("DKIF")) out.hevcAnnexB.push_back(std::byte(c));
	le(0,2); le(32,2);
	for(int i=0;i<4;++i) out.hevcAnnexB.push_back(std::byte(fourcc[i]));
	le(image.width,2); le(image.height,2); le(1,4); le(1,4); le(1,4); le(0,4);
	le(bytes.size(),4); le(0,8);
	const auto* first=reinterpret_cast<const std::byte*>(bytes.data());
	out.hevcAnnexB.insert(out.hevcAnnexB.end(),first,first+bytes.size());
	out.codedColor=image.color;
	return out;
}

std::vector<EncoderParamInfo> query_kvazaar_parameters() {
	EncoderParamInfo preset;
	preset.name="preset"; preset.label="Preset"; preset.group="Speed / Quality"; preset.kind=ParamKind::Enum; preset.defaultValue=std::string("medium");
	for (const char* s : {"ultrafast","superfast","veryfast","faster","fast","medium","slow","slower","veryslow","placebo"}) preset.enumValues.push_back({.value=s,.label=s});
	auto qp=number("qp","QP","Rate Control",32,0,51);
	qp.enabledWhen={{.parameter="lossless",.acceptedValues={"false"},.explanation="Lossless coding does not use QP."}};
	return {qp,toggle("lossless","Lossless","Reversible coding of the supplied YUV samples; QP is ignored."),preset,number("threads","Threads","Execution",4,1,64)};
}
EncodedImage encode_kvazaar_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	known(params,{"qp","preset","lossless","threads"}); validate(image);
	if(image.width<8 || image.height<8 || (image.width&7) || (image.height&7))
		throw std::invalid_argument("Kvazaar requires width and height divisible by 8 and at least 8 pixels");
	if(bit_depth(image.format)!=8 || (!is_420(image.format) && !is_gray(image.format)))
		throw std::invalid_argument("Kvazaar requires 8-bit 4:2:0 or monochrome input");
	const kvz_api* api=kvz_api_get(8);
	if(!api) throw std::runtime_error("Kvazaar 8-bit API unavailable");
	std::unique_ptr<kvz_config,decltype(api->config_destroy)> cfg(api->config_alloc(),api->config_destroy);
	if(!cfg || !api->config_init(cfg.get())) throw std::runtime_error("Kvazaar configuration allocation failed");
	const auto preset=value<std::string>(params,"preset","medium");
	if(!api->config_parse(cfg.get(),"preset",preset.c_str())) throw std::invalid_argument("Invalid Kvazaar preset");
	cfg->width=image.width; cfg->height=image.height; cfg->qp=integer(params,"qp",32,0,51);
	cfg->threads=integer(params,"threads",4,1,64); cfg->owf=0; cfg->intra_period=1; cfg->vps_period=1;
	cfg->lossless=value<bool>(params,"lossless",false); cfg->add_encoder_info=0;
	const char* sampling=is_gray(image.format) ? "P400" : is_444(image.format) ? "P444" : "P420";
	if(!api->config_parse(cfg.get(),"input-format",sampling)) throw std::invalid_argument("Kvazaar input format unsupported");
	cfg->vui.fullrange=image.color.range==ColorRange::Full;
	cfg->vui.colorprim=static_cast<int>(image.color.primaries); cfg->vui.transfer=static_cast<int>(image.color.transfer);
	cfg->vui.colormatrix=static_cast<int>(image.color.matrix);
	if(image.color.chroma420Location) cfg->vui.chroma_loc=static_cast<int>(*image.color.chroma420Location);
	std::unique_ptr<kvz_encoder,decltype(api->encoder_close)> encoder(api->encoder_open(cfg.get()),api->encoder_close);
	if(!encoder) throw std::runtime_error("Kvazaar rejected encoder configuration");
	const auto csp=is_gray(image.format) ? KVZ_CSP_400 : is_444(image.format) ? KVZ_CSP_444 : KVZ_CSP_420;
	std::unique_ptr<kvz_picture,decltype(api->picture_free)> input(api->picture_alloc_csp(csp,image.width,image.height),api->picture_free);
	if(!input) throw std::bad_alloc();
	for(int p=0;p<plane_count(image.format);++p) for(int y=0;y<plane_height(image,p);++y)
		std::memcpy(input->data[p]+std::ptrdiff_t(y)*(p && is_420(image.format) ? input->stride/2 : input->stride),image.planes[p].bytes.data()+std::size_t(y)*image.planes[p].strideBytes,plane_width(image,p));
	input->pts=0;
	EncodedImage out;
	for(int attempt=0;attempt<32;++attempt) {
		kvz_data_chunk* chunks=nullptr; uint32_t size=0;
		const int ok=api->encoder_encode(encoder.get(),attempt==0 ? input.get() : nullptr,&chunks,&size,nullptr,nullptr,nullptr);
		std::unique_ptr<kvz_data_chunk,decltype(api->chunk_free)> owner(chunks,api->chunk_free);
		if(!ok) throw std::runtime_error("Kvazaar encoding failed");
		for(auto* chunk=chunks;chunk;chunk=chunk->next) {
			const auto* data=reinterpret_cast<const std::byte*>(chunk->data);
			out.hevcAnnexB.insert(out.hevcAnnexB.end(),data,data+chunk->len);
		}
		if(size==0 && attempt>0) break;
		if(attempt==31) throw std::runtime_error("Kvazaar did not finish the still frame");
	}
	if(out.hevcAnnexB.empty()) throw std::runtime_error("Kvazaar produced no frame");
	out.codedColor=image.color;
	return out;
}

std::vector<EncoderParamInfo> query_webp_parameters() {
	return {toggle("lossless","Lossless","Preserve the 8-bit RGB input exactly. Lossy mode uses WebP's internal 4:2:0 conversion."),
		number("quality","Quality","Rate Control",75,0,100),number("method","Compression effort","Speed / Quality",4,0,6)};
}
EncodedImage encode_webp_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	known(params,{"lossless","quality","method"}); validate(image,true);
	if(image.format!=PixelFormat::RGBP8 || image.color.primaries!=ColorPrimaries::BT709 || image.color.transfer!=TransferCharacteristics::SRGB || image.color.range!=ColorRange::Full)
		throw std::invalid_argument("WebP requires 8-bit full-range sRGB input");
	if(image.width>WEBP_MAX_DIMENSION || image.height>WEBP_MAX_DIMENSION) throw std::invalid_argument("WebP dimensions exceed 16383 pixels");
	WebPConfig cfg;
	if(!WebPConfigInit(&cfg)) throw std::runtime_error("WebP configuration ABI mismatch");
	cfg.lossless=value<bool>(params,"lossless",false);
	cfg.quality=integer(params,"quality",75,0,100); cfg.method=integer(params,"method",4,0,6); cfg.exact=1;
	WebPPicture picture;
	if(!WebPPictureInit(&picture)) throw std::runtime_error("WebP picture ABI mismatch");
	struct FreePicture { WebPPicture* p; ~FreePicture(){WebPPictureFree(p);} } freePicture{&picture};
	picture.width=image.width; picture.height=image.height; picture.use_argb=1;
	std::vector<uint8_t> rgb(std::size_t(image.width)*image.height*3);
	for(int y=0;y<image.height;++y) for(int x=0;x<image.width;++x) for(int p=0;p<3;++p)
		rgb[(std::size_t(y)*image.width+x)*3+p]=image.planes[p].bytes[std::size_t(y)*image.planes[p].strideBytes+x];
	if(!WebPPictureImportRGB(&picture,rgb.data(),image.width*3)) throw std::bad_alloc();
	WebPMemoryWriter writer; WebPMemoryWriterInit(&writer);
	struct FreeWriter { WebPMemoryWriter* w; ~FreeWriter(){WebPMemoryWriterClear(w);} } freeWriter{&writer};
	picture.writer=WebPMemoryWrite; picture.custom_ptr=&writer;
	if(!WebPValidateConfig(&cfg) || !WebPEncode(&cfg,&picture)) throw std::runtime_error("WebP encode failed: " + std::to_string(picture.error_code));
	EncodedImage out; const auto* first=reinterpret_cast<const std::byte*>(writer.mem);
	out.hevcAnnexB.assign(first,first+writer.size); out.codedColor=image.color;
	return out;
}
std::shared_ptr<const RawImage> decode_webp_image(const EncodedImage& encoded,double* codecSeconds) {
	int w=0,h=0;
	gui::CodecTimer timer;
	std::unique_ptr<uint8_t,decltype(&WebPFree)> rgb(timer.measure([&] {return WebPDecodeRGB(reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data()),encoded.hevcAnnexB.size(),&w,&h);}),WebPFree);
	if(!rgb) throw std::runtime_error("libwebp rejected the encoded image");
	if(codecSeconds) *codecSeconds=timer.seconds();
	auto image=std::make_shared<RawImage>(); image->width=w; image->height=h; image->format=PixelFormat::RGBP8;
	image->color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,.matrix=MatrixCoefficients::Identity,.range=ColorRange::Full};
	for(auto& p:image->planes) {p.strideBytes=w; p.bytes.resize(std::size_t(w)*h);}
	for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int p=0;p<3;++p) image->planes[p].bytes[std::size_t(y)*w+x]=rgb.get()[(std::size_t(y)*w+x)*3+p];
	return image;
}

std::vector<EncoderParamInfo> query_vpx_vp8_parameters() {
	return {number("quality","Quantizer","Rate Control",32,0,63),number("speed","Speed","Speed / Quality",6,0,16),number("threads","Threads","Execution",4,1,64)};
}
std::vector<EncoderParamInfo> query_vpx_vp9_parameters() {
	auto quality=number("quality","Quantizer","Rate Control",32,0,63);
	quality.enabledWhen={{.parameter="lossless",.acceptedValues={"false"},.explanation="Lossless coding does not use a quantizer."}};
	return {quality,toggle("lossless","Lossless","Preserve the supplied YUV samples exactly; quantizer is ignored."),
		number("speed","Speed","Speed / Quality",4,0,9),number("threads","Threads","Execution",4,1,64)};
}
EncodedImage encode_vpx_vp8_still_image(const RawImage& image,std::span<const EncoderParam> params) {return encode_vpx(image,params,false);}
EncodedImage encode_vpx_vp9_still_image(const RawImage& image,std::span<const EncoderParam> params) {return encode_vpx(image,params,true);}

std::shared_ptr<const RawImage> decode_vpx_image(const EncodedImage& encoded, bool vp9,double* codecSeconds) {
	const auto& bytes=encoded.hevcAnnexB;
	if(bytes.size()<44 || std::memcmp(bytes.data(),"DKIF",4)!=0 || std::memcmp(bytes.data()+8,vp9 ? "VP90" : "VP80",4)!=0)
		throw std::runtime_error("Invalid VP8/VP9 IVF header");
	auto le=[&](int at,int count) {uint32_t n=0;for(int i=0;i<count;++i)n|=uint32_t(std::to_integer<uint8_t>(bytes[at+i]))<<(8*i);return n;};
	const uint32_t header=le(6,2);
	if(header<32 || std::size_t(header)+12>bytes.size()) throw std::runtime_error("Truncated IVF header");
	const uint32_t size=le(header,4);
	if(size==0 || size>bytes.size()-header-12) throw std::runtime_error("Truncated IVF frame");
	VpxContext decoder; vpx_codec_dec_cfg_t cfg{}; cfg.threads=1;
	vpx_check(vpx_codec_dec_init(&decoder.ctx,vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(),&cfg,0),&decoder.ctx,"initialize decoder");
	const auto codecStart=std::chrono::steady_clock::now();
	vpx_check(vpx_codec_decode(&decoder.ctx,reinterpret_cast<const uint8_t*>(bytes.data()+header+12),size,nullptr,0),&decoder.ctx,"decode");
	vpx_codec_iter_t iter=nullptr; const auto* frame=vpx_codec_get_frame(&decoder.ctx,&iter);
	if(!frame) throw std::runtime_error("libvpx produced no decoded frame");
	int corrupted=0;
	vpx_check(vpx_codec_control(&decoder.ctx,VP8D_GET_FRAME_CORRUPTED,&corrupted),&decoder.ctx,"check decoded frame");
	if(corrupted) throw std::runtime_error("libvpx reported a corrupted frame");
	if(codecSeconds) *codecSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
	auto image=std::make_shared<RawImage>(); image->width=frame->d_w; image->height=frame->d_h;
	const int depth=frame->bit_depth;
	if(depth!=8 && depth!=10 && depth!=12) throw std::runtime_error("Unexpected libvpx decoded precision");
	if(frame->x_chroma_shift==0 && frame->y_chroma_shift==0) image->format=pixel_format_for(depth,true);
	else if(frame->x_chroma_shift==1 && frame->y_chroma_shift==1) image->format=pixel_format_for(depth,false);
	else if(frame->x_chroma_shift==1 && frame->y_chroma_shift==0) image->format=depth==8 ? PixelFormat::YUV422P8 : depth==10 ? PixelFormat::YUV422P10LE : PixelFormat::YUV422P12LE;
	else throw std::runtime_error("Unsupported libvpx chroma layout");
	// VP8/VP9 cannot express a complete CICP description. Preserve known
	// application context rather than inventing missing primaries/transfer.
	if(encoded.codedColor) image->color=*encoded.codedColor;
	else {
		image->color.matrix=frame->cs==VPX_CS_BT_2020 ? MatrixCoefficients::BT2020NonConstant : frame->cs==VPX_CS_BT_709 ? MatrixCoefficients::BT709 : MatrixCoefficients::BT601;
		image->color.range=frame->range==VPX_CR_FULL_RANGE ? ColorRange::Full : ColorRange::Limited;
	}
	for(int p=0;p<3;++p) {
		const int row=plane_width(*image,p)*bytes_per_sample(image->format);
		image->planes[p].strideBytes=row; image->planes[p].bytes.resize(std::size_t(row)*plane_height(*image,p));
		for(int y=0;y<plane_height(*image,p);++y) std::memcpy(image->planes[p].bytes.data()+std::size_t(y)*row,frame->planes[p]+std::ptrdiff_t(y)*frame->stride[p],row);
	}
	return image;
}

std::vector<EncoderParamInfo> query_rav1e_parameters() {
	EncoderParamInfo tune;
	tune.name="tune"; tune.label="Tune"; tune.group="Speed / Quality"; tune.kind=ParamKind::Enum;
	tune.defaultValue=std::string("psychovisual"); tune.enumValues={{.value="psychovisual",.label="Psychovisual"},{.value="psnr",.label="PSNR"}};
	return {number("quantizer","Quantizer","Rate Control",100,0,255),number("speed","Speed","Speed / Quality",6,0,10),tune,number("threads","Threads","Execution",4,1,64)};
}
EncodedImage encode_rav1e_still_image(const RawImage& image,std::span<const EncoderParam> params) {
	known(params,{"quantizer","speed","threads","tune"}); validate(image);
	const int depth=bit_depth(image.format);
	if(depth!=8 && depth!=10 && depth!=12) throw std::invalid_argument("rav1e supports 8-, 10-, and 12-bit input");
	std::unique_ptr<RaConfig,decltype(&rav1e_config_unref)> cfg(rav1e_config_default(),rav1e_config_unref);
	if(!cfg) throw std::bad_alloc();
	auto set=[&](const char* name,std::string value) {if(rav1e_config_parse(cfg.get(),name,value.c_str())<0) throw std::invalid_argument(std::string("rav1e rejected ")+name);};
	set("width",std::to_string(image.width)); set("height",std::to_string(image.height));
	set("speed",std::to_string(integer(params,"speed",6,0,10)));
	set("quantizer",std::to_string(integer(params,"quantizer",100,0,255)));
	set("threads",std::to_string(integer(params,"threads",4,1,64)));
	set("tune",value<std::string>(params,"tune","psychovisual"));
	set("still_picture","true"); set("low_latency","true"); set("rdo_lookahead_frames","1");
	const auto sampling=is_gray(image.format) ? RA_CHROMA_SAMPLING_CS400 : is_420(image.format) ? RA_CHROMA_SAMPLING_CS420 : is_422(image.format) ? RA_CHROMA_SAMPLING_CS422 : RA_CHROMA_SAMPLING_CS444;
	RaChromaSamplePosition position=RA_CHROMA_SAMPLE_POSITION_UNKNOWN;
	if(image.color.chroma420Location==Chroma420SampleLocation::LeftCenter) position=RA_CHROMA_SAMPLE_POSITION_VERTICAL;
	if(image.color.chroma420Location==Chroma420SampleLocation::TopLeft) position=RA_CHROMA_SAMPLE_POSITION_COLOCATED;
	if(rav1e_config_set_pixel_format(cfg.get(),depth,sampling,position,image.color.range==ColorRange::Full ? RA_PIXEL_RANGE_FULL : RA_PIXEL_RANGE_LIMITED)<0 ||
	   rav1e_config_set_color_description(cfg.get(),static_cast<RaMatrixCoefficients>(image.color.matrix),static_cast<RaColorPrimaries>(image.color.primaries),static_cast<RaTransferCharacteristics>(image.color.transfer))<0)
		throw std::invalid_argument("rav1e rejected input color description");
	std::unique_ptr<RaContext,decltype(&rav1e_context_unref)> encoder(rav1e_context_new(cfg.get()),rav1e_context_unref);
	if(!encoder) throw std::runtime_error("rav1e rejected encoder configuration");
	std::unique_ptr<RaFrame,decltype(&rav1e_frame_unref)> frame(rav1e_frame_new(encoder.get()),rav1e_frame_unref);
	if(!frame) throw std::bad_alloc();
	for(int p=0;p<plane_count(image.format);++p) rav1e_frame_fill_plane(frame.get(),p,image.planes[p].bytes.data(),image.planes[p].bytes.size(),image.planes[p].strideBytes,bytes_per_sample(image.format));
	if(rav1e_send_frame(encoder.get(),frame.get())!=RA_ENCODER_STATUS_SUCCESS || rav1e_send_frame(encoder.get(),nullptr)!=RA_ENCODER_STATUS_SUCCESS)
		throw std::runtime_error("rav1e rejected still frame or flush");
	for(int attempt=0;attempt<32;++attempt) {
		RaPacket* raw=nullptr; const auto status=rav1e_receive_packet(encoder.get(),&raw);
		std::unique_ptr<RaPacket,decltype(&rav1e_packet_unref)> packet(raw,rav1e_packet_unref);
		if(status==RA_ENCODER_STATUS_SUCCESS && packet) return wrap_still_ivf({packet->data,packet->len},image,"AV01");
		if(status!=RA_ENCODER_STATUS_ENCODED) throw std::runtime_error("rav1e produced no still packet, status="+std::to_string(status));
	}
	throw std::runtime_error("rav1e did not finish the still frame");
}
} // namespace codec_gui
