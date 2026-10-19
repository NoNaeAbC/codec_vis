#include "encoder_backends.hpp"
#include "encoder_param_state.hpp"
#include "raw_image_utils.hpp"
#include "raw_image_conversion.hpp"
#include "test_check.hpp"
#include "../codec_gui_image_io.hpp"

#include <cmath>
#include <iostream>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {
RawImage pattern(bool rgb=false, int w=128, int h=96) {
	RawImage image;
	image.width=w; image.height=h; image.format=rgb ? PixelFormat::RGBP8 : PixelFormat::YUV444P12LE;
	image.color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,
		.matrix=rgb ? MatrixCoefficients::Identity : MatrixCoefficients::BT709,.range=rgb ? ColorRange::Full : ColorRange::Limited};
	for(int p=0;p<3;++p) {
		image.planes[p].strideBytes=w*bytes_per_sample(image.format)+8;
		image.planes[p].bytes.resize(std::size_t(image.planes[p].strideBytes)*h);
		for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
			const int q=rgb ? (x*13+y*7+p*43)&255 : (p ? 120+(x/3+y/5+p*3)%16 : 32+(x*3+y*5)%192)*16+(x+y)%16;
			put_sample(image.planes[p],x,y,bytes_per_sample(image.format),q,max_sample_value(image.format));
		}
	}
	return image;
}
void set(std::vector<EncoderParam>& params,std::string name,ParamValue value) {
	for(auto& p:params) if(p.name==name) {p.value=std::move(value);return;}
	params.push_back({.name=std::move(name),.value=std::move(value)});
}
std::vector<EncoderParam> defaults(const EncoderBackend& b) {
	std::vector<EncoderParam> params;
	for(const auto& p:b.queryCapabilities().params) params.push_back({.name=p.name,.value=resolved_param_value(AppState{},b.id,p)});
	return params;
}
void equal_samples(const RawImage& a,const RawImage& b) {
	CODEC_VIS_CHECK(a.width==b.width && a.height==b.height && a.format==b.format);
	for(int p=0;p<plane_count(a.format);++p) for(int y=0;y<plane_height(a,p);++y) for(int x=0;x<plane_width(a,p);++x)
		CODEC_VIS_CHECK(sample_at(a.planes[p],x,y,bytes_per_sample(a.format))==sample_at(b.planes[p],x,y,bytes_per_sample(b.format)));
}
void run(const EncoderBackend& b,const RawImage& source,const std::vector<EncoderParam>& params,bool lossless=false) {
	const auto result=run_backend_encode(b,source,params);
	CODEC_VIS_CHECK(!result.encoded.hevcAnnexB.empty());
	CODEC_VIS_CHECK(!result.encoded.previewImage); // Decode the actual bytes.
	CODEC_VIS_CHECK(result.metadata.encodeSeconds>0 && std::isfinite(result.metadata.encodeSeconds));
	CODEC_VIS_CHECK(result.metadata.metricState==MetricState::NotRequested && result.metadata.metrics.empty());
	const auto decoded=b.decodePreview(result.encoded);
	if(!decoded.image) throw std::runtime_error(b.name+": "+decoded.error);
	CODEC_VIS_CHECK(decoded.image->width==result.comparisonReference->width && decoded.image->height==result.comparisonReference->height);
	CODEC_VIS_CHECK(decoded.image->format==result.comparisonReference->format);
	CODEC_VIS_CHECK(decoded.image->color.primaries==result.comparisonReference->color.primaries);
	CODEC_VIS_CHECK(decoded.image->color.transfer==result.comparisonReference->color.transfer);
	CODEC_VIS_CHECK(decoded.image->color.matrix==result.comparisonReference->color.matrix);
	CODEC_VIS_CHECK(decoded.image->color.range==result.comparisonReference->color.range);
	const auto linear=raw_image_to_linear_bt2020(*decoded.image);
	for(const auto& plane:linear.planes) for(float v:plane) CODEC_VIS_CHECK(std::isfinite(v));
	const auto imported=decode_input_image({reinterpret_cast<const unsigned char*>(result.encoded.hevcAnnexB.data()),result.encoded.hevcAnnexB.size()},b.codec=="HEVC" ? "source.HEVC" : "renamed.data");
	equal_samples(*decoded.image,imported);
	if(lossless) equal_samples(*result.comparisonReference,*decoded.image);
	if(lossless && b.id.value==17) equal_samples(source,*decoded.image);
	RawImage incomplete=*result.comparisonReference;
	incomplete.planes[0].bytes.resize(incomplete.planes[0].strideBytes*(incomplete.height-1));
	bool rejected=false;
	try {(void)b.encode(incomplete,{});} catch(const std::invalid_argument&) {rejected=true;}
	CODEC_VIS_CHECK(rejected);
	auto truncated=result.encoded;
	truncated.hevcAnnexB.resize(std::min<std::size_t>(truncated.hevcAnnexB.size(),16));
	CODEC_VIS_CHECK(!b.decodePreview(truncated).image);
	std::cout<<"PASS "<<b.name<<" "<<decoded.image->width<<"x"<<decoded.image->height<<" depth="<<bit_depth(decoded.image->format)
		<<" format="<<static_cast<int>(decoded.image->format)<<" lossless="<<lossless<<" bytes="<<result.encoded.hevcAnnexB.size()<<std::endl;
}
}

