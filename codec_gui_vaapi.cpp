// codec_gui_vaapi.cpp
#include "codec_gui_x265.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_av1.h>
#include <va/va_enc_hevc.h>
}

namespace codec_gui {
namespace {

template <typename T>
T get_param(std::span<const EncoderParam> params, std::string_view name, T defaultValue) {
	for (const EncoderParam& param : params) {
		if (param.name == name) {
			return std::get<T>(param.value);
		}
	}
	return defaultValue;
}

int64_t get_int_param(std::span<const EncoderParam> params, std::string_view name, int64_t defaultValue) {
	for (const EncoderParam& param : params) {
		if (param.name == name) {
			return std::get<int64_t>(param.value);
		}
	}
	return defaultValue;
}

int64_t get_int_param_alias(std::span<const EncoderParam> params, std::string_view name, std::string_view alias, int64_t defaultValue) {
	for (const EncoderParam& param : params) {
		if (param.name == name || param.name == alias) {
			return std::get<int64_t>(param.value);
		}
	}
	return defaultValue;
}

struct BitWriter {
	std::vector<uint8_t> bytes;
	int bitOffset = 0;

	void bit(bool value) {
		if (bitOffset == 0) {
			bytes.push_back(0);
		}
		if (value) {
			bytes.back() |= static_cast<uint8_t>(0x80u >> bitOffset);
		}
		bitOffset = (bitOffset + 1) & 7;
	}

	void bits(uint64_t value, int count) {
		for (int i = count - 1; i >= 0; --i) {
			bit(((value >> i) & 1u) != 0);
		}
	}

	void ue(uint32_t value) {
		const uint32_t codeNum = value + 1;
		int bitsNeeded = 0;
		for (uint32_t tmp = codeNum; tmp != 0; tmp >>= 1) {
			++bitsNeeded;
		}
		for (int i = 0; i < bitsNeeded - 1; ++i) {
			bit(false);
		}
		bits(codeNum, bitsNeeded);
	}

	void se(int32_t value) {
		const uint32_t codeNum = value <= 0 ? static_cast<uint32_t>(-2 * value) : static_cast<uint32_t>(2 * value - 1);
		ue(codeNum);
	}

	void trailing_bits() {
		bit(true);
		while (bitOffset != 0) {
			bit(false);
		}
	}

