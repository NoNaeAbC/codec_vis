#pragma once

#include "native_video/h265/api.hpp"

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif
#include <gst/codecparsers/gsth265parser.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace native_video::h265::decoder {
	// Codec-semantic RPS membership. Values intentionally match the VA-API
	// picture flags so the VA lowering remains a zero-cost field copy.
	inline constexpr std::uint32_t kReference_St_Curr_Before = 0x00000010U;
	inline constexpr std::uint32_t kReference_St_Curr_After  = 0x00000020U;
	inline constexpr std::uint32_t kReference_Lt_Curr        = 0x00000040U;

	enum class Nal_Unit_Type : std::uint8_t {
		TRAIL_N    = 0,
		TRAIL_R    = 1,
		TSA_N      = 2,
		TSA_R      = 3,
		STSA_N     = 4,
		STSA_R     = 5,
		RADL_N     = 6,
		RADL_R     = 7,
		RASL_N     = 8,
		RASL_R     = 9,
		BLA_W_LP   = 16,
		BLA_W_RADL = 17,
		BLA_N_LP   = 18,
		IDR_W_RADL = 19,
		IDR_N_LP   = 20,
		CRA        = 21,
		VPS        = 32,
		SPS        = 33,
		PPS        = 34,
		AUD        = 35,
		EOS        = 36,
		EOB        = 37,
		FD         = 38,
		PREFIX_SEI = 39,
		SUFFIX_SEI = 40,
	};

	struct Nal_Unit {
		Nal_Unit_Type type_              = Nal_Unit_Type::TRAIL_N;
		std::uint8_t  layer_id_          = 0;
		std::uint8_t  temporal_id_       = 0;
		std::uint64_t start_code_offset_ = 0;
		std::uint64_t nal_offset_        = 0;
		std::uint64_t payload_offset_    = 0;
		std::uint64_t end_offset_        = 0;
	};

	struct Frame_Reference_State {
		std::int32_t                  pic_order_cnt_     = 0;
		std::uint16_t                 pic_order_cnt_lsb_ = 0;
		std::uint8_t                  current_dpb_slot_  = 0xFFU;
		bool                          non_reference_     = false;
		bool                          output_            = true;
		bool                          field_picture_     = false;
		bool                          bottom_field_      = false;
		std::array<std::uint8_t, 30>  ref_pic_list_slot_{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
												 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
												 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
												 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
		std::array<std::uint8_t, 8>   ref_pic_set_st_curr_before_{0xFFU, 0xFFU, 0xFFU, 0xFFU,
														0xFFU, 0xFFU, 0xFFU, 0xFFU};
		std::array<std::uint8_t, 8>   ref_pic_set_st_curr_after_{0xFFU, 0xFFU, 0xFFU, 0xFFU,
													   0xFFU, 0xFFU, 0xFFU, 0xFFU};
		std::array<std::uint8_t, 8>   ref_pic_set_lt_curr_{0xFFU, 0xFFU, 0xFFU, 0xFFU,
													 0xFFU, 0xFFU, 0xFFU, 0xFFU};
		std::array<bool, 15>          reference_long_term_{};
		std::array<bool, 15>          reference_unavailable_{};
		std::array<bool, 15>          reference_field_picture_{};
		std::array<bool, 15>          reference_bottom_field_{};
		std::array<std::int32_t, 15>  reference_pic_order_cnt_{};
		std::array<std::uint32_t, 15> reference_va_flags_{};
		std::array<std::uint8_t, 15>  reference_dpb_slot_{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
														  0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
	};

	struct Parsed_Decode_Slice {
		GstH265NalUnit  nalu_{};
		std::uint64_t   logical_offset_ = 0;
		GstH265SliceHdr header_{};
	};

	struct H265_Parameter_Snapshot {
		GstH265VPS vps_{};
		GstH265SPS sps_{};
		GstH265PPS pps_{};
	};

	struct Parsed_Decode_Frame {
		std::shared_ptr<H265_Parameter_Snapshot> parameters_;
		std::vector<Parsed_Decode_Slice>         slices_;
	};

	struct Parsed_Bitstream {
		H265_Sequence_Info                      sequence_;
		std::vector<Nal_Unit>                   nals_;
		std::vector<H265_Frame_Decode_Metadata> frame_metadata_;
		std::vector<Frame_Reference_State>      frame_states_;
		std::vector<Parsed_Decode_Frame>        decode_frames_;
		bool                                    has_sps_              = false;
		bool                                    requires_unsupported_ = false;
		std::uint64_t                           unsupported_offset_   = 0;
		std::string                             unsupported_message_;
	};

	class Annex_B_Parser {
	public:
		[[nodiscard]] Result<Parsed_Bitstream> parse(std::span<const std::uint8_t> bitstream) const;
		[[nodiscard]] Result<Parsed_Bitstream> parse_hevc_samples(
				std::span<const std::uint8_t> decoder_configuration,
				std::span<const av1::Borrowed_Bitstream_Extent> access_units,
				std::uint8_t nal_length_size) const;
	};
} // namespace native_video::h265::decoder
