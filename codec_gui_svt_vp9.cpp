#include "codec_gui_extra.hpp"
#include "gui/raw_image_utils.hpp"
#include <EbSvtVp9Enc.h>
#include <cstring>
#include <stdexcept>

namespace codec_gui {
namespace {
void checked(EbErrorType error, const char* operation) {
	if(error!=EB_ErrorNone) throw std::runtime_error(std::string("SVT-VP9 ")+operation+" failed: "+std::to_string(static_cast<int>(error)));
}
int integer(std::span<const EncoderParam> params, const char* key, int fallback, int low, int high) {
	for(const auto& p:params) if(p.name==key) {
		const auto* n=std::get_if<int64_t>(&p.value);
		if(!n || *n<low || *n>high) throw std::invalid_argument(std::string("Invalid SVT-VP9 parameter: ")+key);
		return static_cast<int>(*n);
	}
	return fallback;
}
EncoderParamInfo number(const char* name,const char* label,const char* group,int def,int low,int high) {
	EncoderParamInfo p; p.name=name; p.label=label; p.group=group; p.kind=ParamKind::Int;
	p.defaultValue=int64_t(def); p.intRange=IntRange{.min=low,.max=high,.step=1};
	if(p.name=="tune") p.help="0: visually optimized; 1: PSNR/SSIM optimized; 2: VMAF optimized. This selects the encoder's optimization target, not the application's quality metric.";
	return p;
}
} // namespace
std::vector<EncoderParamInfo> query_svt_vp9_parameters() {
	return {number("qp","QP","Rate Control",32,0,63),number("preset","Preset","Speed / Quality",9,0,9),
		number("tune","Tune","Speed / Quality",1,0,2),number("threads","Logical processors","Execution",4,1,64)};
}
EncodedImage encode_svt_vp9_still_image(const RawImage& image,std::span<const EncoderParam> params) {
	using namespace gui;
	for(const auto& p:params) if(p.name!="qp" && p.name!="preset" && p.name!="tune" && p.name!="threads") throw std::invalid_argument("Unknown SVT-VP9 parameter: "+p.name);
	// The installed SVT-VP9 0.3.1 accepts larger dimensions during config,
	// but corrupts the heap on a 5600x3728 single-frame encode and crashes in
	// eb_vp9_deinit_encoder. Do not enter that unsafe path from the application.
	if(image.width>3840 || image.height>2160)
		throw std::invalid_argument("SVT-VP9 cannot safely encode this image size with the installed library. Use libvpx VP9 for images above 3840 x 2160.");
	if(image.format!=PixelFormat::YUV420P8 || image.width<64 || image.height<64 || (image.width&7) || (image.height&7))
		throw std::invalid_argument("SVT-VP9 requires 8-bit 4:2:0 dimensions divisible by 8, width 64..3840 and height 64..2160");
	if(image.color.range!=ColorRange::Limited) throw std::invalid_argument("SVT-VP9 only signals limited range");
	for(int p=0;p<3;++p) {
		const auto& plane=image.planes[p]; const int row=plane_width(image,p);
		if(plane.strideBytes<row || plane.bytes.size()<std::size_t(plane.strideBytes)*(plane_height(image,p)-1)+row)
			throw std::invalid_argument("SVT-VP9 incomplete input plane");
	}
	EbSvtVp9EncConfiguration cfg{};
	EbComponentType* encoder=nullptr;
	struct Close {EbComponentType*& e; bool initialized=false; ~Close(){if(e){if(initialized)eb_vp9_deinit_encoder(e);eb_vp9_deinit_handle(e);}}} close{encoder};
	checked(eb_vp9_svt_init_handle(&encoder,nullptr,&cfg),"allocate encoder");
	cfg.source_width=image.width; cfg.source_height=image.height; cfg.encoder_bit_depth=8;
	cfg.enc_mode=integer(params,"preset",9,0,9); cfg.qp=integer(params,"qp",32,0,63);
	cfg.tune=integer(params,"tune",1,0,2); cfg.logical_processors=integer(params,"threads",4,1,64);
	cfg.intra_period=0; cfg.rate_control_mode=0;
	cfg.frames_to_be_encoded=1; cfg.frame_rate=1; cfg.target_socket=-1;
	checked(eb_vp9_svt_enc_set_parameter(encoder,&cfg),"configure");
	checked(eb_vp9_init_encoder(encoder),"initialize"); close.initialized=true;
	EbSvtEncInput pixels{};
	pixels.luma=const_cast<uint8_t*>(image.planes[0].bytes.data()); pixels.cb=const_cast<uint8_t*>(image.planes[1].bytes.data()); pixels.cr=const_cast<uint8_t*>(image.planes[2].bytes.data());
	pixels.y_stride=image.planes[0].strideBytes; pixels.cb_stride=image.planes[1].strideBytes; pixels.cr_stride=image.planes[2].strideBytes;
	EbBufferHeaderType input{}; input.size=sizeof(input); input.p_buffer=reinterpret_cast<uint8_t*>(&pixels);
	input.n_alloc_len=sizeof(pixels); input.n_filled_len=image.width*image.height*3/2;
	checked(eb_vp9_svt_enc_send_picture(encoder,&input),"send frame");
	EbBufferHeaderType eos{}; eos.size=sizeof(eos); eos.flags=EB_BUFFERFLAG_EOS;
	checked(eb_vp9_svt_enc_send_picture(encoder,&eos),"flush");
	std::vector<uint8_t> bytes;
	for(;;) {
		EbBufferHeaderType* packet=nullptr;
		checked(eb_vp9_svt_get_packet(encoder,&packet,1),"receive packet");
		struct Release {EbBufferHeaderType*& p; ~Release(){if(p)eb_vp9_svt_release_out_buffer(&p);}} release{packet};
		if(!packet) throw std::runtime_error("SVT-VP9 returned no packet");
		if(packet->n_filled_len) {
			if(!bytes.empty() || !packet->p_buffer) throw std::runtime_error("SVT-VP9 returned an invalid still packet");
			bytes.assign(packet->p_buffer,packet->p_buffer+packet->n_filled_len);
		}
		if(packet->flags&EB_BUFFERFLAG_EOS) break;
	}
	return wrap_still_ivf(bytes,image,"VP90");
}
} // namespace codec_gui