	void byte_align_zero() {
		while (bitOffset != 0) {
			bit(false);
		}
	}
};

void put_u16le(std::vector<std::byte>& out, uint16_t value) {
	out.push_back(std::byte(value & 0xffu));
	out.push_back(std::byte(value >> 8u));
}

void put_u32le(std::vector<std::byte>& out, uint32_t value) {
	for (int shift = 0; shift < 32; shift += 8) {
		out.push_back(std::byte((value >> shift) & 0xffu));
	}
}

void put_u64le(std::vector<std::byte>& out, uint64_t value) {
	for (int shift = 0; shift < 64; shift += 8) {
		out.push_back(std::byte((value >> shift) & 0xffu));
	}
}

void append_ivf_header(std::vector<std::byte>& out, int width, int height, uint32_t frameCount) {
	for (char c : {'D', 'K', 'I', 'F'}) out.push_back(std::byte(c));
	put_u16le(out, 0);
	put_u16le(out, 32);
	for (char c : {'A', 'V', '0', '1'}) out.push_back(std::byte(c));
	put_u16le(out, static_cast<uint16_t>(width));
	put_u16le(out, static_cast<uint16_t>(height));
	put_u32le(out, 1);
	put_u32le(out, 1);
	put_u32le(out, frameCount);
	put_u32le(out, 0);
}

void append_ivf_frame(std::vector<std::byte>& out, const std::vector<std::byte>& frame, uint64_t pts) {
	put_u32le(out, static_cast<uint32_t>(frame.size()));
	put_u64le(out, pts);
	out.insert(out.end(), frame.begin(), frame.end());
}

int bits_required(uint32_t value) {
	int bits = 0;
	do {
		++bits;
		value >>= 1;
	} while (value != 0);
	return bits;
}

std::vector<uint8_t> escape_rbsp(const std::vector<uint8_t>& rbsp) {
	std::vector<uint8_t> out;
	int zeroCount = 0;
	for (uint8_t byte : rbsp) {
		if (zeroCount >= 2 && byte <= 0x03) {
			out.push_back(0x03);
			zeroCount = 0;
		}
		out.push_back(byte);
		zeroCount = byte == 0 ? zeroCount + 1 : 0;
	}
	return out;
}

std::vector<std::byte> hevc_slice_header(int32_t qpDelta, bool sao, bool loopFilterAcrossSlices, bool entryPointsPresent) {
	BitWriter bw;
	bw.bit(true);      // first_slice_segment_in_pic_flag
	bw.bit(false);     // no_output_of_prior_pics_flag
	bw.ue(0);          // slice_pic_parameter_set_id
	bw.ue(2);          // slice_type: I
	if (sao) {
		bw.bit(true);  // slice_sao_luma_flag
		bw.bit(true);  // slice_sao_chroma_flag
	}
	bw.se(qpDelta);    // slice_qp_delta
	if (sao) {
		bw.bit(loopFilterAcrossSlices);
	}
	if (entryPointsPresent) {
		bw.ue(0);      // num_entry_point_offsets
	}
	bw.trailing_bits();

	std::vector<std::byte> nal{
		std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
		std::byte{0x26}, std::byte{0x01},
	};
	for (uint8_t byte : escape_rbsp(bw.bytes)) {
		nal.push_back(std::byte{byte});
	}
	return nal;
}

void hevc_profile_tier_level(BitWriter& bw, uint8_t profileIdc, uint8_t levelIdc, bool highTier) {
	bw.bits(0, 2);
	bw.bit(highTier);
	bw.bits(profileIdc, 5);
	for (uint8_t idc = 1; idc <= 32; ++idc) {
		bw.bit(idc == profileIdc);
	}
	bw.bit(true);
	bw.bit(false);
	bw.bit(false);
	bw.bit(true);
	bw.bits(0, 44);
	bw.bits(levelIdc, 8);
}

std::vector<std::byte> make_hevc_nal(uint8_t nalType, const std::vector<uint8_t>& rbsp) {
	std::vector<std::byte> nal{
		std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
		std::byte{static_cast<uint8_t>(nalType << 1u)}, std::byte{0x01},
	};
	for (uint8_t byte : escape_rbsp(rbsp)) {
		nal.push_back(std::byte{byte});
	}
	return nal;
}

std::vector<std::byte> hevc_vps_header(uint8_t profileIdc, uint8_t levelIdc, bool highTier) {
	BitWriter bw;
	bw.bits(0, 4);
	bw.bit(true);
	bw.bit(true);
	bw.bits(0, 6);
	bw.bits(0, 3);
	bw.bit(true);
	bw.bits(0xffff, 16);
	hevc_profile_tier_level(bw, profileIdc, levelIdc, highTier);
	bw.bit(false);
	bw.ue(0);
	bw.bits(0, 5);
	bw.ue(0);
	bw.bits(0, 6);
	bw.ue(0);
	bw.bit(false);
	bw.bit(false);
	bw.trailing_bits();
	return make_hevc_nal(32, bw.bytes);
}

std::vector<std::byte> hevc_sps_header(
	int width,
	int height,
	uint8_t profileIdc,
	uint8_t levelIdc,
	bool highTier,
	uint32_t chromaFormat,
	uint32_t depthMinus8,
	bool sao,
	bool strongIntraSmoothing
) {
	BitWriter bw;
	bw.bits(0, 4);
	bw.bits(0, 3);
	bw.bit(true);
	hevc_profile_tier_level(bw, profileIdc, levelIdc, highTier);
	bw.ue(0);
	bw.ue(chromaFormat);
	if (chromaFormat == 3) {
		bw.bit(false);
	}
	bw.ue(static_cast<uint32_t>(width));
	bw.ue(static_cast<uint32_t>(height));
	bw.bit(false);
	bw.ue(depthMinus8);
	bw.ue(depthMinus8);
	bw.ue(4);
	bw.bit(false);
	bw.ue(0);
	bw.ue(0);
	bw.ue(0);
	bw.ue(0);
	bw.ue(3);
	bw.ue(0);
	bw.ue(3);
	bw.ue(2);
	bw.ue(2);
	bw.bit(false);
	bw.bit(false);
	bw.bit(sao);
	bw.bit(false);
	bw.ue(0);
	bw.bit(false);
	bw.bit(false);
	bw.bit(strongIntraSmoothing);
	bw.bit(false);
	bw.bit(false);
	bw.trailing_bits();
	return make_hevc_nal(33, bw.bytes);
}

std::vector<std::byte> hevc_pps_header(const VAEncPictureParameterBufferHEVC& pic) {
	BitWriter bw;
	bw.ue(0);
	bw.ue(0);
	bw.bit(false);
	bw.bit(false);
	bw.bits(0, 3);
	bw.bit(pic.pic_fields.bits.sign_data_hiding_enabled_flag != 0);
	bw.bit(false);
	bw.ue(0);
	bw.ue(0);
	bw.se(static_cast<int32_t>(pic.pic_init_qp) - 26);
	bw.bit(pic.pic_fields.bits.constrained_intra_pred_flag != 0);
	bw.bit(pic.pic_fields.bits.transform_skip_enabled_flag != 0);
	bw.bit(pic.pic_fields.bits.cu_qp_delta_enabled_flag != 0);
	if (pic.pic_fields.bits.cu_qp_delta_enabled_flag != 0) {
		bw.ue(pic.diff_cu_qp_delta_depth);
	}
	bw.se(pic.pps_cb_qp_offset);
	bw.se(pic.pps_cr_qp_offset);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(pic.pic_fields.bits.tiles_enabled_flag != 0);
	bw.bit(pic.pic_fields.bits.entropy_coding_sync_enabled_flag != 0);
	if (pic.pic_fields.bits.tiles_enabled_flag != 0) {
		bw.ue(pic.num_tile_columns_minus1);
		bw.ue(pic.num_tile_rows_minus1);
		bw.bit(false);
		for (uint8_t i = 0; i < pic.num_tile_columns_minus1; ++i) {
			bw.ue(pic.column_width_minus1[i]);
		}
		for (uint8_t i = 0; i < pic.num_tile_rows_minus1; ++i) {
			bw.ue(pic.row_height_minus1[i]);
		}
		bw.bit(pic.pic_fields.bits.loop_filter_across_tiles_enabled_flag != 0);
	}
	bw.bit(pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag != 0);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.ue(0);
	bw.bit(false);
	bw.bit(false);
	bw.trailing_bits();
	return make_hevc_nal(34, bw.bytes);
}

void append_leb128(std::vector<std::byte>& out, uint64_t value) {
	do {
		uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
		value >>= 7u;
		if (value != 0) {
			byte |= 0x80u;
		}
		out.push_back(std::byte{byte});
	} while (value != 0);
}

void append_leb128_fixed4(std::vector<std::byte>& out, uint64_t value) {
	for (int i = 0; i < 4; ++i) {
		uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
		value >>= 7u;
		if (i != 3) {
			byte |= 0x80u;
		}
		out.push_back(std::byte{byte});
	}
}

std::vector<std::byte> make_av1_obu(uint8_t type, const std::vector<uint8_t>& payload, bool fixedSizeField = false) {
	std::vector<std::byte> out;
	out.push_back(std::byte{static_cast<uint8_t>((type << 3u) | 0x02u)});
	if (fixedSizeField) {
		append_leb128_fixed4(out, payload.size());
	} else {
		append_leb128(out, payload.size());
	}
	for (uint8_t byte : payload) {
		out.push_back(std::byte{byte});
	}
	return out;
}

std::vector<std::byte> av1_temporal_delimiter_obu() {
	return {std::byte{0x12}, std::byte{0x00}};
}

uint32_t ceil_div_u32(uint32_t value, uint32_t divisor) {
	return (value + divisor - 1u) / divisor;
}

uint32_t tile_log2(uint32_t blkSize, uint32_t target) {
	uint32_t k = 0;
	while ((blkSize << k) < target) {
		++k;
	}
	return k;
}

uint32_t floor_log2_u32(uint32_t value) {
	uint32_t out = 0;
	while (value > 1) {
		value >>= 1u;
		++out;
	}
	return out;
}

uint32_t next_power_of_two_u32(uint32_t value) {
	uint32_t out = 1;
	while (out < value) {
		out <<= 1u;
	}
	return out;
}

struct Av1TileLayout {
	uint32_t cols = 1;
	uint32_t rows = 1;
	uint32_t colLog2 = 0;
	uint32_t rowLog2 = 0;
	uint32_t minColLog2 = 0;
	uint32_t maxColLog2 = 0;
	uint32_t minRowLog2 = 0;
	uint32_t maxRowLog2 = 0;
	uint32_t tileSizeBytesMinus1 = 3;
};

Av1TileLayout av1_tile_layout(int width, int height, bool use128, uint32_t requestedCols, uint32_t requestedRows) {
	const uint32_t sbSize = use128 ? 128u : 64u;
	const uint32_t sbCols = ceil_div_u32(static_cast<uint32_t>(width), sbSize);
	const uint32_t sbRows = ceil_div_u32(static_cast<uint32_t>(height), sbSize);
	const uint32_t maxTileWidthSb = 4096u / sbSize;
	const uint32_t maxTileAreaSb = (4096u * 2304u) / (sbSize * sbSize);
	Av1TileLayout layout;
	layout.minColLog2 = tile_log2(maxTileWidthSb, sbCols);
	layout.maxColLog2 = tile_log2(1, std::min(sbCols, 64u));
	layout.maxRowLog2 = tile_log2(1, std::min(sbRows, 64u));
	const uint32_t minTilesLog2 = std::max(layout.minColLog2, tile_log2(maxTileAreaSb, sbRows * sbCols));
	const uint32_t requestedColLog2 = floor_log2_u32(next_power_of_two_u32(std::clamp<uint32_t>(requestedCols, 1, 64)));
	layout.colLog2 = std::clamp<uint32_t>(std::max({requestedColLog2, layout.minColLog2, std::min(minTilesLog2, layout.maxColLog2)}), layout.minColLog2, layout.maxColLog2);
	layout.minRowLog2 = minTilesLog2 > layout.colLog2 ? minTilesLog2 - layout.colLog2 : 0;
	const uint32_t requestedRowLog2 = floor_log2_u32(next_power_of_two_u32(std::clamp<uint32_t>(requestedRows, 1, 64)));
	layout.rowLog2 = std::clamp<uint32_t>(std::max(requestedRowLog2, layout.minRowLog2), layout.minRowLog2, layout.maxRowLog2);
	layout.cols = 1u << layout.colLog2;
	layout.rows = 1u << layout.rowLog2;
	return layout;
}

std::vector<std::byte> av1_sequence_header_obu(int width, int height, int depth, uint8_t levelIdx, bool highTier, bool use128, bool filterIntra, bool intraEdge, bool cdef, bool restoration) {
	const int widthBits = bits_required(static_cast<uint32_t>(width - 1));
	const int heightBits = bits_required(static_cast<uint32_t>(height - 1));
	BitWriter bw;
	bw.bits(0, 3);
	bw.bit(true);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bits(0, 5);
	bw.bits(0, 12);
	bw.bits(levelIdx, 5);
	if (levelIdx > 7) {
		bw.bit(highTier);
	}
	bw.bits(static_cast<uint32_t>(widthBits - 1), 4);
	bw.bits(static_cast<uint32_t>(heightBits - 1), 4);
	bw.bits(static_cast<uint32_t>(width - 1), widthBits);
	bw.bits(static_cast<uint32_t>(height - 1), heightBits);
	bw.bit(false);
	bw.bit(use128);
	bw.bit(filterIntra);
	bw.bit(intraEdge);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(cdef);
	bw.bit(restoration);
	bw.bit(depth == 10);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bits(0, 2);
	bw.bit(false);
	bw.bit(false);
	bw.trailing_bits();
	return make_av1_obu(1, bw.bytes);
}

std::vector<std::byte> av1_frame_header_obu(int width, int height, bool use128, const Av1TileLayout& tiles, int qindex, bool disableCdf, bool screenContent, bool intrabc, bool palette, int txMode, int filterLevel, bool cdef) {
	BitWriter bw;
	bw.bit(false);
	bw.bits(0, 2);
	bw.bit(true);
	bw.bit(disableCdf);
	bw.bit(false);
	(void)width;
	(void)height;
	(void)use128;
	(void)screenContent;
	bw.bit(false);
	if (!disableCdf) {
		bw.bit(false);
	}
	bw.bit(true);
	for (uint32_t i = tiles.minColLog2; i < tiles.colLog2; ++i) {
		bw.bit(true);
	}
	if (tiles.colLog2 < tiles.maxColLog2) {
		bw.bit(false);
	}
	for (uint32_t i = tiles.minRowLog2; i < tiles.rowLog2; ++i) {
		bw.bit(true);
	}
	if (tiles.rowLog2 < tiles.maxRowLog2) {
		bw.bit(false);
	}
	if (tiles.colLog2 > 0 || tiles.rowLog2 > 0) {
		bw.bits(0, static_cast<int>(tiles.colLog2 + tiles.rowLog2));
		bw.bits(tiles.tileSizeBytesMinus1, 2);
	}
	bw.bits(static_cast<uint32_t>(qindex), 8);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	bw.bit(false);
	const bool codedLossless = qindex == 0;
	if (qindex > 0) {
		bw.bit(false);
	}
	if (!codedLossless && !intrabc) {
		bw.bits(static_cast<uint32_t>(filterLevel), 6);
		bw.bits(static_cast<uint32_t>(filterLevel), 6);
		if (filterLevel != 0) {
			bw.bits(0, 6);
			bw.bits(0, 6);
		}
		bw.bits(0, 3);
		bw.bit(false);
	}
	if (!codedLossless && !intrabc && cdef) {
		bw.bits(0, 2);
		bw.bits(0, 2);
		bw.bits(0, 4);
		bw.bits(0, 2);
		bw.bits(0, 4);
		bw.bits(0, 2);
	}
	bw.bit(txMode != 0);
	(void)palette;
	bw.trailing_bits();
	return make_av1_obu(3, bw.bytes, true);
}

uint8_t av1_level_idx_for_picture(int width, int height) {
	const uint64_t samples = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
	struct LevelLimit {
		uint8_t idx;
		uint64_t maxPictureSamples;
	};
	constexpr LevelLimit levels[] = {
		{0, 147456},   {1, 278784},   {4, 665856},   {5, 1065024},
		{8, 2359296},  {12, 8912896}, {16, 35651584}, {20, 35651584},
	};
	for (const LevelLimit& level : levels) {
		if (samples <= level.maxPictureSamples) {
			return level.idx;
		}
	}
	return 20;
}

uint8_t av1_level_idx_from_params(std::span<const EncoderParam> params, int width, int height) {
	const int64_t requested = get_int_param(params, "level-idx", -1);
	if (requested < 0) {
		return av1_level_idx_for_picture(width, height);
	}
	return static_cast<uint8_t>(std::clamp<int64_t>(requested, 0, 20));
}

int bit_depth(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8: return 8;
		case PixelFormat::YUV420P10LE: return 10;
		case PixelFormat::YUV422P8: return 8;
		case PixelFormat::YUV422P10LE: return 10;
		case PixelFormat::YUV444P8: return 8;
		case PixelFormat::YUV444P10LE: return 10;
		default: throw std::runtime_error("VA-API HEVC accepts YUV420/YUV422/YUV444 8/10-bit input");
	}
}

bool is_444(PixelFormat format) {
	return format == PixelFormat::YUV444P8 || format == PixelFormat::YUV444P10LE;
}

bool is_422(PixelFormat format) {
	return format == PixelFormat::YUV422P8 || format == PixelFormat::YUV422P10LE;
}

bool is_420(PixelFormat format) {
	return format == PixelFormat::YUV420P8 || format == PixelFormat::YUV420P10LE;
}

int plane_width(const RawImage& image, int plane) {
	return plane == 0 || is_444(image.format) ? image.width : image.width / 2;
}

int plane_height(const RawImage& image, int plane) {
	return plane == 0 || is_444(image.format) || is_422(image.format) ? image.height : image.height / 2;
}

PixelFormat vaapi_pixel_format(int bitDepth, std::string_view chroma) {
	if (chroma == "444") {
		return bitDepth == 8 ? PixelFormat::YUV444P8 : PixelFormat::YUV444P10LE;
	}
	if (chroma == "422") {
		return bitDepth == 8 ? PixelFormat::YUV422P8 : PixelFormat::YUV422P10LE;
	}
	if (chroma == "420") {
		return bitDepth == 8 ? PixelFormat::YUV420P8 : PixelFormat::YUV420P10LE;
	}
	throw std::runtime_error("invalid chroma-subsampling value: " + std::string(chroma));
}

void validate_yuv_image(const RawImage& image) {
	if (image.width <= 0 || image.height <= 0) {
		throw std::runtime_error("image dimensions must be positive");
	}
	if ((is_420(image.format) || is_422(image.format)) && (image.width & 1) != 0) {
		throw std::runtime_error("VA-API subsampled chroma input requires even width");
	}
	if (is_420(image.format) && (image.height & 1) != 0) {
		throw std::runtime_error("VA-API YUV420 input requires even height");
	}
	const int bps = bit_depth(image.format) == 8 ? 1 : 2;
	for (int plane = 0; plane < 3; ++plane) {
		const int width = plane_width(image, plane);
		const int height = plane_height(image, plane);
		if (image.planes[plane].strideBytes < width * bps) {
			throw std::runtime_error("plane stride is smaller than required by image width");
		}
		if (image.planes[plane].bytes.size() < static_cast<std::size_t>(image.planes[plane].strideBytes) * height) {
			throw std::runtime_error("plane buffer is smaller than stride * height");
		}
	}
}

uint16_t read_sample(const ImagePlane& plane, int x, int y, int bps) {
	const uint8_t* src = plane.bytes.data() + static_cast<std::size_t>(y) * plane.strideBytes + static_cast<std::size_t>(x) * bps;
	if (bps == 1) return *src;
	uint16_t value = 0;
	std::memcpy(&value, src, 2);
	return static_cast<uint16_t>(value & 0x03ffu);
}

void write_sample(ImagePlane& plane, int x, int y, int bps, uint16_t sample) {
	uint8_t* dst = plane.bytes.data() + static_cast<std::size_t>(y) * plane.strideBytes + static_cast<std::size_t>(x) * bps;
	if (bps == 1) {
		*dst = static_cast<uint8_t>(sample);
	} else {
		std::memcpy(dst, &sample, 2);
	}
}

uint16_t rescale_sample(uint16_t sample, int srcDepth, int dstDepth) {
	if (srcDepth == dstDepth) return sample;
	return dstDepth == 8 ? static_cast<uint16_t>(sample >> 2u) : static_cast<uint16_t>(sample << 2u);
}

RawImage convert_yuv_format(const RawImage& image, PixelFormat targetFormat) {
	validate_yuv_image(image);
	if (image.format == targetFormat) return image;

	RawImage out;
	out.width = image.width;
	out.height = image.height;
	out.format = targetFormat;
	out.color = image.color;
	const int srcDepth = bit_depth(image.format);
	const int dstDepth = bit_depth(targetFormat);
	const int srcBps = srcDepth == 8 ? 1 : 2;
	const int dstBps = dstDepth == 8 ? 1 : 2;
	for (int plane = 0; plane < 3; ++plane) {
		out.planes[plane].strideBytes = plane_width(out, plane) * dstBps;
		out.planes[plane].bytes.resize(static_cast<std::size_t>(out.planes[plane].strideBytes) * plane_height(out, plane));
	}

	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			write_sample(out.planes[0], x, y, dstBps, rescale_sample(read_sample(image.planes[0], x, y, srcBps), srcDepth, dstDepth));
		}
	}
	for (int plane = 1; plane < 3; ++plane) {
		const int dstWidth = plane_width(out, plane);
		const int dstHeight = plane_height(out, plane);
		for (int y = 0; y < dstHeight; ++y) {
			for (int x = 0; x < dstWidth; ++x) {
				uint32_t sample = 0;
				if ((is_420(image.format) || is_422(image.format)) && is_444(targetFormat)) {
					sample = read_sample(image.planes[plane], x / 2, is_420(image.format) ? y / 2 : y, srcBps);
				} else if (is_444(image.format) && is_420(targetFormat)) {
					for (int dy = 0; dy < 2; ++dy) {
						for (int dx = 0; dx < 2; ++dx) {
							sample += read_sample(image.planes[plane], x * 2 + dx, y * 2 + dy, srcBps);
						}
					}
					sample = (sample + 2) / 4;
				} else if (is_444(image.format) && is_422(targetFormat)) {
					sample = (read_sample(image.planes[plane], x * 2, y, srcBps) +
					          read_sample(image.planes[plane], x * 2 + 1, y, srcBps) + 1) / 2;
				} else if (is_422(image.format) && is_420(targetFormat)) {
					sample = (read_sample(image.planes[plane], x, y * 2, srcBps) +
					          read_sample(image.planes[plane], x, y * 2 + 1, srcBps) + 1) / 2;
				} else if (is_420(image.format) && is_422(targetFormat)) {
					sample = read_sample(image.planes[plane], x, y / 2, srcBps);
				} else {
					sample = read_sample(image.planes[plane], x, y, srcBps);
				}
				write_sample(out.planes[plane], x, y, dstBps, rescale_sample(static_cast<uint16_t>(sample), srcDepth, dstDepth));
			}
		}
	}
	return out;
}