int main(int argc,char** argv) {
	try {
		const auto backends=initial_encoder_backends(std::span<const VaapiDeviceInfo>{});
		for(uint64_t id=16;id<=21;++id) {
			if(argc==2 && std::stoull(argv[1])!=id) continue;
			const auto* b=find_backend(backends,BackendId{id}); CODEC_VIS_CHECK(b && b->queryCapabilities().snapshot.available && b->decodePreview);
			auto params=defaults(*b);
			if(id==16) set(params,"preset",std::string("ultrafast"));
			if(id==18 || id==19 || id==21) set(params,"speed",int64_t(id==21 ? 10 : 8));
			const auto source=pattern(id==17);
			if(id==17) {
				run(*b,source,params);
				set(params,"lossless",true); run(*b,source,params,true);
				// Odd RGB dimensions need no chroma padding in lossless mode.
				run(*b,pattern(true,131,97),params,true);
			} else {
				std::vector<std::string> depths,chroma;
				for(const auto& p:b->queryCapabilities().params) {
					if(p.name=="bit-depth") for(const auto& v:p.enumValues) depths.push_back(v.value);
					if(p.name=="chroma-subsampling") for(const auto& v:p.enumValues) chroma.push_back(v.value);
				}
				for(const auto& depth:depths) for(const auto& c:chroma) {
					set(params,"bit-depth",depth); set(params,"chroma-subsampling",c);
					run(*b,source,params);
					if(id==16 || id==19) {
						set(params,"lossless",true); run(*b,source,params,true); set(params,"lossless",false);
					}
				}
				// Exercise the actual RGB input preparation and odd-size workflow.
				set(params,"bit-depth",std::string("8"));set(params,"chroma-subsampling",std::string("420"));
				if(id==16 || id==20) {
					run(*b,pattern(true),params);
					bool rejected=false;
					try {(void)run_backend_encode(*b,pattern(true,131,97),params);} catch(const std::invalid_argument&) {rejected=true;}
					CODEC_VIS_CHECK(rejected);
				} else run(*b,pattern(true,131,97),params);
			}
			// Endpoints must be accepted, affect the real bitstream, and remain
			// decodable. Run them on the ordinary color pattern, not a flat field.
			auto low=params,high=params;
			const std::string quality=id==16 || id==20 ? "qp" : id==21 ? "quantizer" : "quality";
			set(low,quality,int64_t(0)); set(high,quality,int64_t(id==16 ? 51 : id==17 ? 100 : id==21 ? 255 : 63));
			if(id==17) {set(low,"lossless",false);set(high,"lossless",false);}
			run(*b,source,low); run(*b,source,high);
			CODEC_VIS_CHECK(run_backend_encode(*b,source,low).encoded.hevcAnnexB!=run_backend_encode(*b,source,high).encoded.hevcAnnexB);
			if(id!=17) {
				auto full=source; full.color.range=ColorRange::Full;
				auto colorParams=params; set(colorParams,"range",std::string("full"));
				if(id==20) {
					bool rejected=false;
					try {(void)run_backend_encode(*b,full,colorParams);} catch(const std::invalid_argument&) {rejected=true;}
					CODEC_VIS_CHECK(rejected);
				} else run(*b,full,colorParams);
				full.color={.primaries=ColorPrimaries::BT2020,.transfer=TransferCharacteristics::PQ,.matrix=MatrixCoefficients::BT2020NonConstant,.range=ColorRange::Limited};
				set(colorParams,"range",std::string("limited"));
				run(*b,full,colorParams);
			} else {
				auto hdr=pattern();
				hdr.color={.primaries=ColorPrimaries::BT2020,.transfer=TransferCharacteristics::PQ,.matrix=MatrixCoefficients::BT2020NonConstant,.range=ColorRange::Limited};
				bool rejected=false;
				try {(void)run_backend_encode(*b,hdr,params);} catch(const std::invalid_argument&) {rejected=true;}
				CODEC_VIS_CHECK(rejected);
				auto mapped=params; set(mapped,"lossless",false);
				set(mapped,"color-primaries",std::string("bt709")); set(mapped,"transfer",std::string("srgb")); set(mapped,"tone-map",std::string("reinhard"));
				run(*b,hdr,mapped);
			}
			auto invalid=params; invalid.push_back({.name="not-an-option",.value=int64_t(1)});
			bool rejected=false;
			try {(void)run_backend_encode(*b,source,invalid);} catch(const std::invalid_argument&) {rejected=true;}
			CODEC_VIS_CHECK(rejected);
			CODEC_VIS_CHECK(!b->decodePreview(EncodedImage{}).image);
		}
	} catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