int requested_bit_depth(std::span<const EncoderParam> params, const RawImage& image) {
	const std::string value = get_param<std::string>(params, "bit-depth", "source");
	if (value == "source") return bit_depth(image.format);
	if (value == "8") return 8;
	if (value == "10") return 10;
	throw std::runtime_error("invalid bit-depth value: " + value);
}

struct VaError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

void checked(VAStatus status, std::string_view operation) {
	if (status == VA_STATUS_SUCCESS) {
		return;
	}
	throw VaError(std::string("VA-API ") + std::string(operation) + " failed: " + vaErrorStr(status));
}

struct VaDisplay {
	int fd = -1;
	VADisplay dpy = nullptr;

	explicit VaDisplay(const std::string& path) {
		fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			throw VaError("failed to open VA-API render node '" + path + "': " + std::strerror(errno));
		}
		dpy = vaGetDisplayDRM(fd);
		if (dpy == nullptr) {
			throw VaError("vaGetDisplayDRM returned null");
		}
		int major = 0;
		int minor = 0;
		checked(vaInitialize(dpy, &major, &minor), "initialize");
	}

	VaDisplay(const VaDisplay&) = delete;
	VaDisplay& operator=(const VaDisplay&) = delete;

	~VaDisplay() {
		if (dpy != nullptr) {
			(void)vaTerminate(dpy);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
};

struct ConfigGuard {
	VADisplay dpy = nullptr;
	VAConfigID id = VA_INVALID_ID;
	ConfigGuard() = default;
	ConfigGuard(VADisplay dpy_, VAConfigID id_) : dpy(dpy_), id(id_) {}
	ConfigGuard(const ConfigGuard&) = delete;
	ConfigGuard& operator=(const ConfigGuard&) = delete;
		ConfigGuard(ConfigGuard&& other) noexcept : dpy(other.dpy), id(other.id) {
			other.dpy = nullptr;
			other.id = VA_INVALID_ID;
		}
		ConfigGuard& operator=(ConfigGuard&& other) noexcept {
			if (this != &other) {
				if (dpy != nullptr && id != VA_INVALID_ID) {
					(void)vaDestroyConfig(dpy, id);
				}
				dpy = other.dpy;
				id = other.id;
				other.dpy = nullptr;
				other.id = VA_INVALID_ID;
			}
			return *this;
		}
	~ConfigGuard() {
		if (dpy != nullptr && id != VA_INVALID_ID) {
			(void)vaDestroyConfig(dpy, id);
		}
	}
};

struct ContextGuard {
	VADisplay dpy = nullptr;
	VAContextID id = VA_INVALID_ID;
	ContextGuard() = default;
	ContextGuard(VADisplay dpy_, VAContextID id_) : dpy(dpy_), id(id_) {}
	ContextGuard(const ContextGuard&) = delete;
	ContextGuard& operator=(const ContextGuard&) = delete;
		ContextGuard(ContextGuard&& other) noexcept : dpy(other.dpy), id(other.id) {
			other.dpy = nullptr;
			other.id = VA_INVALID_ID;
		}
		ContextGuard& operator=(ContextGuard&& other) noexcept {
			if (this != &other) {
				if (dpy != nullptr && id != VA_INVALID_ID) {
					(void)vaDestroyContext(dpy, id);
				}
				dpy = other.dpy;
				id = other.id;
				other.dpy = nullptr;
				other.id = VA_INVALID_ID;
			}
			return *this;
		}
	~ContextGuard() {
		if (dpy != nullptr && id != VA_INVALID_ID) {
			(void)vaDestroyContext(dpy, id);
		}
	}
};

struct SurfaceGuard {
	VADisplay dpy = nullptr;
	VASurfaceID surface = VA_INVALID_SURFACE;
	SurfaceGuard() = default;
	SurfaceGuard(VADisplay dpy_, VASurfaceID surface_) : dpy(dpy_), surface(surface_) {}
	SurfaceGuard(const SurfaceGuard&) = delete;
	SurfaceGuard& operator=(const SurfaceGuard&) = delete;
		SurfaceGuard(SurfaceGuard&& other) noexcept : dpy(other.dpy), surface(other.surface) {
			other.dpy = nullptr;
			other.surface = VA_INVALID_SURFACE;
		}
		SurfaceGuard& operator=(SurfaceGuard&& other) noexcept {
			if (this != &other) {
				if (dpy != nullptr && surface != VA_INVALID_SURFACE) {
					(void)vaDestroySurfaces(dpy, &surface, 1);
				}
				dpy = other.dpy;
				surface = other.surface;
				other.dpy = nullptr;
				other.surface = VA_INVALID_SURFACE;
			}
			return *this;
		}
	~SurfaceGuard() {
		if (dpy != nullptr && surface != VA_INVALID_SURFACE) {
			(void)vaDestroySurfaces(dpy, &surface, 1);
		}
	}
};

struct BufferGuard {
	VADisplay dpy = nullptr;
	VABufferID id = VA_INVALID_ID;
	BufferGuard() = default;
	BufferGuard(VADisplay dpy_, VABufferID id_) : dpy(dpy_), id(id_) {}
	BufferGuard(const BufferGuard&) = delete;
	BufferGuard& operator=(const BufferGuard&) = delete;
		BufferGuard(BufferGuard&& other) noexcept : dpy(other.dpy), id(other.id) {
			other.dpy = nullptr;
			other.id = VA_INVALID_ID;
		}
		BufferGuard& operator=(BufferGuard&& other) noexcept {
			if (this != &other) {
				if (dpy != nullptr && id != VA_INVALID_ID) {
					(void)vaDestroyBuffer(dpy, id);
				}
				dpy = other.dpy;
				id = other.id;
				other.dpy = nullptr;
				other.id = VA_INVALID_ID;
			}
			return *this;
		}
	~BufferGuard() {
		if (dpy != nullptr && id != VA_INVALID_ID) {
			(void)vaDestroyBuffer(dpy, id);
		}
	}
};

struct ImageGuard {
	VADisplay dpy = nullptr;
	VAImage image{};
	bool valid = false;
	~ImageGuard() {
		if (dpy != nullptr && valid) {
			(void)vaDestroyImage(dpy, image.image_id);
		}
	}
};

VAConfigID create_config(VADisplay dpy, VAProfile profile, uint32_t rtFormat, uint32_t packedHeaders, uint32_t rcMode) {
	VAConfigAttrib attrs[3] = {
		{VAConfigAttribRTFormat, rtFormat},
		{VAConfigAttribRateControl, rcMode},
	};
	int attrCount = 2;
	if (packedHeaders != 0) {
		attrs[attrCount++] = {VAConfigAttribEncPackedHeaders, packedHeaders};
	}
	VAConfigID config = VA_INVALID_ID;
	checked(vaCreateConfig(dpy, profile, VAEntrypointEncSlice, attrs, attrCount, &config), "create encode config");
	return config;
}

VASurfaceID create_surface(VADisplay dpy, const RawImage& image, uint32_t rtFormat, uint32_t pixelFormat) {
	VASurfaceAttrib attribs[2]{};
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
	attribs[1].type = VASurfaceAttribPixelFormat;
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].value.type = VAGenericValueTypeInteger;
	attribs[1].value.value.i = static_cast<int32_t>(pixelFormat);
	VASurfaceID surface = VA_INVALID_SURFACE;
	checked(vaCreateSurfaces(dpy, rtFormat, image.width, image.height, &surface, 1, attribs, 2), "create surface");
	return surface;
}

void upload_yuv_to_surface(VADisplay dpy, VASurfaceID surface, const RawImage& image) {
	ImageGuard derived{dpy};
	checked(vaDeriveImage(dpy, surface, &derived.image), "derive image");
	derived.valid = true;
	void* mapped = nullptr;
	checked(vaMapBuffer(dpy, derived.image.buf, &mapped), "map image");
	std::memset(mapped, 0, derived.image.data_size);
	struct UnmapGuard {
		VADisplay dpy = nullptr;
		VABufferID id = VA_INVALID_ID;
		~UnmapGuard() {
			if (dpy != nullptr && id != VA_INVALID_ID) {
				(void)vaUnmapBuffer(dpy, id);
			}
		}
	} unmap{dpy, derived.image.buf};

	auto* base = static_cast<uint8_t*>(mapped);
	const int depth = bit_depth(image.format);
	const bool yuv444 = is_444(image.format);
	const bool yuv422 = is_422(image.format);
	if (!yuv444 && !yuv422 && depth == 8 && derived.image.format.fourcc != VA_FOURCC_NV12) {
		throw std::runtime_error("VA-API driver did not provide an NV12 image mapping");
	}
	if (!yuv444 && !yuv422 && depth == 10 && derived.image.format.fourcc != VA_FOURCC_P010) {
		throw std::runtime_error("VA-API driver did not provide a P010 image mapping");
	}
	if (yuv444 && depth == 8 && derived.image.format.fourcc != VA_FOURCC_444P) {
		throw std::runtime_error("VA-API driver did not provide a 444P image mapping");
	}
	if (yuv444 && depth == 10 && derived.image.format.fourcc != VA_FOURCC_Y410) {
		throw std::runtime_error("VA-API driver did not provide a Y410 image mapping");
	}
	if (yuv422 && depth == 8 && derived.image.format.fourcc != VA_FOURCC_422H &&
	    derived.image.format.fourcc != VA_FOURCC_YUY2 && derived.image.format.fourcc != VA_FOURCC_UYVY) {
		throw std::runtime_error("VA-API driver did not provide a supported 8-bit YUV422 image mapping");
	}
	if (yuv422 && depth == 10 && derived.image.format.fourcc != VA_FOURCC_Y210) {
		throw std::runtime_error("VA-API driver did not provide a Y210 image mapping");
	}
	if (yuv444) {
		if (depth == 10) {
			for (int y = 0; y < image.height; ++y) {
				uint8_t* dst = base + derived.image.offsets[0] + static_cast<std::size_t>(y) * derived.image.pitches[0];
				const uint8_t* srcY = image.planes[0].bytes.data() + static_cast<std::size_t>(y) * image.planes[0].strideBytes;
				const uint8_t* srcU = image.planes[1].bytes.data() + static_cast<std::size_t>(y) * image.planes[1].strideBytes;
				const uint8_t* srcV = image.planes[2].bytes.data() + static_cast<std::size_t>(y) * image.planes[2].strideBytes;
				for (int x = 0; x < image.width; ++x) {
					uint16_t yy = 0;
					uint16_t u = 0;
					uint16_t v = 0;
					std::memcpy(&yy, srcY + x * 2, 2);
					std::memcpy(&u, srcU + x * 2, 2);
					std::memcpy(&v, srcV + x * 2, 2);
					const uint32_t packed = (3u << 30u) |
					                        (static_cast<uint32_t>(v & 0x03ffu) << 20u) |
					                        (static_cast<uint32_t>(yy & 0x03ffu) << 10u) |
					                        static_cast<uint32_t>(u & 0x03ffu);
					std::memcpy(dst + x * 4, &packed, 4);
				}
			}
			return;
		}
		for (int plane = 0; plane < 3; ++plane) {
			for (int y = 0; y < image.height; ++y) {
				uint8_t* dst = base + derived.image.offsets[plane] + static_cast<std::size_t>(y) * derived.image.pitches[plane];
				const uint8_t* src = image.planes[plane].bytes.data() + static_cast<std::size_t>(y) * image.planes[plane].strideBytes;
				if (depth == 8) {
					std::memcpy(dst, src, static_cast<std::size_t>(image.width));
				} else {
					for (int x = 0; x < image.width; ++x) {
						uint16_t sample = 0;
						std::memcpy(&sample, src + x * 2, 2);
						sample = static_cast<uint16_t>((sample & 0x03ffu) << 6u);
						std::memcpy(dst + x * 2, &sample, 2);
					}
				}
			}
		}
		return;
	}
	if (yuv422) {
		if (depth == 8) {
			if (derived.image.format.fourcc == VA_FOURCC_YUY2 || derived.image.format.fourcc == VA_FOURCC_UYVY) {
				for (int y = 0; y < image.height; ++y) {
					uint8_t* dst = base + derived.image.offsets[0] + static_cast<std::size_t>(y) * derived.image.pitches[0];
					const uint8_t* srcY = image.planes[0].bytes.data() + static_cast<std::size_t>(y) * image.planes[0].strideBytes;
					const uint8_t* srcU = image.planes[1].bytes.data() + static_cast<std::size_t>(y) * image.planes[1].strideBytes;
					const uint8_t* srcV = image.planes[2].bytes.data() + static_cast<std::size_t>(y) * image.planes[2].strideBytes;
					for (int x = 0; x < image.width / 2; ++x) {
						if (derived.image.format.fourcc == VA_FOURCC_YUY2) {
							dst[x * 4 + 0] = srcY[x * 2 + 0];
							dst[x * 4 + 1] = srcU[x];
							dst[x * 4 + 2] = srcY[x * 2 + 1];
							dst[x * 4 + 3] = srcV[x];
						} else {
							dst[x * 4 + 0] = srcU[x];
							dst[x * 4 + 1] = srcY[x * 2 + 0];
							dst[x * 4 + 2] = srcV[x];
							dst[x * 4 + 3] = srcY[x * 2 + 1];
						}
					}
				}
			} else {
				for (int plane = 0; plane < 3; ++plane) {
					for (int y = 0; y < image.height; ++y) {
						const int width = plane == 0 ? image.width : image.width / 2;
						uint8_t* dst = base + derived.image.offsets[plane] + static_cast<std::size_t>(y) * derived.image.pitches[plane];
						const uint8_t* src = image.planes[plane].bytes.data() + static_cast<std::size_t>(y) * image.planes[plane].strideBytes;
						std::memcpy(dst, src, static_cast<std::size_t>(width));
					}
				}
			}
		} else {
			for (int y = 0; y < image.height; ++y) {
				uint8_t* dst = base + derived.image.offsets[0] + static_cast<std::size_t>(y) * derived.image.pitches[0];
				const uint8_t* srcY = image.planes[0].bytes.data() + static_cast<std::size_t>(y) * image.planes[0].strideBytes;
				const uint8_t* srcU = image.planes[1].bytes.data() + static_cast<std::size_t>(y) * image.planes[1].strideBytes;
				const uint8_t* srcV = image.planes[2].bytes.data() + static_cast<std::size_t>(y) * image.planes[2].strideBytes;
				for (int x = 0; x < image.width / 2; ++x) {
					uint16_t y0 = 0;
					uint16_t y1 = 0;
					uint16_t u = 0;
					uint16_t v = 0;
					std::memcpy(&y0, srcY + x * 4, 2);
					std::memcpy(&y1, srcY + x * 4 + 2, 2);
					std::memcpy(&u, srcU + x * 2, 2);
					std::memcpy(&v, srcV + x * 2, 2);
					y0 = static_cast<uint16_t>((y0 & 0x03ffu) << 6u);
					u = static_cast<uint16_t>((u & 0x03ffu) << 6u);
					y1 = static_cast<uint16_t>((y1 & 0x03ffu) << 6u);
					v = static_cast<uint16_t>((v & 0x03ffu) << 6u);
					std::memcpy(dst + x * 8 + 0, &y0, 2);
					std::memcpy(dst + x * 8 + 2, &u, 2);
					std::memcpy(dst + x * 8 + 4, &y1, 2);
					std::memcpy(dst + x * 8 + 6, &v, 2);
				}
			}
		}
		return;
	}
	for (int y = 0; y < image.height; ++y) {
		uint8_t* dst = base + derived.image.offsets[0] + static_cast<std::size_t>(y) * derived.image.pitches[0];
		const uint8_t* src = image.planes[0].bytes.data() + static_cast<std::size_t>(y) * image.planes[0].strideBytes;
		if (depth == 8) {
			std::memcpy(dst, src, static_cast<std::size_t>(image.width));
		} else {
			for (int x = 0; x < image.width; ++x) {
				uint16_t sample = 0;
				std::memcpy(&sample, src + x * 2, 2);
				sample = static_cast<uint16_t>((sample & 0x03ffu) << 6u);
				std::memcpy(dst + x * 2, &sample, 2);
			}
		}
	}
	for (int y = 0; y < image.height / 2; ++y) {
		uint8_t* dst = base + derived.image.offsets[1] + static_cast<std::size_t>(y) * derived.image.pitches[1];
		const uint8_t* srcU = image.planes[1].bytes.data() + static_cast<std::size_t>(y) * image.planes[1].strideBytes;
		const uint8_t* srcV = image.planes[2].bytes.data() + static_cast<std::size_t>(y) * image.planes[2].strideBytes;
		if (depth == 8) {
			for (int x = 0; x < image.width / 2; ++x) {
				dst[x * 2 + 0] = srcU[x];
				dst[x * 2 + 1] = srcV[x];
			}
		} else {
			for (int x = 0; x < image.width / 2; ++x) {
				uint16_t u = 0;
				uint16_t v = 0;
				std::memcpy(&u, srcU + x * 2, 2);
				std::memcpy(&v, srcV + x * 2, 2);
				u = static_cast<uint16_t>((u & 0x03ffu) << 6u);
				v = static_cast<uint16_t>((v & 0x03ffu) << 6u);
				std::memcpy(dst + x * 4 + 0, &u, 2);
				std::memcpy(dst + x * 4 + 2, &v, 2);
			}
		}
	}
}

template <typename T>
BufferGuard create_buffer(VADisplay dpy, VAContextID ctx, VABufferType type, const T& data) {
	VABufferID id = VA_INVALID_ID;
	T copy = data;
	checked(vaCreateBuffer(dpy, ctx, type, sizeof(T), 1, &copy, &id), "create parameter buffer");
	return {dpy, id};
}

BufferGuard create_raw_buffer(VADisplay dpy, VAContextID ctx, VABufferType type, std::span<const std::byte> data) {
	VABufferID id = VA_INVALID_ID;
	checked(vaCreateBuffer(dpy, ctx, type, static_cast<unsigned int>(data.size()), 1, const_cast<std::byte*>(data.data()), &id), "create raw parameter buffer");
	return {dpy, id};
}

uint32_t vaapi_rate_control_mode(std::string_view value) {
	if (value == "cbr") return VA_RC_CBR;
	if (value == "vbr") return VA_RC_VBR;
	if (value == "cqp") return VA_RC_CQP;
	if (value == "icq") return VA_RC_ICQ;
	throw std::runtime_error("unsupported VA-API rate control mode: " + std::string(value));
}

BufferGuard create_misc_rate_control(VADisplay dpy, VAContextID ctx, uint32_t rcMode, uint32_t qp, uint32_t bitrateKbps) {
	std::vector<std::byte> bytes(sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl));
	auto* misc = reinterpret_cast<VAEncMiscParameterBuffer*>(bytes.data());
	auto* rc = reinterpret_cast<VAEncMiscParameterRateControl*>(misc->data);
	misc->type = VAEncMiscParameterTypeRateControl;
	rc->bits_per_second = bitrateKbps * 1000u;
	rc->target_percentage = rcMode == VA_RC_VBR ? 66u : 100u;
	rc->window_size = 1000u;
	rc->initial_qp = qp;
	rc->min_qp = 1;
	rc->max_qp = 51;
	rc->ICQ_quality_factor = std::clamp<uint32_t>(qp, 1, 51);
	rc->quality_factor = qp;
	return create_raw_buffer(dpy, ctx, VAEncMiscParameterBufferType, bytes);
}

BufferGuard create_frame_rate(VADisplay dpy, VAContextID ctx) {
	std::vector<std::byte> bytes(sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate));
	auto* misc = reinterpret_cast<VAEncMiscParameterBuffer*>(bytes.data());
	auto* fr = reinterpret_cast<VAEncMiscParameterFrameRate*>(misc->data);
	misc->type = VAEncMiscParameterTypeFrameRate;
	fr->framerate = 1;
	return create_raw_buffer(dpy, ctx, VAEncMiscParameterBufferType, bytes);
}

BufferGuard create_quality_level(VADisplay dpy, VAContextID ctx, uint32_t quality) {
	std::vector<std::byte> bytes(sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterBufferQualityLevel));
	auto* misc = reinterpret_cast<VAEncMiscParameterBuffer*>(bytes.data());
	auto* q = reinterpret_cast<VAEncMiscParameterBufferQualityLevel*>(misc->data);
	misc->type = VAEncMiscParameterTypeQualityLevel;
	q->quality_level = quality;
	return create_raw_buffer(dpy, ctx, VAEncMiscParameterBufferType, bytes);
}

struct PackedHeaderBuffers {
	BufferGuard params;
	BufferGuard data;
};

PackedHeaderBuffers create_packed_header(VADisplay dpy, VAContextID ctx, uint32_t type, std::span<const std::byte> bytes) {
	VAEncPackedHeaderParameterBuffer params{};
	params.type = type;
	params.bit_length = static_cast<unsigned int>(bytes.size() * 8);
	params.has_emulation_bytes = 1;
	return {
		create_buffer(dpy, ctx, VAEncPackedHeaderParameterBufferType, params),
		create_raw_buffer(dpy, ctx, VAEncPackedHeaderDataBufferType, bytes),
	};
}

EncoderParamInfo bool_param(
	std::string name,
	std::string label,
	std::string group,
	bool defaultValue,
	std::string help
) {
	EncoderParamInfo info;
	info.name = std::move(name);
	info.label = std::move(label);
	info.group = std::move(group);
	info.kind = ParamKind::Bool;
	info.defaultValue = defaultValue;
	info.help = std::move(help);
	return info;
}

EncoderParamInfo int_param(
	std::string name,
	std::string label,
	std::string group,
	int64_t defaultValue,
	IntRange range,
	std::string help
) {
	EncoderParamInfo info;
	info.name = std::move(name);
	info.label = std::move(label);
	info.group = std::move(group);
	info.kind = ParamKind::Int;
	info.defaultValue = defaultValue;
	info.intRange = range;
	info.help = std::move(help);
	return info;
}

EncoderParamInfo enum_param(
	std::string name,
	std::string label,
	std::string group,
	std::string defaultValue,
	std::vector<EnumValue> values,
	std::string help
) {
	EncoderParamInfo info;
	info.name = std::move(name);
	info.label = std::move(label);
	info.group = std::move(group);
	info.kind = ParamKind::Enum;
	info.defaultValue = defaultValue;
	info.enumValues = std::move(values);
	info.help = std::move(help);
	return info;
}

std::vector<std::byte> collect_coded_buffer(VADisplay dpy, VABufferID codedBuffer) {
	void* mapped = nullptr;
	checked(vaMapBuffer(dpy, codedBuffer, &mapped), "map coded buffer");
	struct UnmapGuard {
		VADisplay dpy = nullptr;
		VABufferID id = VA_INVALID_ID;
		~UnmapGuard() {
			if (dpy != nullptr && id != VA_INVALID_ID) {
				(void)vaUnmapBuffer(dpy, id);
			}
		}
	} unmap{dpy, codedBuffer};

	std::vector<std::byte> out;
	for (auto* segment = static_cast<VACodedBufferSegment*>(mapped);
	     segment != nullptr;
	     segment = static_cast<VACodedBufferSegment*>(segment->next)) {
		if ((segment->status & VA_CODED_BUF_STATUS_BAD_BITSTREAM) != 0) {
			throw std::runtime_error("VA-API encoder reported a bad bitstream");
		}
		const auto* first = reinterpret_cast<const std::byte*>(segment->buf);
		out.insert(out.end(), first, first + segment->size);
	}
	return out;
}

VAProfile hevc_profile_for_format(PixelFormat format, bool scc) {
	if (is_422(format)) {
		if (scc) {
			throw std::runtime_error("VA-API HEVC SCC profiles on this driver do not include 4:2:2");
		}
		return VAProfileHEVCMain422_10;
	}
	if (is_444(format)) {
		if (scc) {
			return bit_depth(format) == 8 ? VAProfileHEVCSccMain444 : VAProfileHEVCSccMain444_10;
		}
		return bit_depth(format) == 8 ? VAProfileHEVCMain444 : VAProfileHEVCMain444_10;
	}
	if (scc) {
		return bit_depth(format) == 8 ? VAProfileHEVCSccMain : VAProfileHEVCSccMain10;
	}
	return bit_depth(format) == 8 ? VAProfileHEVCMain : VAProfileHEVCMain10;
}

void fill_equal_hevc_tiles(VAEncPictureParameterBufferHEVC& pic, uint32_t width, uint32_t height, uint32_t cols, uint32_t rows) {
	const uint32_t ctb = 64;
	const uint32_t ctuCols = (width + ctb - 1) / ctb;
	const uint32_t ctuRows = (height + ctb - 1) / ctb;
	cols = std::clamp<uint32_t>(cols, 1, static_cast<uint32_t>(std::size(pic.column_width_minus1)));
	rows = std::clamp<uint32_t>(rows, 1, static_cast<uint32_t>(std::size(pic.row_height_minus1)));
	if (cols > ctuCols || rows > ctuRows) {
		throw std::runtime_error("VA-API HEVC tile count exceeds CTU grid");
	}
	pic.num_tile_columns_minus1 = static_cast<uint8_t>(cols - 1);
	pic.num_tile_rows_minus1 = static_cast<uint8_t>(rows - 1);
	pic.pic_fields.bits.tiles_enabled_flag = (cols > 1 || rows > 1) ? 1u : 0u;
	for (uint32_t col = 0; col < cols; ++col) {
		const uint32_t start = (ctuCols * col) / cols;
		const uint32_t end = (ctuCols * (col + 1)) / cols;
		pic.column_width_minus1[col] = static_cast<uint8_t>(end - start - 1);
	}
	for (uint32_t row = 0; row < rows; ++row) {
		const uint32_t start = (ctuRows * row) / rows;
		const uint32_t end = (ctuRows * (row + 1)) / rows;
		pic.row_height_minus1[row] = static_cast<uint8_t>(end - start - 1);
	}
}

struct VaEncodeObjects {
	VaDisplay display;
	ConfigGuard config;
	ContextGuard context;
	SurfaceGuard input;
	SurfaceGuard recon;
	BufferGuard coded;

	VaEncodeObjects(const RawImage& image, VAProfile profile, uint32_t rtFormat, uint32_t pixelFormat, const std::string& device, uint32_t packedHeaders, uint32_t rcMode, bool contextRenderTargets = true)
		: display(device),
		  config(display.dpy, create_config(display.dpy, profile, rtFormat, packedHeaders, rcMode)),
		  input(display.dpy, create_surface(display.dpy, image, rtFormat, pixelFormat)),
		  recon(display.dpy, create_surface(display.dpy, image, rtFormat, pixelFormat)) {
		VAContextID rawContext = VA_INVALID_ID;
		VASurfaceID renderTargets[] = {input.surface, recon.surface};
		checked(
			vaCreateContext(
				display.dpy,
				config.id,
				image.width,
				image.height,
				VA_PROGRESSIVE,
				contextRenderTargets ? renderTargets : nullptr,
				contextRenderTargets ? 2 : 0,
				&rawContext
			),
			"create encode context"
		);
		context = ContextGuard(display.dpy, rawContext);

		const unsigned int codedSize = static_cast<unsigned int>(std::max<std::size_t>(1024 * 1024, static_cast<std::size_t>(image.width) * image.height * 4));
		VABufferID codedId = VA_INVALID_ID;
		checked(vaCreateBuffer(display.dpy, context.id, VAEncCodedBufferType, codedSize, 1, nullptr, &codedId), "create coded buffer");
		coded = BufferGuard(display.dpy, codedId);
	}
	};

} // namespace

std::vector<EncoderParamInfo> query_vaapi_hevc_parameters() {
	std::vector<EncoderParamInfo> out{
		enum_param("rate-control", "Mode", "Rate Control", "cqp", {{"cqp", "CQP"}, {"icq", "ICQ"}, {"vbr", "VBR"}, {"cbr", "CBR"}}, "VA-API rate-control mode advertised by this driver."),
		int_param("qpi", "QP", "Rate Control", 35, {0, 51, 1}, "I-picture quantization parameter for still-image encoding."),
		int_param("bitrate-kbps", "Bitrate", "Rate Control", 10000, {1, 1000000, 1000}, "Target bitrate for CBR/VBR/ICQ modes."),
		int_param("target-usage", "Target usage", "Speed / Quality", 4, {0, 7, 1}, "VA quality level: 1 highest quality, 7 fastest, 0 driver default."),
		bool_param("scc", "SCC", "Profile / Level", false, "Use HEVC Screen Content Coding profiles where supported."),
		int_param("level-idc", "Level IDC", "Profile / Level", 120, {30, 255, 3}, "HEVC general_level_idc value."),
		bool_param("high-tier", "High tier", "Profile / Level", false, "Set HEVC general_tier_flag."),
		enum_param("bit-depth", "Bit depth", "Compression", "source", {{"source", "Source"}, {"8", "8-bit"}, {"10", "10-bit"}}, "Encode bit depth."),
		enum_param(
			"chroma-subsampling",
			"Chroma subsampling",
			"Compression",
			"420",
			{{"420", "4:2:0"}, {"422", "4:2:2"}, {"444", "4:4:4"}},
			"Encode chroma format."
		),
		enum_param("color-primaries", "Color primaries", "Color", "source", {{"source", "Source"}, {"bt709", "BT.709"}, {"bt2020", "BT.2020"}}, "Color primaries metadata."),
		enum_param("transfer", "Transfer", "Color", "source", {{"source", "Source"}, {"srgb", "sRGB"}, {"bt709", "BT.709"}, {"pq", "PQ"}, {"hlg", "HLG"}}, "Transfer characteristics metadata."),
		enum_param("matrix", "Matrix", "Color", "source", {{"source", "Source"}, {"bt709", "BT.709"}, {"bt2020nc", "BT.2020 non-constant"}}, "Matrix coefficients metadata."),
		enum_param("range", "Range", "Color", "source", {{"source", "Source"}, {"limited", "Limited"}, {"full", "Full"}}, "YUV sample range metadata."),
		bool_param("sao", "SAO", "Coding Tools", true, "Enable HEVC sample adaptive offset."),
		bool_param("slice-sao-luma", "Slice SAO luma", "Coding Tools", true, "Enable luma SAO in the packed slice header."),
		bool_param("slice-sao-chroma", "Slice SAO chroma", "Coding Tools", true, "Enable chroma SAO in the packed slice header."),
		bool_param("strong-intra-smoothing", "Strong intra smoothing", "Coding Tools", false, "Enable HEVC strong intra smoothing."),
		bool_param("constrained-intra", "Constrained intra", "Coding Tools", false, "Enable constrained intra prediction."),
		bool_param("transform-skip", "Transform skip", "Coding Tools", true, "Enable transform skip."),
		bool_param("sign-data-hiding", "Sign data hiding", "Coding Tools", false, "Enable sign data hiding."),
		bool_param("cu-qp-delta", "CU QP delta", "Coding Tools", false, "Enable CU QP delta signalling."),
		int_param("pps-cb-qp-offset", "PPS Cb QP offset", "Quantization", 0, {-12, 12, 1}, "PPS Cb QP offset."),
		int_param("pps-cr-qp-offset", "PPS Cr QP offset", "Quantization", 0, {-12, 12, 1}, "PPS Cr QP offset."),
		int_param("slice-cb-qp-offset", "Slice Cb QP offset", "Quantization", 0, {-12, 12, 1}, "Slice Cb QP offset."),
		int_param("slice-cr-qp-offset", "Slice Cr QP offset", "Quantization", 0, {-12, 12, 1}, "Slice Cr QP offset."),
		int_param("slice-beta-offset-div2", "Deblock beta offset / 2", "Deblocking", 0, {-6, 6, 1}, "HEVC slice_beta_offset_div2."),
		int_param("slice-tc-offset-div2", "Deblock tc offset / 2", "Deblocking", 0, {-6, 6, 1}, "HEVC slice_tc_offset_div2."),
		int_param("num-tile-cols", "Tile columns", "Partitioning", 1, {1, 19, 1}, "Number of HEVC tile columns."),
		int_param("num-tile-rows", "Tile rows", "Partitioning", 1, {1, 21, 1}, "Number of HEVC tile rows."),
		bool_param("entropy-coding-sync", "WPP", "Partitioning", false, "Enable wavefront parallel processing."),
		bool_param("loop-filter-across-tiles", "Filter across tiles", "Partitioning", true, "Allow filters across tile boundaries."),
		bool_param("loop-filter-across-slices", "Filter across slices", "Partitioning", true, "Allow filters across slice boundaries."),
	};
	return out;
}

EncodedImage encode_vaapi_hevc_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const bool scc = get_param<bool>(params, "scc", false);
	const std::string chroma = get_param<std::string>(params, "chroma-subsampling", "420");
	if (scc && chroma == "422") {
		throw std::runtime_error("VA-API HEVC SCC profiles on this driver do not include 4:2:2");
	}
	const int targetDepth = requested_bit_depth(params, image);
	RawImage encodeImage = convert_yuv_format(image, vaapi_pixel_format(targetDepth, chroma));
	const int depth = bit_depth(encodeImage.format);
	const bool yuv444 = is_444(encodeImage.format);
	const bool yuv422 = is_422(encodeImage.format);
	const VAProfile profile = hevc_profile_for_format(encodeImage.format, scc);
	const uint32_t rtFormat = yuv444
		? (depth == 8 ? VA_RT_FORMAT_YUV444 : VA_RT_FORMAT_YUV444_10)
		: (yuv422
			? (depth == 8 ? VA_RT_FORMAT_YUV422 : VA_RT_FORMAT_YUV422_10)
			: (depth == 8 ? VA_RT_FORMAT_YUV420 : VA_RT_FORMAT_YUV420_10));
	const uint32_t pixelFormat = yuv444
		? (depth == 8 ? VA_FOURCC_444P : VA_FOURCC_Y410)
		: (yuv422
			? (depth == 8 ? VA_FOURCC_YUY2 : VA_FOURCC_Y210)
			: (depth == 8 ? VA_FOURCC_NV12 : VA_FOURCC_P010));
	const uint32_t qpi = static_cast<uint32_t>(get_int_param(params, "qpi", 35));
	const uint32_t rcMode = vaapi_rate_control_mode(get_param<std::string>(params, "rate-control", "cqp"));
	const uint32_t bitrateKbps = static_cast<uint32_t>(std::clamp<int64_t>(get_int_param(params, "bitrate-kbps", 10000), 1, 1000000));
	const uint32_t quality = static_cast<uint32_t>(get_int_param(params, "target-usage", 4));
	const std::string device = "/dev/dri/renderD128";
	if (scc && rcMode != VA_RC_CQP) {
		throw std::runtime_error("VA-API HEVC SCC profiles on this driver advertise only CQP rate control");
	}
	VaEncodeObjects va(
		encodeImage,
			profile,
			rtFormat,
			pixelFormat,
			device,
			VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_SLICE,
			rcMode
	);
	upload_yuv_to_surface(va.display.dpy, va.input.surface, encodeImage);
	checked(vaSyncSurface(va.display.dpy, va.input.surface), "sync HEVC input surface");

	VAEncSequenceParameterBufferHEVC seq{};
	seq.general_profile_idc = (yuv444 || yuv422) ? 4 : (depth == 8 ? 1 : 2);
	seq.general_level_idc = static_cast<uint8_t>(get_int_param(params, "level-idc", 120));
	seq.general_tier_flag = get_param<bool>(params, "high-tier", false) ? 1 : 0;
	seq.intra_period = 1;
	seq.intra_idr_period = 1;
	seq.ip_period = 1;
	seq.pic_width_in_luma_samples = static_cast<uint16_t>(encodeImage.width);
	seq.pic_height_in_luma_samples = static_cast<uint16_t>(encodeImage.height);
	seq.seq_fields.bits.chroma_format_idc = yuv444 ? 3u : (yuv422 ? 2u : 1u);
	seq.seq_fields.bits.bit_depth_luma_minus8 = static_cast<uint32_t>(depth - 8);
	seq.seq_fields.bits.bit_depth_chroma_minus8 = static_cast<uint32_t>(depth - 8);
	seq.seq_fields.bits.strong_intra_smoothing_enabled_flag = get_param<bool>(params, "strong-intra-smoothing", false) ? 1u : 0u;
	seq.seq_fields.bits.sample_adaptive_offset_enabled_flag = get_param<bool>(params, "sao", true) ? 1u : 0u;
	seq.log2_min_luma_coding_block_size_minus3 = 0;
	seq.log2_diff_max_min_luma_coding_block_size = 3;
	seq.log2_min_transform_block_size_minus2 = 0;
	seq.log2_diff_max_min_transform_block_size = 3;
	seq.max_transform_hierarchy_depth_inter = 2;
	seq.max_transform_hierarchy_depth_intra = 2;

	VAEncPictureParameterBufferHEVC pic{};
	pic.decoded_curr_pic.picture_id = va.recon.surface;
	pic.decoded_curr_pic.pic_order_cnt = 0;
	for (auto& ref : pic.reference_frames) {
		ref.picture_id = VA_INVALID_SURFACE;
		ref.flags = VA_PICTURE_HEVC_INVALID;
	}
	pic.coded_buf = va.coded.id;
	pic.collocated_ref_pic_index = 0xff;
	pic.pic_init_qp = static_cast<uint8_t>(qpi);
	pic.pps_cb_qp_offset = static_cast<int8_t>(get_int_param(params, "pps-cb-qp-offset", 0));
	pic.pps_cr_qp_offset = static_cast<int8_t>(get_int_param(params, "pps-cr-qp-offset", 0));
	pic.nal_unit_type = 19;
	pic.pic_fields.bits.idr_pic_flag = 1;
	pic.pic_fields.bits.coding_type = 1;
	pic.pic_fields.bits.reference_pic_flag = 1;
	pic.pic_fields.bits.sign_data_hiding_enabled_flag = get_param<bool>(params, "sign-data-hiding", false) ? 1u : 0u;
	pic.pic_fields.bits.constrained_intra_pred_flag = get_param<bool>(params, "constrained-intra", false) ? 1u : 0u;
	pic.pic_fields.bits.transform_skip_enabled_flag = get_param<bool>(params, "transform-skip", true) ? 1u : 0u;
	pic.pic_fields.bits.cu_qp_delta_enabled_flag = get_param<bool>(params, "cu-qp-delta", false) ? 1u : 0u;
	pic.pic_fields.bits.entropy_coding_sync_enabled_flag = get_param<bool>(params, "entropy-coding-sync", false) ? 1u : 0u;
	pic.pic_fields.bits.loop_filter_across_tiles_enabled_flag = get_param<bool>(params, "loop-filter-across-tiles", true) ? 1u : 0u;
	pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = get_param<bool>(params, "loop-filter-across-slices", true) ? 1u : 0u;
	fill_equal_hevc_tiles(
		pic,
		static_cast<uint32_t>(encodeImage.width),
		static_cast<uint32_t>(encodeImage.height),
		static_cast<uint32_t>(get_int_param(params, "num-tile-cols", 1)),
		static_cast<uint32_t>(get_int_param(params, "num-tile-rows", 1))
	);

	VAEncSliceParameterBufferHEVC slice{};
	slice.slice_segment_address = 0;
	const int ctb = 64;
	slice.num_ctu_in_slice = static_cast<uint32_t>(((encodeImage.width + ctb - 1) / ctb) * ((encodeImage.height + ctb - 1) / ctb));
	slice.slice_type = 2;
	for (auto& ref : slice.ref_pic_list0) {
		ref.picture_id = VA_INVALID_SURFACE;
		ref.flags = VA_PICTURE_HEVC_INVALID;
	}
	for (auto& ref : slice.ref_pic_list1) {
		ref.picture_id = VA_INVALID_SURFACE;
		ref.flags = VA_PICTURE_HEVC_INVALID;
	}
	slice.max_num_merge_cand = 1;
	slice.slice_cb_qp_offset = static_cast<int8_t>(get_int_param(params, "slice-cb-qp-offset", 0));
	slice.slice_cr_qp_offset = static_cast<int8_t>(get_int_param(params, "slice-cr-qp-offset", 0));
	slice.slice_beta_offset_div2 = static_cast<int8_t>(get_int_param(params, "slice-beta-offset-div2", 0));
	slice.slice_tc_offset_div2 = static_cast<int8_t>(get_int_param(params, "slice-tc-offset-div2", 0));
	slice.slice_qp_delta = 0;
	slice.slice_fields.bits.last_slice_of_pic_flag = 1;
	slice.slice_fields.bits.slice_sao_luma_flag = get_param<bool>(params, "slice-sao-luma", true) ? 1u : 0u;
	slice.slice_fields.bits.slice_sao_chroma_flag = get_param<bool>(params, "slice-sao-chroma", true) ? 1u : 0u;
	slice.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = get_param<bool>(params, "loop-filter-across-slices", true) ? 1u : 0u;

	auto seqBuf = create_buffer(va.display.dpy, va.context.id, VAEncSequenceParameterBufferType, seq);
	auto rcBuf = create_misc_rate_control(va.display.dpy, va.context.id, rcMode, qpi, bitrateKbps);
	auto frameRateBuf = create_frame_rate(va.display.dpy, va.context.id);
	auto qualityBuf = create_quality_level(va.display.dpy, va.context.id, quality);
	auto picBuf = create_buffer(va.display.dpy, va.context.id, VAEncPictureParameterBufferType, pic);
	auto sliceBuf = create_buffer(va.display.dpy, va.context.id, VAEncSliceParameterBufferType, slice);
	const bool entryPointsPresent = get_int_param(params, "num-tile-cols", 1) > 1 ||
	                                get_int_param(params, "num-tile-rows", 1) > 1 ||
	                                get_param<bool>(params, "entropy-coding-sync", false);
	const std::vector<std::byte> sliceHeader = hevc_slice_header(
		slice.slice_qp_delta,
		get_param<bool>(params, "sao", true),
		get_param<bool>(params, "loop-filter-across-slices", true),
		entryPointsPresent
	);
	const std::vector<std::byte> vpsHeader = hevc_vps_header(seq.general_profile_idc, seq.general_level_idc, seq.general_tier_flag != 0);
	const std::vector<std::byte> spsHeader = hevc_sps_header(
		encodeImage.width,
		encodeImage.height,
		seq.general_profile_idc,
		seq.general_level_idc,
		seq.general_tier_flag != 0,
		seq.seq_fields.bits.chroma_format_idc,
		seq.seq_fields.bits.bit_depth_luma_minus8,
		seq.seq_fields.bits.sample_adaptive_offset_enabled_flag != 0,
		seq.seq_fields.bits.strong_intra_smoothing_enabled_flag != 0
	);
	const std::vector<std::byte> ppsHeader = hevc_pps_header(pic);
	auto packedVps = create_packed_header(va.display.dpy, va.context.id, VAEncPackedHeaderHEVC_VPS, vpsHeader);
	auto packedSps = create_packed_header(va.display.dpy, va.context.id, VAEncPackedHeaderHEVC_SPS, spsHeader);
	auto packedPps = create_packed_header(va.display.dpy, va.context.id, VAEncPackedHeaderHEVC_PPS, ppsHeader);
	auto packedSlice = create_packed_header(va.display.dpy, va.context.id, VAEncPackedHeaderHEVC_Slice, sliceHeader);
	VABufferID buffers[] = {
		seqBuf.id,
		rcBuf.id,
		frameRateBuf.id,
		qualityBuf.id,
		picBuf.id,
		packedVps.params.id,
		packedVps.data.id,
		packedSps.params.id,
		packedSps.data.id,
		packedPps.params.id,
		packedPps.data.id,
		packedSlice.params.id,
		packedSlice.data.id,
		sliceBuf.id,
	};
	checked(vaBeginPicture(va.display.dpy, va.context.id, va.input.surface), "begin HEVC picture");
	checked(vaRenderPicture(va.display.dpy, va.context.id, buffers, static_cast<int>(std::size(buffers))), "render HEVC picture");
	checked(vaEndPicture(va.display.dpy, va.context.id), "end HEVC picture");

	EncodedImage result;
	result.hevcAnnexB = collect_coded_buffer(va.display.dpy, va.coded.id);
	if (result.hevcAnnexB.empty()) {
		throw std::runtime_error("VA-API HEVC produced an empty bitstream");
	}
	return result;
}

std::vector<EncoderParamInfo> query_vaapi_av1_parameters() {
	return {
		enum_param("rate-control", "Mode", "Rate Control", "cqp", {{"cqp", "CQP"}, {"icq", "ICQ"}, {"vbr", "VBR"}, {"cbr", "CBR"}}, "VA-API rate-control mode advertised by this driver."),
		int_param("qindex", "Q index", "Rate Control", 128, {0, 255, 1}, "AV1 base quantizer index for CQP still-image encoding."),
		int_param("bitrate-kbps", "Bitrate", "Rate Control", 10000, {1, 1000000, 1000}, "Target bitrate for CBR/VBR/ICQ modes."),
		int_param("level-idx", "Level index", "Profile / Level", -1, {-1, 20, 1}, "AV1 seq_level_idx value; -1 chooses the lowest level for the image size."),
		enum_param("bit-depth", "Bit depth", "Compression", "source", {{"source", "Source"}, {"8", "8-bit"}, {"10", "10-bit"}}, "Encode bit depth."),
		bool_param("disable-cdf-update", "Disable CDF update", "Coding Tools", false, "Disable AV1 CDF updates for the still frame."),
		bool_param("cdef", "CDEF", "Filters", true, "Enable AV1 constrained directional enhancement filtering."),
		int_param("loop-filter-level", "Loop filter level", "Filters", 0, {0, 63, 1}, "AV1 luma loop filter level."),
		enum_param("tx-mode", "TX mode", "Transform", "largest", {{"largest", "Largest"}}, "AV1 transform mode."),
		int_param("tile-columns", "Tile columns", "Partitioning", 1, {1, 64, 1}, "Number of AV1 tile columns."),
		int_param("tile-rows", "Tile rows", "Partitioning", 1, {1, 64, 1}, "Number of AV1 tile rows."),
	};
}

EncodedImage encode_vaapi_av1_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const int targetDepth = requested_bit_depth(params, image);
	if (targetDepth != 8 && targetDepth != 10) {
		throw std::runtime_error("VA-API AV1 accepts 8-bit or 10-bit input");
	}
	RawImage encodeImage = convert_yuv_format(image, targetDepth == 8 ? PixelFormat::YUV420P8 : PixelFormat::YUV420P10LE);
	const int depth = bit_depth(encodeImage.format);
	const uint32_t rtFormat = depth == 8 ? VA_RT_FORMAT_YUV420 : VA_RT_FORMAT_YUV420_10;
	const uint32_t pixelFormat = depth == 8 ? VA_FOURCC_NV12 : VA_FOURCC_P010;
	const uint32_t qindex = static_cast<uint32_t>(std::clamp<int64_t>(get_int_param_alias(params, "qindex", "qp", 128), 0, 255));
	const uint32_t rcMode = vaapi_rate_control_mode(get_param<std::string>(params, "rate-control", "cqp"));
	const uint32_t bitrateKbps = static_cast<uint32_t>(std::clamp<int64_t>(get_int_param(params, "bitrate-kbps", 10000), 1, 1000000));
	const uint8_t levelIdx = av1_level_idx_from_params(params, encodeImage.width, encodeImage.height);
	const bool highTier = false;
	const bool use128 = false;
	const bool filterIntra = false;
	const bool intraEdge = false;
	const bool screenContent = false;
	const bool intrabc = false;
	const bool palette = false;
	const bool disableCdf = get_param<bool>(params, "disable-cdf-update", false);
	const bool cdef = get_param<bool>(params, "cdef", true);
	const bool restoration = false;
	const int filterLevel = static_cast<int>(std::clamp<int64_t>(get_int_param(params, "loop-filter-level", 0), 0, 63));
	const int txMode = 1;
	const uint32_t tileCols = static_cast<uint32_t>(std::clamp<int64_t>(get_int_param(params, "tile-columns", 1), 1, 64));
	const uint32_t tileRows = static_cast<uint32_t>(std::clamp<int64_t>(get_int_param(params, "tile-rows", 1), 1, 64));
	const Av1TileLayout tiles = av1_tile_layout(encodeImage.width, encodeImage.height, use128, tileCols, tileRows);

	VaEncodeObjects va(
		encodeImage,
		VAProfileAV1Profile0,
		rtFormat,
		pixelFormat,
		"/dev/dri/renderD128",
		VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE,
		rcMode
	);
	upload_yuv_to_surface(va.display.dpy, va.input.surface, encodeImage);
	checked(vaSyncSurface(va.display.dpy, va.input.surface), "sync AV1 input surface");

	VAEncSequenceParameterBufferAV1 seq{};
	seq.seq_profile = 0;
	seq.seq_level_idx = levelIdx;
	seq.seq_tier = highTier ? 1 : 0;
	seq.intra_period = 1;
	seq.ip_period = 1;
	seq.seq_fields.bits.still_picture = 1;
	seq.seq_fields.bits.use_128x128_superblock = use128 ? 1u : 0u;
	seq.seq_fields.bits.enable_filter_intra = filterIntra ? 1u : 0u;
	seq.seq_fields.bits.enable_intra_edge_filter = intraEdge ? 1u : 0u;
	seq.seq_fields.bits.enable_cdef = cdef ? 1u : 0u;
	seq.seq_fields.bits.enable_restoration = restoration ? 1u : 0u;
	seq.seq_fields.bits.bit_depth_minus8 = static_cast<uint32_t>(depth - 8);
	seq.seq_fields.bits.subsampling_x = 1;
	seq.seq_fields.bits.subsampling_y = 1;
	seq.order_hint_bits_minus_1 = 7;

	VAEncPictureParameterBufferAV1 pic{};
	pic.frame_width_minus_1 = static_cast<uint16_t>(encodeImage.width - 1);
	pic.frame_height_minus_1 = static_cast<uint16_t>(encodeImage.height - 1);
	pic.reconstructed_frame = va.recon.surface;
	pic.coded_buf = va.coded.id;
	for (VASurfaceID& ref : pic.reference_frames) {
		ref = VA_INVALID_SURFACE;
	}
	for (uint8_t& refIdx : pic.ref_frame_idx) {
		refIdx = 0;
	}
	pic.primary_ref_frame = 7;
	pic.refresh_frame_flags = 0xff;
	pic.picture_flags.bits.frame_type = 0;
	pic.picture_flags.bits.error_resilient_mode = 1;
	pic.picture_flags.bits.disable_cdf_update = disableCdf ? 1u : 0u;
	pic.picture_flags.bits.enable_frame_obu = 0;
	pic.picture_flags.bits.allow_intrabc = intrabc ? 1u : 0u;
	pic.picture_flags.bits.palette_mode_enable = palette ? 1u : 0u;
	pic.picture_flags.bits.allow_screen_content_tools = screenContent ? 1u : 0u;
	pic.picture_flags.bits.force_integer_mv = 0;
	pic.filter_level[0] = static_cast<uint8_t>(filterLevel);
	pic.filter_level[1] = static_cast<uint8_t>(filterLevel);
	pic.base_qindex = static_cast<uint8_t>(qindex);
	pic.min_base_qindex = static_cast<uint8_t>(std::max<uint32_t>(1, qindex));
	pic.max_base_qindex = static_cast<uint8_t>(std::max<uint32_t>(1, qindex));
	pic.mode_control_flags.bits.tx_mode = static_cast<uint32_t>(txMode);
	pic.tile_cols = static_cast<uint8_t>(tiles.cols);
	pic.tile_rows = static_cast<uint8_t>(tiles.rows);
	const uint32_t sbSize = use128 ? 128u : 64u;
	const uint32_t sbCols = ceil_div_u32(static_cast<uint32_t>(encodeImage.width), sbSize);
	const uint32_t sbRows = ceil_div_u32(static_cast<uint32_t>(encodeImage.height), sbSize);
	const uint32_t tileWidthSb = 1u + ((sbCols - 1u) >> tiles.colLog2);
	for (uint32_t col = 0; col < tiles.cols; ++col) {
		const uint32_t startSb = std::min<uint32_t>(col * tileWidthSb, sbCols);
		const uint32_t endSb = col + 1 == tiles.cols ? sbCols : std::min<uint32_t>((col + 1u) * tileWidthSb, sbCols);
		pic.width_in_sbs_minus_1[col] = static_cast<uint16_t>(std::max<uint32_t>(1, endSb - startSb) - 1u);
	}
	const uint32_t tileHeightSb = 1u + ((sbRows - 1u) >> tiles.rowLog2);
	for (uint32_t row = 0; row < tiles.rows; ++row) {
		const uint32_t startSb = std::min<uint32_t>(row * tileHeightSb, sbRows);
		const uint32_t endSb = row + 1 == tiles.rows ? sbRows : std::min<uint32_t>((row + 1u) * tileHeightSb, sbRows);
		pic.height_in_sbs_minus_1[row] = static_cast<uint16_t>(std::max<uint32_t>(1, endSb - startSb) - 1u);
	}
	pic.cdef_damping_minus_3 = 0;
	pic.cdef_bits = 0;
	pic.loop_restoration_flags.bits.yframe_restoration_type = restoration ? 1u : 0u;
	pic.loop_restoration_flags.bits.cbframe_restoration_type = restoration ? 1u : 0u;
	pic.loop_restoration_flags.bits.crframe_restoration_type = restoration ? 1u : 0u;
	pic.byte_offset_frame_hdr_obu_size = 0;
	pic.bit_offset_qindex = 0;

	VAEncTileGroupBufferAV1 tileGroup{};
	tileGroup.tg_start = 0;
	tileGroup.tg_end = static_cast<uint8_t>(tiles.cols * tiles.rows - 1u);

	const std::vector<std::byte> sequenceHeader = av1_sequence_header_obu(
		encodeImage.width,
		encodeImage.height,
		depth,
		levelIdx,
		highTier,
		use128,
		filterIntra,
		intraEdge,
		cdef,
		restoration
	);
	const std::vector<std::byte> frameHeader = av1_frame_header_obu(
		encodeImage.width,
		encodeImage.height,
		use128,
		tiles,
		static_cast<int>(qindex),
		disableCdf,
		screenContent,
		intrabc,
		palette,
		txMode == 1 ? 0 : 1,
		filterLevel,
		cdef
	);
	pic.size_in_bits_frame_hdr_obu = 0;
	pic.tile_group_obu_hdr_info.bits.obu_has_size_field = 1;
	auto seqBuf = create_buffer(va.display.dpy, va.context.id, VAEncSequenceParameterBufferType, seq);
	auto rcBuf = create_misc_rate_control(va.display.dpy, va.context.id, rcMode, qindex, bitrateKbps);
	auto picBuf = create_buffer(va.display.dpy, va.context.id, VAEncPictureParameterBufferType, pic);
	auto packedSeq = create_packed_header(va.display.dpy, va.context.id, VAEncPackedHeaderAV1_SPS, sequenceHeader);
	auto packedPic = create_packed_header(va.display.dpy, va.context.id, VAEncPackedHeaderAV1_PPS, frameHeader);
	auto tileBuf = create_buffer(va.display.dpy, va.context.id, VAEncSliceParameterBufferType, tileGroup);
	VABufferID buffers[] = {
		seqBuf.id,
		rcBuf.id,
		picBuf.id,
		packedSeq.params.id,
		packedSeq.data.id,
		packedPic.params.id,
		packedPic.data.id,
		tileBuf.id,
	};
	checked(vaBeginPicture(va.display.dpy, va.context.id, va.input.surface), "begin AV1 picture");
	checked(vaRenderPicture(va.display.dpy, va.context.id, buffers, static_cast<int>(std::size(buffers))), "render AV1 picture");
	checked(vaEndPicture(va.display.dpy, va.context.id), "end AV1 picture");

	const std::vector<std::byte> encodedTileGroup = collect_coded_buffer(va.display.dpy, va.coded.id);
	if (encodedTileGroup.empty()) {
		throw std::runtime_error("VA-API AV1 produced an empty bitstream");
	}
	std::vector<std::byte> frame;
	const std::vector<std::byte> td = av1_temporal_delimiter_obu();
	frame.insert(frame.end(), td.begin(), td.end());
	frame.insert(frame.end(), encodedTileGroup.begin(), encodedTileGroup.end());
	EncodedImage result;
	append_ivf_header(result.hevcAnnexB, encodeImage.width, encodeImage.height, 1);
	append_ivf_frame(result.hevcAnnexB, frame, 0);
	return result;
}

} // namespace codec_gui
