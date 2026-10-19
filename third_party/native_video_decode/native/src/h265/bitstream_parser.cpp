#include <cstdint>
#include "h265/decoder_internal.hpp"
#include "h265/independent_scc_slice.hpp"
#include "native_video/color/hdr10plus_t35.hpp"

#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth265parser.h>

#include <va/va.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace native_video::h265::decoder {
	namespace {
		struct Parser_Deleter {
			void operator()(GstH265Parser *parser) const {
				if (parser != nullptr) { gst_h265_parser_free(parser); }
			}
		};

		using Parser_Ptr = std::unique_ptr<GstH265Parser, Parser_Deleter>;

		constexpr std::size_t kVirtual_Dpb_Slot_Count = 16U;

		[[nodiscard]] Media_Error h265_error(std::uint64_t byte_offset, std::uint64_t bit_offset, std::string message) {
			return Media_Error{
					.code_        = Media_Error_Code::INVALID_H265_BITSTREAM,
					.byte_offset_ = byte_offset,
					.bit_offset_  = bit_offset,
					.message_     = std::move(message),
			};
		}

		[[nodiscard]] std::string parser_result_message(GstH265ParserResult result) {
			switch (result) {
				case GST_H265_PARSER_OK:
					return "H.265 parser succeeded";
				case GST_H265_PARSER_BROKEN_DATA:
					return "H.265 parser reported broken bitstream data";
				case GST_H265_PARSER_BROKEN_LINK:
					return "H.265 parser could not resolve referenced parameter sets";
				case GST_H265_PARSER_ERROR:
					return "H.265 parser failed";
				case GST_H265_PARSER_NO_NAL:
					return "H.265 parser found no NAL unit";
				case GST_H265_PARSER_NO_NAL_END:
					return "H.265 parser found a NAL unit without a following start code";
			}
			return "H.265 parser returned an unknown result";
		}

		[[nodiscard]] bool is_vcl_type(guint8 type) {
			return type <= GST_H265_NAL_SLICE_RASL_R ||
				   (type >= GST_H265_NAL_SLICE_BLA_W_LP && type <= GST_H265_NAL_SLICE_CRA_NUT);
		}

		[[nodiscard]] bool is_irap_type(guint8 type) { return GST_H265_IS_NAL_TYPE_IRAP(type); }
		[[nodiscard]] bool is_idr_type(guint8 type) { return GST_H265_IS_NAL_TYPE_IDR(type); }
		[[nodiscard]] bool is_bla_type(guint8 type) { return GST_H265_IS_NAL_TYPE_BLA(type); }
		[[nodiscard]] bool is_cra_type(guint8 type) { return GST_H265_IS_NAL_TYPE_CRA(type); }
		[[nodiscard]] bool is_rasl_or_radl(guint8 type) {
			return GST_H265_IS_NAL_TYPE_RASL(type) || GST_H265_IS_NAL_TYPE_RADL(type);
		}

		[[nodiscard]] bool is_non_reference_vcl(guint8 type) {
			return type == GST_H265_NAL_SLICE_TRAIL_N || type == GST_H265_NAL_SLICE_TSA_N ||
				   type == GST_H265_NAL_SLICE_STSA_N || type == GST_H265_NAL_SLICE_RADL_N ||
				   type == GST_H265_NAL_SLICE_RASL_N;
		}

		void inherit_dependent_slice_header(GstH265SliceHdr &slice, const std::optional<GstH265SliceHdr> &independent) {
			if (slice.dependent_slice_segment_flag == 0U || !independent) { return; }

			const auto dependent_first_slice_segment = slice.first_slice_segment_in_pic_flag;
			const auto dependent_segment_address     = slice.segment_address;
			const auto dependent_header_size         = slice.header_size;
			const auto dependent_epb_count           = slice.n_emulation_prevention_bytes;
			auto      *dependent_pps                 = slice.pps;

			slice                                 = *independent;
			slice.first_slice_segment_in_pic_flag = dependent_first_slice_segment;
			slice.dependent_slice_segment_flag    = 1U;
			slice.segment_address                 = dependent_segment_address;
			slice.header_size                     = dependent_header_size;
			slice.n_emulation_prevention_bytes    = dependent_epb_count;
			if (dependent_pps != nullptr) { slice.pps = dependent_pps; }
		}

		[[nodiscard]] std::shared_ptr<H265_Parameter_Snapshot> snapshot_parameter_sets(const GstH265SliceHdr &slice) {
			auto snapshot  = std::make_shared<H265_Parameter_Snapshot>();
			snapshot->pps_ = *slice.pps;
			snapshot->sps_ = *slice.pps->sps;
			if (slice.pps->sps->vps != nullptr) {
				snapshot->vps_     = *slice.pps->sps->vps;
				snapshot->sps_.vps = &snapshot->vps_;
			} else {
				snapshot->sps_.vps = nullptr;
			}
			snapshot->pps_.sps = &snapshot->sps_;
			return snapshot;
		}

		[[nodiscard]] Nal_Unit_Type to_internal_type(guint8 type) { return static_cast<Nal_Unit_Type>(type); }

		[[nodiscard]] H265_Slice_Type to_internal_slice_type(guint8 type) {
			switch (type) {
				case GST_H265_B_SLICE:
					return H265_Slice_Type::B;
				case GST_H265_P_SLICE:
					return H265_Slice_Type::P;
				case GST_H265_I_SLICE:
					return H265_Slice_Type::I;
				default:
					return H265_Slice_Type::I;
			}
		}

		struct Field_Picture_Info {
			bool field_picture_ = false;
			bool bottom_field_  = false;
		};

		[[nodiscard]] std::optional<Field_Picture_Info> field_picture_info_from_pic_struct(guint8 pic_struct) {
			switch (pic_struct) {
				case GST_H265_SEI_PIC_STRUCT_TOP_FIELD:
				case GST_H265_SEI_PIC_STRUCT_TOP_PAIRED_PREVIOUS_BOTTOM:
				case GST_H265_SEI_PIC_STRUCT_TOP_PAIRED_NEXT_BOTTOM:
					return Field_Picture_Info{.field_picture_ = true, .bottom_field_ = false};
				case GST_H265_SEI_PIC_STRUCT_BOTTOM_FIELD:
				case GST_H265_SEI_PIC_STRUCT_BOTTOM_PAIRED_PREVIOUS_TOP:
				case GST_H265_SEI_PIC_STRUCT_BOTTOM_PAIRED_NEXT_TOP:
					return Field_Picture_Info{.field_picture_ = true, .bottom_field_ = true};
				default:
					return Field_Picture_Info{};
			}
		}

		[[nodiscard]] Field_Picture_Info field_picture_info_for_slice(const GstH265SliceHdr &slice,
																	  std::optional<guint8>  pic_struct) {
			if (pic_struct) { return field_picture_info_from_pic_struct(*pic_struct).value_or(Field_Picture_Info{}); }
			const auto *pps = slice.pps;
			const auto *sps = pps == nullptr ? nullptr : pps->sps;
			if (sps != nullptr && sps->vui_params.field_seq_flag != 0U) {
				return Field_Picture_Info{.field_picture_ = true, .bottom_field_ = false};
			}
			return {};
		}

		void free_sei_messages(GArray *messages) {
			if (messages == nullptr) { return; }
			for (guint index = 0; index < messages->len; ++index) {
				gst_h265_sei_free(&g_array_index(messages, GstH265SEIMessage, index));
			}
			g_array_unref(messages);
		}

		[[nodiscard]] std::vector<std::uint8_t> rbsp_from_nalu_payload(const GstH265NalUnit &nalu) {
			std::vector<std::uint8_t> rbsp;
			if (nalu.size <= nalu.header_bytes) { return rbsp; }
			const auto *payload = nalu.data + nalu.offset + nalu.header_bytes;
			const auto  size    = static_cast<std::size_t>(nalu.size - nalu.header_bytes);
			rbsp.reserve(size);
			std::uint8_t zero_run = 0;
			for (std::size_t index = 0; index < size; ++index) {
				const auto byte = payload[index];
				if (zero_run >= 2U && byte == 0x03U) {
					zero_run = 0;
					continue;
				}
				rbsp.push_back(byte);
				zero_run = byte == 0U ? static_cast<std::uint8_t>(std::min<std::int32_t>(zero_run + 1, 2)) : 0U;
			}
			return rbsp;
		}

		struct Minimal_Sps_Capabilities {
			std::uint32_t width_            = 0;
			std::uint32_t height_           = 0;
			std::uint8_t  chroma_format_    = 1;
			std::uint8_t  luma_bit_depth_   = 8;
			std::uint8_t  chroma_bit_depth_ = 8;
		};

		struct Minimal_Sps_Bitreader {
			std::span<const std::uint8_t> bytes_;
			std::size_t                   bit_ = 0;

			[[nodiscard]] std::uint32_t read_bits(std::size_t count) {
				if (count > 32U || bit_ + count > bytes_.size() * 8U) {
					throw std::runtime_error{"truncated H.265 SPS"};
				}
				std::uint32_t value = 0;
				for (std::size_t index = 0; index < count; ++index) {
					value = (value << 1U) | ((bytes_[bit_ / 8U] >> (7U - static_cast<std::uint32_t>(bit_ % 8U))) & 1U);
					++bit_;
				}
				return value;
			}

			[[nodiscard]] std::uint32_t ue() {
				std::size_t leading_zero_bits = 0;
				while (read_bits(1U) == 0U) {
					if (++leading_zero_bits > 31U) { throw std::runtime_error{"invalid H.265 Exp-Golomb value"}; }
				}
				return ((1U << leading_zero_bits) - 1U) + (leading_zero_bits == 0U ? 0U : read_bits(leading_zero_bits));
			}

			void skip_bits(std::size_t count) {
				while (count > 0U) {
					const auto step = std::min<std::size_t>(count, 32U);
					(void) read_bits(step);
					count -= step;
				}
			}
		};

		[[nodiscard]] std::optional<Minimal_Sps_Capabilities> minimal_sps_capabilities(const GstH265NalUnit &nalu) {
			try {
				const auto            rbsp = rbsp_from_nalu_payload(nalu);
				Minimal_Sps_Bitreader bits{rbsp};
				bits.skip_bits(4U);
				const auto max_sub_layers_minus1 = bits.read_bits(3U);
				if (max_sub_layers_minus1 > 6U) { return std::nullopt; }
				bits.skip_bits(1U);

				bits.skip_bits(96U);
				std::array<bool, 7> sub_layer_profile_present{};
				std::array<bool, 7> sub_layer_level_present{};
				for (std::uint32_t layer = 0; layer < max_sub_layers_minus1; ++layer) {
					sub_layer_profile_present[layer] = bits.read_bits(1U) != 0U;
					sub_layer_level_present[layer]   = bits.read_bits(1U) != 0U;
				}
				if (max_sub_layers_minus1 > 0U) { bits.skip_bits(2U * (8U - max_sub_layers_minus1)); }
				for (std::uint32_t layer = 0; layer < max_sub_layers_minus1; ++layer) {
					if (sub_layer_profile_present[layer]) { bits.skip_bits(88U); }
					if (sub_layer_level_present[layer]) { bits.skip_bits(8U); }
				}

				(void) bits.ue();
				const auto chroma_format = bits.ue();
				if (chroma_format > 3U) { return std::nullopt; }
				if (chroma_format == 3U) { bits.skip_bits(1U); }
				const auto width  = bits.ue();
				const auto height = bits.ue();
				if (width == 0U || height == 0U) { return std::nullopt; }
				if (bits.read_bits(1U) != 0U) {
					(void) bits.ue();
					(void) bits.ue();
					(void) bits.ue();
					(void) bits.ue();
				}
				const auto luma_minus8   = bits.ue();
				const auto chroma_minus8 = bits.ue();
				if (luma_minus8 > 247U || chroma_minus8 > 247U) { return std::nullopt; }
				return Minimal_Sps_Capabilities{
						.width_            = width,
						.height_           = height,
						.chroma_format_    = static_cast<std::uint8_t>(chroma_format),
						.luma_bit_depth_   = static_cast<std::uint8_t>(luma_minus8 + 8U),
						.chroma_bit_depth_ = static_cast<std::uint8_t>(chroma_minus8 + 8U),
				};
			} catch (const std::exception &) { return std::nullopt; }
		}

		[[nodiscard]] std::optional<guint8> raw_pic_struct_from_sei(GstH265Parser *parser, const GstH265NalUnit &nalu) {
			const auto *sps = parser == nullptr ? nullptr : parser->last_sps;
			if (sps == nullptr || sps->vui_params.frame_field_info_present_flag == 0U) {
				if (std::getenv("H265_TRACE_DPB") != nullptr) {
					std::cerr << "h265_sei_raw_skip offset=" << nalu.offset << " has_sps=" << (sps != nullptr);
					if (sps != nullptr) {
						std::cerr << " vui_parsed=" << static_cast<std::uint32_t>(sps->vui_params.parsed)
								  << " field_seq=" << static_cast<std::uint32_t>(sps->vui_params.field_seq_flag)
								  << " frame_field_info="
								  << static_cast<std::uint32_t>(sps->vui_params.frame_field_info_present_flag);
					}
					std::cerr << "\n";
				}
				return std::nullopt;
			}

			const auto  rbsp   = rbsp_from_nalu_payload(nalu);
			std::size_t offset = 0;
			while (offset + 1U < rbsp.size()) {
				std::uint32_t payload_type = 0;
				while (offset < rbsp.size() && rbsp[offset] == 0xFFU) {
					payload_type += 255U;
					++offset;
				}
				if (offset >= rbsp.size()) { break; }
				payload_type += rbsp[offset++];

				std::uint32_t payload_size = 0;
				while (offset < rbsp.size() && rbsp[offset] == 0xFFU) {
					payload_size += 255U;
					++offset;
				}
				if (offset >= rbsp.size()) { break; }
				payload_size += rbsp[offset++];

				if (payload_size > rbsp.size() - offset) { break; }
				if (payload_type == GST_H265_SEI_PIC_TIMING && payload_size > 0U) {
					return static_cast<guint8>(rbsp[offset] >> 4U);
				}
				offset += payload_size;
			}
			return std::nullopt;
		}

		struct Sei_Info {
			std::optional<color::Hdr10_Plus> hdr10_plus_;
			std::optional<guint8>                              pic_struct_;
			std::optional<av1::Mastering_Display_Color_Volume> mastering_display_;
			std::optional<av1::Content_Light_Level>             content_light_;
		};

		[[nodiscard]] Sei_Info sei_info(GstH265Parser *parser, GstH265NalUnit &nalu) {
			Sei_Info info;
			info.pic_struct_ = raw_pic_struct_from_sei(parser, nalu);
			GArray    *messages = nullptr;
			const auto result   = gst_h265_parser_parse_sei(parser, &nalu, &messages);
			if (result != GST_H265_PARSER_OK) {
				if (std::getenv("H265_TRACE_DPB") != nullptr) {
					std::cerr << "h265_sei offset=" << nalu.offset << " type=" << static_cast<std::uint32_t>(nalu.type)
							  << " result=" << parser_result_message(result) << "\n";
				}
				free_sei_messages(messages);
				return info;
			}
			if (messages != nullptr) {
				for (guint index = 0; index < messages->len; ++index) {
					const auto &message = g_array_index(messages, GstH265SEIMessage, index);
					if (message.payloadType == GST_H265_SEI_REGISTERED_USER_DATA) {
						const auto                           &data = message.payload.registered_user_data;
						constexpr std::array<std::uint8_t, 5> signature{0, 0x3c, 0, 1, 4};
						if (data.country_code == 0xb5 && data.size > signature.size() &&
							std::equal(signature.begin(), signature.end(), data.data)) {
							auto parsed = color::parse_app4_registered_t35(
											data.country_code, std::span<const std::uint8_t>(data.data, data.size));
							if (!parsed && std::getenv("H265_TRACE_DPB") != nullptr)
								std::cerr << "h265_sei HDR10+ parse: " << parsed.error() << "\n";
							info.hdr10_plus_ = parsed ? parsed->preview : color::Hdr10_Plus{};
						}
					} else if (message.payloadType == GST_H265_SEI_PIC_TIMING) {
						info.pic_struct_ = message.payload.pic_timing.pic_struct;
					} else if (message.payloadType == GST_H265_SEI_MASTERING_DISPLAY_COLOUR_VOLUME) {
						const auto      &mastering        = message.payload.mastering_display_colour_volume;
						constexpr double coordinate_scale = 1.0 / 50000.0;
						constexpr double luminance_scale  = 1.0 / 10000.0;
						// HEVC carries the mastering primaries in G, B, R order.
						info.mastering_display_ = av1::Mastering_Display_Color_Volume{
								.red_ = {.x_ = mastering.display_primaries_x[2] * coordinate_scale,
										 .y_ = mastering.display_primaries_y[2] * coordinate_scale},
								.green_ = {.x_ = mastering.display_primaries_x[0] * coordinate_scale,
										   .y_ = mastering.display_primaries_y[0] * coordinate_scale},
								.blue_ = {.x_ = mastering.display_primaries_x[1] * coordinate_scale,
										  .y_ = mastering.display_primaries_y[1] * coordinate_scale},
								.white_ = {.x_ = mastering.white_point_x * coordinate_scale,
										   .y_ = mastering.white_point_y * coordinate_scale},
								.min_luminance_ = mastering.min_display_mastering_luminance * luminance_scale,
								.max_luminance_ = mastering.max_display_mastering_luminance * luminance_scale,
						};
					} else if (message.payloadType == GST_H265_SEI_CONTENT_LIGHT_LEVEL) {
						const auto &light = message.payload.content_light_level;
						info.content_light_ = av1::Content_Light_Level{
								.max_content_light_level_       = light.max_content_light_level,
								.max_frame_average_light_level_ = light.max_pic_average_light_level,
						};
					}
				}
			}
			if (std::getenv("H265_TRACE_DPB") != nullptr) {
				std::cerr << "h265_sei offset=" << nalu.offset << " type=" << static_cast<std::uint32_t>(nalu.type)
						  << " messages=" << (messages == nullptr ? 0U : messages->len);
				if (info.pic_struct_) {
					std::cerr << " pic_struct=" << static_cast<std::uint32_t>(*info.pic_struct_);
				}
				if (info.mastering_display_) {
					std::cerr << " mastering_max=" << info.mastering_display_->max_luminance_;
				}
				if (info.content_light_) {
					std::cerr << " max_cll=" << info.content_light_->max_content_light_level_;
				}
				std::cerr << "\n";
			}
			free_sei_messages(messages);
			return info;
		}

		[[nodiscard]] bool profile_compatible(const GstH265ProfileTierLevel &ptl, std::uint8_t profile_idc) {
			return ptl.profile_idc == profile_idc || (profile_idc < std::size(ptl.profile_compatibility_flag) &&
													  ptl.profile_compatibility_flag[profile_idc] != 0U);
		}

		[[nodiscard]] H265_Profile to_internal_profile(const GstH265SPS &sps) {
			const auto &ptl       = sps.profile_tier_level;
			const auto  bit_depth = static_cast<std::uint8_t>(sps.bit_depth_luma_minus8 + 8U);
			// Vulkan Video has no StdVideoH265ProfileIdc value for the High
			// Throughput profiles (IDC 5).  It is only valid to use the Range
			// Extensions profile when the bitstream explicitly declares itself
			// compatible with IDC 4 as well.
			if (profile_compatible(ptl, GST_H265_PROFILE_IDC_HIGH_THROUGHPUT) &&
				!profile_compatible(ptl, GST_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSION)) {
				return H265_Profile::UNSUPPORTED_EXTENSION;
			}
			if (profile_compatible(ptl, GST_H265_PROFILE_IDC_SCREEN_CONTENT_CODING) || sps.sps_scc_extension_flag) {
				if (sps.chroma_format_idc == 3U) {
					return bit_depth > 8U ? H265_Profile::SCC_MAIN_444_10 : H265_Profile::SCC_MAIN_444;
				}
				return bit_depth > 8U ? H265_Profile::SCC_MAIN_10 : H265_Profile::SCC_MAIN;
			}
			if (profile_compatible(ptl, GST_H265_PROFILE_IDC_MAIN_STILL_PICTURE)) {
				return H265_Profile::MAIN_STILL_PICTURE;
			}
			if (sps.chroma_format_idc == 2U) {
				return bit_depth > 10U ? H265_Profile::MAIN_422_12 : H265_Profile::MAIN_422_10;
			}
			if (sps.chroma_format_idc == 3U) {
				if (bit_depth > 10U) { return H265_Profile::MAIN_444_12; }
				return bit_depth > 8U ? H265_Profile::MAIN_444_10 : H265_Profile::MAIN_444;
			}
			if (bit_depth > 10U) { return H265_Profile::MAIN_12; }
			if (bit_depth > 8U) { return H265_Profile::MAIN_10; }
			if (profile_compatible(ptl, GST_H265_PROFILE_IDC_MAIN) ||
				profile_compatible(ptl, GST_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSION) ||
				profile_compatible(ptl, GST_H265_PROFILE_IDC_HIGH_THROUGHPUT) || sps.sps_range_extension_flag != 0U) {
				return H265_Profile::MAIN;
			}
			if (sps.chroma_format_idc == 1U) { return H265_Profile::MAIN; }
			return H265_Profile::UNSUPPORTED_EXTENSION;
		}

		[[nodiscard]] bool is_range_extension_profile(const GstH265SPS &sps) {
			const auto &ptl = sps.profile_tier_level;
			return profile_compatible(ptl, GST_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSION) ||
				   profile_compatible(ptl, GST_H265_PROFILE_IDC_HIGH_THROUGHPUT);
		}

		[[nodiscard]] Chroma_Sample_Position h265_chroma_sample_position(guint8 value) {
			switch (value) {
				case 0U:
					return Chroma_Sample_Position::COSITED;
				case 1U:
					return Chroma_Sample_Position::MIDPOINT;
				default:
					return Chroma_Sample_Position::UNKNOWN;
			}
		}

		[[nodiscard]] Color_Encoding color_from_sps(const GstH265SPS &sps) {
			Color_Encoding color;
			color.bit_depth_              = static_cast<std::uint8_t>(sps.bit_depth_luma_minus8 + 8U);
			color.range_                  = Color_Range::LIMITED;
			color.matrix_                 = Matrix_Coefficients::UNSPECIFIED;
			color.primaries_              = Color_Primaries::UNSPECIFIED;
			color.transfer_               = Transfer_Function::UNSPECIFIED;
			color.chroma_sample_position_ = Chroma_Sample_Position::UNKNOWN;
			switch (static_cast<H265_Chroma_Format>(sps.chroma_format_idc)) {
				case H265_Chroma_Format::MONOCHROME:
					color.monochrome_    = true;
					color.subsampling_x_ = true;
					color.subsampling_y_ = true;
					break;
				case H265_Chroma_Format::YUV420:
					color.subsampling_x_ = true;
					color.subsampling_y_ = true;
					break;
				case H265_Chroma_Format::YUV422:
					color.subsampling_x_ = true;
					color.subsampling_y_ = false;
					break;
				case H265_Chroma_Format::YUV444:
					color.subsampling_x_ = false;
					color.subsampling_y_ = false;
					break;
			}

			if (sps.vui_parameters_present_flag != 0U && sps.vui_params.parsed) {
				const auto &vui = sps.vui_params;
				color.range_    = vui.video_full_range_flag != 0U ? Color_Range::FULL : Color_Range::LIMITED;
				if (vui.colour_description_present_flag != 0U) {
					color.primaries_ = av1::color_primaries_from_h273_code(vui.colour_primaries);
					color.transfer_  = av1::transfer_function_from_h273_code(vui.transfer_characteristics);
					color.matrix_    = av1::matrix_coefficients_from_h273_code(vui.matrix_coefficients);
				}
				if (vui.chroma_loc_info_present_flag != 0U) {
					color.chroma_sample_position_ = h265_chroma_sample_position(vui.chroma_sample_loc_type_top_field);
				}
			}
			return color;
		}

		[[nodiscard]] H265_Sequence_Info sequence_from_sps(const GstH265SPS &sps) {
			const auto profile = to_internal_profile(sps);
			const auto color   = color_from_sps(sps);
			return H265_Sequence_Info{
					.profile_          = profile,
					.max_width_        = static_cast<std::uint32_t>(std::max(sps.width, 0)),
					.max_height_       = static_cast<std::uint32_t>(std::max(sps.height, 0)),
					.bit_depth_        = color.bit_depth_,
					.chroma_bit_depth_ = static_cast<std::uint8_t>(sps.bit_depth_chroma_minus8 + 8U),
					.chroma_format_    = static_cast<H265_Chroma_Format>(sps.chroma_format_idc),
					.range_extension_  = sps.sps_range_extension_flag != 0U || is_range_extension_profile(sps),
					.screen_content_ = sps.sps_scc_extension_flag != 0U || profile == H265_Profile::SCC_MAIN ||
									   profile == H265_Profile::SCC_MAIN_10 || profile == H265_Profile::SCC_MAIN_444 ||
									   profile == H265_Profile::SCC_MAIN_444_10,
					.high_precision_offsets_ = sps.sps_extension_params.high_precision_offsets_enabled_flag != 0U,
					.extended_precision_processing_ = sps.sps_extension_params.extended_precision_processing_flag != 0U,
					.color_                         = color,
			};
		}

		[[nodiscard]] Result<GstH265NalUnit>
		identify_nal(GstH265Parser *parser, std::span<const std::uint8_t> bitstream, std::uint32_t offset) {
			GstH265NalUnit nalu{};
			auto result = gst_h265_parser_identify_nalu(parser, bitstream.data(), offset, bitstream.size(), &nalu);
			if (result == GST_H265_PARSER_NO_NAL_END) {
				// Annex B permits the final NAL unit to end at EOF. The checked GStreamer helper reports
				// NO_NAL_END because there is no following start code; unchecked identification supplies
				// the bitstream boundary explicitly and is valid only for this terminal case.
				result = gst_h265_parser_identify_nalu_unchecked(parser, bitstream.data(), offset, bitstream.size(),
																 &nalu);
			}
			if (result == GST_H265_PARSER_NO_NAL) {
				return std::unexpected(h265_error(offset, static_cast<std::uint64_t>(offset) * 8U,
												  "H.265 elementary stream must use Annex B start codes"));
			}
			if (result != GST_H265_PARSER_OK) {
				return std::unexpected(
						h265_error(offset, static_cast<std::uint64_t>(offset) * 8U, parser_result_message(result)));
			}
			return nalu;
		}

		struct Indexed_Nal {
			GstH265NalUnit nalu_{};
			std::uint64_t logical_start_code_ = 0;
			std::uint64_t logical_nal_ = 0;
			std::uint64_t logical_end_ = 0;
			bool configuration_ = false;
		};

		[[nodiscard]] Result<std::vector<Indexed_Nal>> identify_annex_b_nals(
				GstH265Parser *parser, std::span<const std::uint8_t> bitstream) {
			std::vector<Indexed_Nal> result;
			for (std::uint32_t offset = 0; offset < bitstream.size();) {
				auto identified = identify_nal(parser, bitstream, offset);
				if (!identified) { return std::unexpected(identified.error()); }
				const auto &nalu = *identified;
				if (nalu.size == 0U) {
					return std::unexpected(h265_error(nalu.offset, static_cast<std::uint64_t>(nalu.offset) * 8U,
							"H.265 parser returned an empty NAL unit"));
				}
				result.push_back({nalu, nalu.sc_offset, nalu.offset,
						static_cast<std::uint64_t>(nalu.offset) + nalu.size, false});
				const auto next = static_cast<std::uint64_t>(nalu.offset) + nalu.size;
				if (next <= offset || next > std::numeric_limits<std::uint32_t>::max()) {
					return std::unexpected(h265_error(offset, static_cast<std::uint64_t>(offset) * 8U,
							"H.265 parser did not advance"));
				}
				offset = static_cast<std::uint32_t>(next);
			}
			return result;
		}

		[[nodiscard]] Result<void> append_hevc_nals(GstH265Parser *parser,
				std::span<const std::uint8_t> bytes, std::uint8_t length_size, std::uint64_t logical_begin,
				std::vector<Indexed_Nal> &output, bool configuration = false) {
			for (std::uint32_t offset = 0; offset < bytes.size();) {
				GstH265NalUnit nalu{};
				const auto status = gst_h265_parser_identify_nalu_hevc(
						parser, bytes.data(), offset, bytes.size(), length_size, &nalu);
				if (status != GST_H265_PARSER_OK && status != GST_H265_PARSER_NO_NAL_END) {
					return std::unexpected(h265_error(logical_begin + offset, (logical_begin + offset) * 8U,
							parser_result_message(status)));
				}
				if (nalu.size == 0U) {
					return std::unexpected(h265_error(logical_begin + offset, (logical_begin + offset) * 8U,
							"HEVC sample contains an empty NAL unit"));
				}
				output.push_back({nalu, logical_begin + nalu.sc_offset, logical_begin + nalu.offset,
						logical_begin + nalu.offset + nalu.size, configuration});
				const auto next = static_cast<std::uint64_t>(nalu.offset) + nalu.size;
				if (next <= offset || next > bytes.size()) {
					return std::unexpected(h265_error(logical_begin + offset, (logical_begin + offset) * 8U,
							"HEVC sample NAL parser did not advance"));
				}
				offset = static_cast<std::uint32_t>(next);
			}
			return {};
		}

		void mark_unsupported(Parsed_Bitstream &parsed, std::uint64_t offset, std::string message) {
			if (parsed.requires_unsupported_) { return; }
			parsed.requires_unsupported_ = true;
			parsed.unsupported_offset_   = offset;
			parsed.unsupported_message_  = std::move(message);
		}

		[[nodiscard]] H265_Frame_Decode_Metadata make_frame_metadata(std::uint64_t begin, std::uint64_t end,
																	 H265_Slice_Type slice_type, bool irap) {
			H265_Frame_Decode_Metadata metadata;
			metadata.bitstream_          = Encoded_Byte_Interval{.begin_ = begin, .size_ = end - begin};
			metadata.primary_slice_type_ = slice_type;
			metadata.irap_               = irap;
			return metadata;
		}

		void copy_visible_rect_from_sps(H265_Frame_Decode_Metadata &metadata, const GstH265SPS *sps) {
			if (sps == nullptr) { return; }
			metadata.visible_x_      = static_cast<std::uint32_t>(std::max(sps->crop_rect_x, 0));
			metadata.visible_y_      = static_cast<std::uint32_t>(std::max(sps->crop_rect_y, 0));
			metadata.visible_width_  = static_cast<std::uint32_t>(std::max(sps->crop_rect_width, 0));
			metadata.visible_height_ = static_cast<std::uint32_t>(std::max(sps->crop_rect_height, 0));
			if (metadata.visible_width_ == 0U || metadata.visible_height_ == 0U) {
				metadata.visible_x_      = 0;
				metadata.visible_y_      = 0;
				metadata.visible_width_  = static_cast<std::uint32_t>(std::max(sps->width, 0));
				metadata.visible_height_ = static_cast<std::uint32_t>(std::max(sps->height, 0));
			}
		}

		struct Decoded_Frame_State {
			std::uint32_t frame_index_       = 0;
			std::int32_t  pic_order_cnt_     = 0;
			std::uint16_t pic_order_cnt_lsb_ = 0;
			bool          non_reference_     = false;
			bool          field_picture_     = false;
			bool          bottom_field_      = false;
		};

		struct Reference_Target {
			std::int32_t  pic_order_cnt_     = 0;
			std::uint16_t pic_order_cnt_lsb_ = 0;
			bool          exact_poc_         = true;
			bool          long_term_         = false;
			std::uint32_t va_flags_          = 0;
			bool          used_by_current_   = true;
		};

		struct Decoded_Reference_Lookup {
			std::optional<Decoded_Frame_State> frame_;
			bool                               ambiguous_lsb_ = false;
		};

		struct Virtual_Dpb_Entry {
			bool          allocated_     = false;
			bool          unavailable_   = false;
			bool          short_ref_     = false;
			bool          long_ref_      = false;
			bool          output_        = false;
			bool          field_picture_ = false;
			bool          bottom_field_  = false;
			std::uint32_t frame_index_   = kInvalid_H265_Frame_Index;
			std::int32_t  pic_order_cnt_ = 0;
			std::uint16_t pic_order_lsb_ = 0;
			std::uint32_t va_flags_      = 0;
		};

		struct Dpb_Reference_Lookup {
			std::optional<std::uint8_t> slot_;
			bool                        ambiguous_lsb_ = false;
		};

		struct Virtual_Dpb_State {
			std::array<Virtual_Dpb_Entry, kVirtual_Dpb_Slot_Count> slots_{};
			std::optional<std::uint32_t>                           open_frame_;
			std::optional<std::uint8_t>                            open_slot_;

			[[nodiscard]] std::optional<std::uint8_t> first_free_slot() const {
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					if (!slots_[slot].allocated_) { return slot; }
				}
				return std::nullopt;
			}

			[[nodiscard]] std::optional<std::uint8_t> first_reclaimable_slot() const {
				std::optional<std::uint8_t> oldest_slot;
				std::int32_t                oldest_poc = std::numeric_limits<std::int32_t>::max();
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					const auto &entry = slots_[slot];
					if (!entry.allocated_) { continue; }
					if (open_slot_ && slot == *open_slot_) { continue; }
					if (entry.short_ref_ || entry.long_ref_ || entry.output_) { continue; }
					if (!oldest_slot || entry.pic_order_cnt_ < oldest_poc) {
						oldest_slot = slot;
						oldest_poc  = entry.pic_order_cnt_;
					}
				}
				return oldest_slot;
			}

			[[nodiscard]] std::optional<std::uint8_t> first_reclaimable_reference_slot() const {
				std::optional<std::uint8_t> oldest_slot;
				std::int32_t                oldest_poc = std::numeric_limits<std::int32_t>::max();
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					const auto &entry = slots_[slot];
					if (!entry.allocated_) { continue; }
					if (open_slot_ && slot == *open_slot_) { continue; }
					if (!(entry.short_ref_ || entry.long_ref_) || entry.output_ || entry.unavailable_) { continue; }
					if (!oldest_slot || entry.pic_order_cnt_ < oldest_poc) {
						oldest_slot = slot;
						oldest_poc  = entry.pic_order_cnt_;
					}
				}
				return oldest_slot;
			}

			[[nodiscard]] std::optional<std::uint8_t> allocate_current(std::uint32_t                frame_index,
																	   const Frame_Reference_State &state) {
				auto slot = first_free_slot();
				if (!slot) { slot = first_reclaimable_slot(); }
				if (!slot) { slot = first_reclaimable_reference_slot(); }
				if (!slot) { return std::nullopt; }
				auto &entry          = slots_[*slot];
				entry                = {};
				entry.allocated_     = true;
				entry.frame_index_   = frame_index;
				entry.pic_order_cnt_ = state.pic_order_cnt_;
				entry.pic_order_lsb_ = state.pic_order_cnt_lsb_;
				entry.output_        = state.output_;
				entry.short_ref_     = !state.non_reference_;
				entry.field_picture_ = state.field_picture_;
				entry.bottom_field_  = state.bottom_field_;
				open_frame_          = frame_index;
				open_slot_           = *slot;
				return slot;
			}

			[[nodiscard]] std::optional<std::uint8_t> allocate_decoded_reference(const Decoded_Frame_State &frame) {
				auto slot = first_free_slot();
				if (!slot) { slot = first_reclaimable_slot(); }
				if (!slot) { slot = first_reclaimable_reference_slot(); }
				if (!slot) { return std::nullopt; }
				auto &entry          = slots_[*slot];
				entry                = {};
				entry.allocated_     = true;
				entry.frame_index_   = frame.frame_index_;
				entry.pic_order_cnt_ = frame.pic_order_cnt_;
				entry.pic_order_lsb_ = frame.pic_order_cnt_lsb_;
				entry.field_picture_ = frame.field_picture_;
				entry.bottom_field_  = frame.bottom_field_;
				return slot;
			}

			void trace_state(const char *event, std::uint32_t frame_index, std::int32_t current_poc) const {
				if (std::getenv("H265_TRACE_DPB") == nullptr) { return; }
				std::cerr << "h265_dpb event=" << event << " frame=" << frame_index << " poc=" << current_poc;
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					const auto &entry = slots_[slot];
					if (!entry.allocated_) { continue; }
					std::cerr << " [" << static_cast<std::uint32_t>(slot) << ":poc=" << entry.pic_order_cnt_
							  << ",f=" << entry.frame_index_ << ",s=" << entry.short_ref_ << ",l=" << entry.long_ref_
							  << ",o=" << entry.output_ << ",u=" << entry.unavailable_
							  << ",field=" << entry.field_picture_ << ",bottom=" << entry.bottom_field_ << "]";
				}
				std::cerr << "\n";
			}

			void begin_current_reference_marking(std::uint8_t current_slot) {
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					if (!slots_[slot].allocated_ || slot == current_slot) { continue; }
					slots_[slot].short_ref_ = false;
					slots_[slot].long_ref_  = false;
					slots_[slot].va_flags_  = 0;
				}
			}

			[[nodiscard]] Dpb_Reference_Lookup find_reference_slot(const Reference_Target &target) const {
				std::optional<std::uint8_t> lsb_match;
				std::optional<std::uint8_t> unavailable_match;
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					const auto &entry = slots_[slot];
					if (!entry.allocated_) { continue; }
					if (open_slot_ && slot == *open_slot_) { continue; }
					if (target.exact_poc_) {
						if (entry.pic_order_cnt_ != target.pic_order_cnt_) { continue; }
						if (!entry.unavailable_ && entry.frame_index_ != kInvalid_H265_Frame_Index) {
							return Dpb_Reference_Lookup{.slot_ = slot};
						}
						if (!unavailable_match) { unavailable_match = slot; }
						continue;
					}
					if (entry.pic_order_lsb_ != target.pic_order_cnt_lsb_) { continue; }
					if (entry.unavailable_ || entry.frame_index_ == kInvalid_H265_Frame_Index) {
						if (!unavailable_match) { unavailable_match = slot; }
						continue;
					}
					if (lsb_match) { return Dpb_Reference_Lookup{.slot_ = std::nullopt, .ambiguous_lsb_ = true}; }
					lsb_match = slot;
				}
				return Dpb_Reference_Lookup{.slot_ = lsb_match ? lsb_match : unavailable_match};
			}

			[[nodiscard]] std::optional<std::uint8_t> allocate_unavailable(const Reference_Target &target) {
				auto slot = first_free_slot();
				if (!slot) { slot = first_reclaimable_slot(); }
				if (!slot) { return std::nullopt; }
				auto &entry          = slots_[*slot];
				entry                = {};
				entry.allocated_     = true;
				entry.unavailable_   = true;
				entry.frame_index_   = kInvalid_H265_Frame_Index;
				entry.pic_order_cnt_ = target.pic_order_cnt_;
				entry.pic_order_lsb_ = target.pic_order_cnt_lsb_;
				return slot;
			}

			void mark_reference(std::uint8_t slot, const Reference_Target &target) {
				auto &entry      = slots_[slot];
				entry.short_ref_ = !target.long_term_;
				entry.long_ref_  = target.long_term_;
				entry.va_flags_ |= target.va_flags_;
			}

			void clear_output(std::uint8_t slot) {
				auto &entry   = slots_[slot];
				entry.output_ = false;
				if (!entry.short_ref_ && !entry.long_ref_) { entry = {}; }
			}

			void bump_output_if_needed(std::uint8_t max_dec_pic_buffering) {
				if (first_free_slot()) { return; }
				std::uint8_t dpb_count = 0;
				for (const auto &entry: slots_) {
					if (!entry.allocated_) { continue; }
					if (entry.output_ || entry.short_ref_ || entry.long_ref_ || entry.unavailable_) { ++dpb_count; }
				}
				if (dpb_count < max_dec_pic_buffering) { return; }

				std::optional<std::uint8_t> min_output_slot;
				std::int32_t                min_output_poc = std::numeric_limits<std::int32_t>::max();
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					const auto &entry = slots_[slot];
					if (!entry.allocated_ || !entry.output_ || entry.short_ref_ || entry.long_ref_ ||
						entry.unavailable_) {
						continue;
					}
					if (!min_output_slot || entry.pic_order_cnt_ < min_output_poc) {
						min_output_slot = slot;
						min_output_poc  = entry.pic_order_cnt_;
					}
				}
				if (min_output_slot) { clear_output(*min_output_slot); }
			}

			void output_one_if_needed(std::uint8_t max_num_reorder_pics, bool no_output_of_prior_pics) {
				if (no_output_of_prior_pics) {
					for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
						const auto &entry = slots_[slot];
						if (!entry.allocated_ || !entry.output_) { continue; }
						if (open_slot_ && slot == *open_slot_) { continue; }
						clear_output(slot);
					}
				}
				std::uint8_t                output_count = 0;
				std::optional<std::uint8_t> min_output_slot;
				std::int32_t                min_output_poc = std::numeric_limits<std::int32_t>::max();
				for (std::uint8_t slot = 0; slot < slots_.size(); ++slot) {
					const auto &entry = slots_[slot];
					if (!entry.allocated_ || !entry.output_) { continue; }
					++output_count;
					if (!min_output_slot || entry.pic_order_cnt_ < min_output_poc) {
						min_output_slot = slot;
						min_output_poc  = entry.pic_order_cnt_;
					}
				}
				if (output_count > max_num_reorder_pics && min_output_slot) { clear_output(*min_output_slot); }
			}

			void release_unmarked() {
				for (auto &entry: slots_) {
					if (!entry.allocated_) { continue; }
					if (entry.short_ref_ || entry.long_ref_ || entry.output_) { continue; }
					entry = {};
				}
			}

			void reset_for_recovery() {
				for (auto &entry: slots_) { entry = {}; }
				open_frame_ = std::nullopt;
				open_slot_  = std::nullopt;
			}
		};

		[[nodiscard]] std::uint8_t h265_sps_max_sublayer_index(const GstH265SPS *sps) {
			if (sps == nullptr) { return 0; }
			return static_cast<std::uint8_t>(
					std::min<std::uint8_t>(sps->max_sub_layers_minus1, GST_H265_MAX_SUB_LAYERS - 1U));
		}

		[[nodiscard]] std::uint8_t max_num_reorder_pics(const GstH265SPS *sps) {
			if (sps == nullptr) { return 0U; }
			return static_cast<std::uint8_t>(
					std::min<std::uint32_t>(sps->max_num_reorder_pics[h265_sps_max_sublayer_index(sps)], 16U));
		}

		[[nodiscard]] std::uint8_t max_dec_pic_buffering(const GstH265SPS *sps) {
			if (sps == nullptr) { return 1U; }
			const auto sublayer = h265_sps_max_sublayer_index(sps);
			return static_cast<std::uint8_t>(
					std::min<std::uint32_t>(static_cast<std::uint32_t>(sps->max_dec_pic_buffering_minus1[sublayer]) + 1U,
									   static_cast<std::uint32_t>(kVirtual_Dpb_Slot_Count)));
		}

		[[nodiscard]] std::int32_t calculate_poc(const GstH265SliceHdr &slice, guint8 nal_type,
												 std::int32_t prev_tid0_pic) {
			const auto *pps = slice.pps;
			const auto *sps = pps == nullptr ? nullptr : pps->sps;
			if (sps == nullptr) { return static_cast<std::int32_t>(slice.pic_order_cnt_lsb); }
			const std::int32_t max_poc_lsb =
					static_cast<std::int32_t>(1U << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4U));
			const std::int32_t prev_poc_lsb = prev_tid0_pic % max_poc_lsb;
			const std::int32_t prev_poc_msb = prev_tid0_pic - prev_poc_lsb;
			const std::int32_t poc_lsb      = slice.pic_order_cnt_lsb;

			std::int32_t poc_msb = 0;
			if (is_idr_type(nal_type) || is_bla_type(nal_type)) {
				poc_msb = 0;
			} else if (poc_lsb < prev_poc_lsb && prev_poc_lsb - poc_lsb >= max_poc_lsb / 2) {
				poc_msb = prev_poc_msb + max_poc_lsb;
			} else if (poc_lsb > prev_poc_lsb && poc_lsb - prev_poc_lsb > max_poc_lsb / 2) {
				poc_msb = prev_poc_msb - max_poc_lsb;
			} else {
				poc_msb = prev_poc_msb;
			}
			return poc_msb + poc_lsb;
		}

		[[nodiscard]] const GstH265ShortTermRefPicSet *active_short_term_rps(const GstH265SliceHdr &slice) {
			const auto *pps = slice.pps;
			const auto *sps = pps == nullptr ? nullptr : pps->sps;
			if (sps == nullptr) { return nullptr; }
			if (slice.short_term_ref_pic_set_sps_flag != 0U) {
				if (slice.short_term_ref_pic_set_idx >= sps->num_short_term_ref_pic_sets) { return nullptr; }
				return &sps->short_term_ref_pic_set[slice.short_term_ref_pic_set_idx];
			}
			return &slice.short_term_ref_pic_sets;
		}

		void append_short_term_targets(std::vector<Reference_Target> &before, std::vector<Reference_Target> &after,
									   std::vector<Reference_Target> &all, const GstH265SliceHdr &slice,
									   std::int32_t current_poc) {
			const auto *rps = active_short_term_rps(slice);
			if (rps == nullptr) { return; }
			for (std::uint8_t index = 0; index < rps->NumNegativePics; ++index) {
				const bool       used = rps->UsedByCurrPicS0[index] != 0U;
				Reference_Target target{
						.pic_order_cnt_     = current_poc + rps->DeltaPocS0[index],
						.pic_order_cnt_lsb_ = static_cast<std::uint16_t>(current_poc + rps->DeltaPocS0[index]),
						.exact_poc_         = true,
						.long_term_         = false,
						.va_flags_          = used ? kReference_St_Curr_Before : 0U,
						.used_by_current_   = used,
				};
				all.push_back(target);
				if (used) { before.push_back(target); }
			}
			for (std::uint8_t index = 0; index < rps->NumPositivePics; ++index) {
				const bool       used = rps->UsedByCurrPicS1[index] != 0U;
				Reference_Target target{
						.pic_order_cnt_     = current_poc + rps->DeltaPocS1[index],
						.pic_order_cnt_lsb_ = static_cast<std::uint16_t>(current_poc + rps->DeltaPocS1[index]),
						.exact_poc_         = true,
						.long_term_         = false,
						.va_flags_          = used ? kReference_St_Curr_After : 0U,
						.used_by_current_   = used,
				};
				all.push_back(target);
				if (used) { after.push_back(target); }
			}
		}

		void append_long_term_targets(std::vector<Reference_Target> &lt, std::vector<Reference_Target> &all,
									  const GstH265SliceHdr &slice, std::int32_t current_poc) {
			const auto *pps = slice.pps;
			const auto *sps = pps == nullptr ? nullptr : pps->sps;
			if (sps == nullptr || sps->long_term_ref_pics_present_flag == 0U) { return; }
			const std::int32_t max_poc_lsb =
					static_cast<std::int32_t>(1U << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4U));
			std::uint32_t      previous_delta_poc_msb = 0;
			const std::uint8_t count                  = static_cast<std::uint8_t>(
					std::min<std::uint32_t>(slice.num_long_term_sps + slice.num_long_term_pics, 16U));
			for (std::uint8_t index = 0; index < count; ++index) {
				std::uint32_t lsb  = 0;
				bool          used = false;
				if (index < slice.num_long_term_sps) {
					const auto lt_index = slice.lt_idx_sps[index];
					if (lt_index >= sps->num_long_term_ref_pics_sps) { continue; }
					lsb  = sps->lt_ref_pic_poc_lsb_sps[lt_index];
					used = sps->used_by_curr_pic_lt_sps_flag[lt_index] != 0U;
				} else {
					lsb  = slice.poc_lsb_lt[index];
					used = slice.used_by_curr_pic_lt_flag[index] != 0U;
				}

				Reference_Target target{
						.pic_order_cnt_     = 0,
						.pic_order_cnt_lsb_ = static_cast<std::uint16_t>(lsb),
						.exact_poc_         = false,
						.long_term_         = true,
						.va_flags_          = used ? kReference_Lt_Curr : 0U,
						.used_by_current_   = used,
				};
				if (slice.delta_poc_msb_present_flag[index] != 0U) {
					previous_delta_poc_msb += slice.delta_poc_msb_cycle_lt[index];
					target.pic_order_cnt_ = current_poc -
											static_cast<std::int32_t>(previous_delta_poc_msb) * max_poc_lsb -
											(current_poc & (max_poc_lsb - 1)) + static_cast<std::int32_t>(lsb);
					target.exact_poc_     = true;
				}
				all.push_back(target);
				if (used) { lt.push_back(target); }
			}
		}

		struct Reference_Sets {
			std::vector<Reference_Target> before_;
			std::vector<Reference_Target> after_;
			std::vector<Reference_Target> lt_;
			std::vector<Reference_Target> all_;
		};

		[[nodiscard]] Reference_Sets derive_reference_sets(const GstH265SliceHdr &slice, std::int32_t current_poc) {
			Reference_Sets sets;
			append_short_term_targets(sets.before_, sets.after_, sets.all_, slice, current_poc);
			append_long_term_targets(sets.lt_, sets.all_, slice, current_poc);
			return sets;
		}

		[[nodiscard]] std::uint8_t active_ref_count(const GstH265SliceHdr &slice, std::uint8_t list) {
			if (list == 0U) {
				return static_cast<std::uint8_t>(std::min<std::uint32_t>(slice.num_ref_idx_l0_active_minus1 + 1U, 15U));
			}
			if (slice.type != GST_H265_B_SLICE) { return 0U; }
			return static_cast<std::uint8_t>(std::min<std::uint32_t>(slice.num_ref_idx_l1_active_minus1 + 1U, 15U));
		}

		[[nodiscard]] std::vector<Reference_Target> build_ref_pic_list(const GstH265SliceHdr &slice,
																	   const Reference_Sets &sets, std::uint8_t list) {
			const auto                    active = active_ref_count(slice, list);
			std::vector<Reference_Target> result;
			if (active == 0U) { return result; }
			const std::array<const std::vector<Reference_Target> *, 3> order =
					list == 0U ? std::array{&sets.before_, &sets.after_, &sets.lt_}
							   : std::array{&sets.after_, &sets.before_, &sets.lt_};
			std::vector<Reference_Target> temp;
			for (const auto *group: order) {
				for (const auto &target: *group) { temp.push_back(target); }
			}
			if (temp.empty()) { return result; }
			const auto &mod      = slice.ref_pic_list_modification;
			const bool  modified = list == 0U ? mod.ref_pic_list_modification_flag_l0 != 0U
											  : mod.ref_pic_list_modification_flag_l1 != 0U;
			const auto *entries  = list == 0U ? mod.list_entry_l0 : mod.list_entry_l1;
			if (modified) {
				for (std::uint8_t index = 0; index < active; ++index) {
					if (entries[index] < temp.size()) { result.push_back(temp[entries[index]]); }
				}
			} else {
				while (result.size() < active) {
					const auto before_size = result.size();
					for (const auto &target: temp) {
						if (result.size() >= active) { break; }
						result.push_back(target);
					}
					if (result.size() == before_size) { break; }
				}
			}
			return result;
		}

		[[nodiscard]] Decoded_Reference_Lookup find_decoded_reference(std::span<const Decoded_Frame_State> decoded,
																	  const Reference_Target              &target) {
			for (auto it = decoded.rbegin(); it != decoded.rend(); ++it) {
				if (target.exact_poc_) {
					if (it->pic_order_cnt_ == target.pic_order_cnt_) { return Decoded_Reference_Lookup{.frame_ = *it}; }
				} else if (it->pic_order_cnt_lsb_ == target.pic_order_cnt_lsb_) {
					return Decoded_Reference_Lookup{.frame_ = *it};
				}
			}
			return {};
		}

		void derive_reference_state(Parsed_Bitstream &parsed, Frame_Reference_State &state,
									std::span<const Decoded_Frame_State> decoded_frames, const GstH265NalUnit &nalu,
									const GstH265SliceHdr &slice, Virtual_Dpb_State &dpb) {
			const auto sets = derive_reference_sets(slice, state.pic_order_cnt_);
			state.ref_pic_set_st_curr_before_.fill(0xFFU);
			state.ref_pic_set_st_curr_after_.fill(0xFFU);
			state.ref_pic_set_lt_curr_.fill(0xFFU);
			if (state.current_dpb_slot_ != 0xFFU) { dpb.begin_current_reference_marking(state.current_dpb_slot_); }

			auto mark_target = [&](const Reference_Target &target) {
				auto dpb_reference = dpb.find_reference_slot(target);
				if (dpb_reference.ambiguous_lsb_) {
					mark_unsupported(parsed, nalu.offset,
									 "H.265 long-term reference POC LSB is ambiguous in indexed decode order");
					return std::optional<std::uint8_t>{};
				}
				std::optional<std::uint8_t> slot = dpb_reference.slot_;
				if (!slot) {
					auto reference = find_decoded_reference(decoded_frames, target);
					if (reference.ambiguous_lsb_) {
						mark_unsupported(parsed, nalu.offset,
										 "H.265 long-term reference POC LSB is ambiguous in indexed decode order");
						return std::optional<std::uint8_t>{};
					}
					if (reference.frame_) {
						for (std::uint8_t candidate = 0; candidate < dpb.slots_.size(); ++candidate) {
							const auto &entry = dpb.slots_[candidate];
							if (entry.allocated_ && !entry.unavailable_ &&
								entry.frame_index_ == reference.frame_->frame_index_) {
								slot = candidate;
								break;
							}
						}
						if (!slot) { slot = dpb.allocate_decoded_reference(*reference.frame_); }
					}
				}
				if (!slot) { slot = dpb.allocate_unavailable(target); }
				if (!slot) {
					mark_unsupported(parsed, nalu.offset, "H.265 frame uses more than 15 active references");
					return std::optional<std::uint8_t>{};
				}
				dpb.mark_reference(*slot, target);
				return slot;
			};

			for (const auto &target: sets.all_) { (void) mark_target(target); }
			const auto preserve_set_order = [&](const std::vector<Reference_Target> &targets, auto &slots) {
				for (std::size_t index = 0; index < targets.size() && index < slots.size(); ++index) {
					if (const auto slot = mark_target(targets[index])) { slots[index] = *slot; }
				}
			};
			preserve_set_order(sets.before_, state.ref_pic_set_st_curr_before_);
			preserve_set_order(sets.after_, state.ref_pic_set_st_curr_after_);
			preserve_set_order(sets.lt_, state.ref_pic_set_lt_curr_);

			for (std::uint8_t list = 0; list < 2U; ++list) {
				const auto rpl = build_ref_pic_list(slice, sets, list);
				for (std::uint8_t index = 0; index < rpl.size() && index < 15U; ++index) {
					auto slot = mark_target(rpl[index]);
					if (slot) { state.ref_pic_list_slot_[static_cast<std::size_t>(list) * 15U + index] = *slot; }
				}
			}
		}

		void finalize_reference_state(Parsed_Bitstream &parsed, H265_Frame_Decode_Metadata &metadata,
									  Frame_Reference_State &state, Virtual_Dpb_State &dpb, std::uint32_t frame_index,
									  std::uint64_t byte_offset) {
			metadata.reference_frame_index_.fill(kInvalid_H265_Frame_Index);
			metadata.used_reference_mask_        = 0;
			metadata.active_reference_mask_      = 0;
			metadata.unavailable_reference_mask_ = 0;
			state.reference_long_term_.fill(false);
			state.reference_unavailable_.fill(false);
			state.reference_field_picture_.fill(false);
			state.reference_bottom_field_.fill(false);
			state.reference_pic_order_cnt_.fill(0);
			state.reference_va_flags_.fill(0);
			state.reference_dpb_slot_.fill(0xFFU);

			std::array<std::uint8_t, kVirtual_Dpb_Slot_Count> dpb_to_va{};
			dpb_to_va.fill(0xFFU);
			std::uint8_t reference_count = 0;
			for (std::uint8_t dpb_slot = 0; dpb_slot < dpb.slots_.size(); ++dpb_slot) {
				const auto &entry = dpb.slots_[dpb_slot];
				if (!entry.allocated_ || !(entry.short_ref_ || entry.long_ref_)) { continue; }
				if (!entry.unavailable_ && entry.frame_index_ == frame_index) { continue; }
				if (reference_count >= metadata.reference_frame_index_.size()) {
					mark_unsupported(parsed, byte_offset, "H.265 frame uses more than 15 active references");
					break;
				}
				dpb_to_va[dpb_slot] = reference_count;
				metadata.used_reference_mask_ |= static_cast<std::uint16_t>(1U << reference_count);
				if (entry.va_flags_ != 0U) {
					metadata.active_reference_mask_ |= static_cast<std::uint16_t>(1U << reference_count);
				}
				if (entry.unavailable_) {
					metadata.unavailable_reference_mask_ |= static_cast<std::uint16_t>(1U << reference_count);
				} else {
					metadata.reference_frame_index_[reference_count] = entry.frame_index_;
				}
				state.reference_unavailable_[reference_count]   = entry.unavailable_;
				state.reference_long_term_[reference_count]     = entry.long_ref_;
				state.reference_field_picture_[reference_count] = entry.field_picture_;
				state.reference_bottom_field_[reference_count]  = entry.bottom_field_;
				state.reference_pic_order_cnt_[reference_count] = entry.pic_order_cnt_;
				state.reference_va_flags_[reference_count]      = entry.va_flags_;
				state.reference_dpb_slot_[reference_count]      = dpb_slot;
				++reference_count;
			}

			const auto remap_dpb_slots = [&](auto &slots) {
			for (auto &slot: slots) {
				if (slot == 0xFFU) { continue; }
				if (slot >= dpb_to_va.size() || dpb_to_va[slot] == 0xFFU) {
					slot = 0xFFU;
					continue;
				}
				slot = dpb_to_va[slot];
			}
			};
			remap_dpb_slots(state.ref_pic_list_slot_);
			remap_dpb_slots(state.ref_pic_set_st_curr_before_);
			remap_dpb_slots(state.ref_pic_set_st_curr_after_);
			remap_dpb_slots(state.ref_pic_set_lt_curr_);
			dpb.release_unmarked();
			dpb.open_frame_ = std::nullopt;
			dpb.open_slot_  = std::nullopt;
		}
	} // namespace

	namespace {
	Result<Parsed_Bitstream> parse_indexed_nals(Parser_Ptr parser, std::span<const Indexed_Nal> indexed_nals) {
		Parsed_Bitstream                         parsed;
		std::uint64_t                            pending_access_unit_start = std::numeric_limits<std::uint64_t>::max();
		std::vector<Decoded_Frame_State>         decoded_frames;
		Virtual_Dpb_State                        dpb;
		std::int32_t                             prev_tid0_pic = 0;
		std::optional<guint8>                    pending_pic_struct;
		std::optional<color::Hdr10_Plus>          pending_hdr10_plus;
		color::Dynamic_HDR pending_dynamic_hdr;
		std::uint32_t hdr_sequence_id = 0;
		std::optional<GstH265SliceHdr>           current_independent_slice;
		bool                                     new_bitstream_or_eos       = true;
		bool                                     no_output_before_recovery  = false;
		bool                                     dropping_open_picture      = false;
		bool                                     non_vcl_after_open_picture = false;
		bool                                     have_open_picture          = false;
		std::uint64_t                            parameter_generation       = 0;
		std::uint64_t                            snapshotted_generation     = std::numeric_limits<std::uint64_t>::max();
		std::shared_ptr<H265_Parameter_Snapshot> parameter_snapshot;

		const auto finalize_open_frame = [&]() {
			if (!dpb.open_frame_) { return; }
			const auto frame_index = *dpb.open_frame_;
			if (frame_index >= parsed.frame_metadata_.size() || frame_index >= parsed.frame_states_.size()) {
				dpb.open_frame_ = std::nullopt;
				dpb.open_slot_  = std::nullopt;
				return;
			}
			finalize_reference_state(parsed, parsed.frame_metadata_[frame_index], parsed.frame_states_[frame_index],
									 dpb, frame_index, parsed.frame_metadata_[frame_index].bitstream_.begin_);
		};

		for (const auto &indexed_nal: indexed_nals) {
			auto nalu = indexed_nal.nalu_;
			const auto logical_start_code = indexed_nal.logical_start_code_;
			const auto logical_nal = indexed_nal.logical_nal_;
			const auto logical_end = indexed_nal.logical_end_;
			if (nalu.size == 0U) {
				return std::unexpected(h265_error(logical_nal, logical_nal * 8U,
											  "H.265 parser returned an empty NAL unit"));
			}
			if (nalu.layer_id != 0U) {
				mark_unsupported(parsed, logical_nal,
								 "H.265 multilayer/MV-HEVC bitstreams are not exposed by this API");
			}
			parsed.nals_.push_back(Nal_Unit{
					.type_              = to_internal_type(nalu.type),
					.layer_id_          = nalu.layer_id,
					.temporal_id_       = static_cast<std::uint8_t>(nalu.temporal_id_plus1 - 1U),
					.start_code_offset_ = logical_start_code,
					.nal_offset_        = logical_nal,
					.payload_offset_    = logical_nal + nalu.header_bytes,
					.end_offset_        = logical_end,
			});
			// GStreamer's base-layer parser cannot safely consume parameter sets from
			// enhancement layers.  The API cannot represent those layers anyway, so
			// retain the NAL in the index for diagnostics and classify the complete
			// bitstream as unsupported without letting it corrupt the layer-0 state.
			if (nalu.layer_id != 0U) { continue; }

			if (!indexed_nal.configuration_ &&
				pending_access_unit_start == std::numeric_limits<std::uint64_t>::max() && !is_vcl_type(nalu.type)) {
				pending_access_unit_start = logical_start_code;
			}

			GstH265ParserResult result = GST_H265_PARSER_OK;
			if (nalu.type == GST_H265_NAL_SPS) {
				GstH265SPS sps{};
				result = gst_h265_parser_parse_sps(parser.get(), &nalu, &sps, TRUE);
				if (result == GST_H265_PARSER_OK) { result = gst_h265_parser_update_sps(parser.get(), &sps); }
				if (result != GST_H265_PARSER_OK) {
					const auto capabilities = minimal_sps_capabilities(nalu);
					if (capabilities &&
						(capabilities->luma_bit_depth_ > 12U || capabilities->chroma_bit_depth_ > 12U)) {
						parsed.has_sps_                    = true;
						parsed.sequence_.profile_          = H265_Profile::UNSUPPORTED_EXTENSION;
						parsed.sequence_.max_width_        = capabilities->width_;
						parsed.sequence_.max_height_       = capabilities->height_;
						parsed.sequence_.bit_depth_        = capabilities->luma_bit_depth_;
						parsed.sequence_.chroma_bit_depth_ = capabilities->chroma_bit_depth_;
						parsed.sequence_.chroma_format_ = static_cast<H265_Chroma_Format>(capabilities->chroma_format_);
						parsed.sequence_.range_extension_   = true;
						parsed.sequence_.color_.bit_depth_  = capabilities->luma_bit_depth_;
						parsed.sequence_.color_.monochrome_ = capabilities->chroma_format_ == 0U;
						return parsed;
					}
					return std::unexpected(h265_error(logical_nal, logical_nal * 8U,
													  parser_result_message(result)));
				}
				if (parser->last_sps != nullptr) {
					const auto prior_max_width  = parsed.has_sps_ ? parsed.sequence_.max_width_ : 0U;
					const auto prior_max_height = parsed.has_sps_ ? parsed.sequence_.max_height_ : 0U;
					parsed.has_sps_             = true;
					parsed.sequence_            = sequence_from_sps(*parser->last_sps);
					parsed.sequence_.max_width_ = std::max(parsed.sequence_.max_width_, prior_max_width);
					parsed.sequence_.max_height_ = std::max(parsed.sequence_.max_height_, prior_max_height);
				}
				++parameter_generation;
			} else if (nalu.type == GST_H265_NAL_VPS || nalu.type == GST_H265_NAL_PPS) {
				result = gst_h265_parser_parse_nal(parser.get(), &nalu);
				if (result != GST_H265_PARSER_OK) {
					return std::unexpected(h265_error(logical_nal, logical_nal * 8U,
														  parser_result_message(result)));
				}
				++parameter_generation;
			}
			if (nalu.type == GST_H265_NAL_PREFIX_SEI || nalu.type == GST_H265_NAL_SUFFIX_SEI) {
				const auto dynamic = color::parse_dynamic_hdr_sei({nalu.data + nalu.offset + nalu.header_bytes,
						static_cast<std::size_t>(nalu.size - nalu.header_bytes)});
				if (nalu.type == GST_H265_NAL_PREFIX_SEI) color::merge_dynamic_hdr(pending_dynamic_hdr, dynamic);
				else if (!dropping_open_picture && have_open_picture && !parsed.frame_metadata_.empty())
					color::merge_dynamic_hdr(parsed.frame_metadata_.back().dynamic_hdr_, dynamic);
				const auto sei = sei_info(parser.get(), nalu);
				if (sei.hdr10_plus_) {
					if (nalu.type == GST_H265_NAL_SUFFIX_SEI && dropping_open_picture) {
						// Metadata on a discarded RASL picture must not alter its predecessor.
					} else if (nalu.type == GST_H265_NAL_SUFFIX_SEI && have_open_picture && !parsed.frame_metadata_.empty())
						parsed.frame_metadata_.back().hdr10_plus_ = sei.hdr10_plus_;
					else pending_hdr10_plus = sei.hdr10_plus_;
				}
				if (sei.mastering_display_) {
					parsed.sequence_.color_.mastering_display_ = sei.mastering_display_;
				}
				if (sei.content_light_) { parsed.sequence_.color_.content_light_ = sei.content_light_; }
				if (sei.pic_struct_) {
					if (nalu.type == GST_H265_NAL_PREFIX_SEI) {
						pending_pic_struct = *sei.pic_struct_;
					} else if (have_open_picture && !parsed.frame_states_.empty()) {
						const auto frame_index = static_cast<std::uint32_t>(parsed.frame_states_.size() - 1U);
						const auto info =
								field_picture_info_from_pic_struct(*sei.pic_struct_).value_or(Field_Picture_Info{});
						auto &state          = parsed.frame_states_.back();
						state.field_picture_ = info.field_picture_;
						state.bottom_field_  = info.bottom_field_;
						if (state.current_dpb_slot_ < dpb.slots_.size()) {
							auto &entry          = dpb.slots_[state.current_dpb_slot_];
							entry.field_picture_ = info.field_picture_;
							entry.bottom_field_  = info.bottom_field_;
						}
						for (auto &decoded: decoded_frames) {
							if (decoded.frame_index_ != frame_index) { continue; }
							decoded.field_picture_ = info.field_picture_;
							decoded.bottom_field_  = info.bottom_field_;
							break;
						}
					} else {
						pending_pic_struct = *sei.pic_struct_;
					}
					++parameter_generation;
				}
			}
			if (nalu.type == GST_H265_NAL_EOS || nalu.type == GST_H265_NAL_EOB) { new_bitstream_or_eos = true; }

			if (is_vcl_type(nalu.type)) {
				GstH265SliceHdr slice{};
				// Resolve the parameter-set links and derived PPS values first.
				result = gst_h265_parser_parse_slice_hdr(parser.get(), &nalu, &slice);
				if (parse_independent_scc_slice(parser.get(), &nalu, &slice) == GST_H265_PARSER_OK) {
					result = GST_H265_PARSER_OK;
				}
				if (result != GST_H265_PARSER_OK) {
					return std::unexpected(h265_error(logical_nal, logical_nal * 8U,
													  parser_result_message(result)));
				}
				if (slice.pps == nullptr || slice.pps->sps == nullptr) {
					return std::unexpected(h265_error(logical_nal, logical_nal * 8U,
													  "H.265 slice header did not resolve PPS/SPS"));
				}

				const auto pic_order_cnt = calculate_poc(slice, nalu.type, prev_tid0_pic);
				const bool starts_new_frame =
						!have_open_picture || slice.first_slice_segment_in_pic_flag != 0U || non_vcl_after_open_picture;
				if (starts_new_frame) { current_independent_slice = std::nullopt; }
				inherit_dependent_slice_header(slice, current_independent_slice);
				if (slice.dependent_slice_segment_flag == 0U) { current_independent_slice = slice; }
				if (starts_new_frame) {
					finalize_open_frame();
					dropping_open_picture = false;
					if (is_idr_type(nalu.type) || is_bla_type(nalu.type)) {
						++hdr_sequence_id;
						no_output_before_recovery = false;
						dpb.reset_for_recovery();
						decoded_frames.clear();
					} else if (is_cra_type(nalu.type)) {
						no_output_before_recovery = new_bitstream_or_eos;
						if (no_output_before_recovery) {
							++hdr_sequence_id;
							// A CRA that starts a new bitstream/CVS cannot reference pictures
							// preceding the recovery point.  Keeping those pictures in the
							// virtual DPB makes repeated POC values resolve to stale images
							// after the leading RASL pictures have been discarded.
							dpb.reset_for_recovery();
							decoded_frames.clear();
						}
					}
					new_bitstream_or_eos = false;
					if (no_output_before_recovery && GST_H265_IS_NAL_TYPE_RASL(nalu.type)) {
						pending_access_unit_start = std::numeric_limits<std::uint64_t>::max();
						pending_pic_struct        = std::nullopt;
						pending_hdr10_plus        = std::nullopt;
						pending_dynamic_hdr = {};
						have_open_picture         = true;
						dropping_open_picture     = true;
						continue;
					}
					const auto *active_vps = slice.pps->sps->vps;
					const bool active_parameter_ids_changed = parameter_snapshot &&
							(parameter_snapshot->pps_.id != slice.pps->id ||
							 parameter_snapshot->sps_.id != slice.pps->sps->id ||
							 (parameter_snapshot->sps_.vps == nullptr) != (active_vps == nullptr) ||
							 (active_vps != nullptr && parameter_snapshot->vps_.id != active_vps->id));
					if (snapshotted_generation != parameter_generation || !parameter_snapshot ||
							active_parameter_ids_changed) {
						parameter_snapshot     = snapshot_parameter_sets(slice);
						snapshotted_generation = parameter_generation;
					}
					slice.pps = &parameter_snapshot->pps_;
					parsed.decode_frames_.push_back(Parsed_Decode_Frame{
							.parameters_ = parameter_snapshot,
							.slices_     = {Parsed_Decode_Slice{
									.nalu_ = nalu, .logical_offset_ = logical_nal, .header_ = slice}},
					});
					const auto begin = pending_access_unit_start == std::numeric_limits<std::uint64_t>::max()
											   ? logical_start_code
											   : pending_access_unit_start;
					auto metadata    = make_frame_metadata(begin, logical_end,
														   to_internal_slice_type(slice.type), is_irap_type(nalu.type));
					copy_visible_rect_from_sps(metadata, slice.pps->sps);
					metadata.hdr10_plus_ = std::exchange(pending_hdr10_plus, std::nullopt);
					metadata.dynamic_hdr_ = std::exchange(pending_dynamic_hdr, {});
					metadata.dynamic_hdr_.sequence_id = hdr_sequence_id;

					Frame_Reference_State state;
					state.pic_order_cnt_     = pic_order_cnt;
					state.pic_order_cnt_lsb_ = slice.pic_order_cnt_lsb;
					state.non_reference_     = is_non_reference_vcl(nalu.type);
					state.output_            = slice.pps->output_flag_present_flag == 0U || slice.pic_output_flag != 0U;
					const auto field_info    = field_picture_info_for_slice(slice, pending_pic_struct);
					state.field_picture_     = field_info.field_picture_;
					state.bottom_field_      = field_info.bottom_field_;
					pending_pic_struct       = std::nullopt;
					const auto frame_index   = static_cast<std::uint32_t>(parsed.frame_metadata_.size());
					if (!is_idr_type(nalu.type)) { dpb.bump_output_if_needed(max_dec_pic_buffering(slice.pps->sps)); }
					dpb.trace_state("before_alloc", frame_index, state.pic_order_cnt_);
					auto current_slot = dpb.allocate_current(frame_index, state);
					if (!current_slot) {
						dpb.trace_state("alloc_failed", frame_index, state.pic_order_cnt_);
						mark_unsupported(parsed, logical_nal, "H.265 decoded-picture buffer is full");
					} else {
						state.current_dpb_slot_ = *current_slot;
						dpb.trace_state("after_alloc", frame_index, state.pic_order_cnt_);
						dpb.output_one_if_needed(max_num_reorder_pics(slice.pps->sps),
												 slice.no_output_of_prior_pics_flag != 0U);
						dpb.trace_state("after_output", frame_index, state.pic_order_cnt_);
					}
					derive_reference_state(parsed, state, decoded_frames, nalu, slice, dpb);
					dpb.trace_state("after_derive_refs", frame_index, state.pic_order_cnt_);

					parsed.frame_metadata_.push_back(metadata);
					parsed.frame_states_.push_back(state);
					if ((nalu.temporal_id_plus1 - 1U) == 0U && !state.non_reference_ && !is_rasl_or_radl(nalu.type)) {
						prev_tid0_pic = state.pic_order_cnt_;
					}
					decoded_frames.push_back(Decoded_Frame_State{
							.frame_index_       = frame_index,
							.pic_order_cnt_     = state.pic_order_cnt_,
							.pic_order_cnt_lsb_ = state.pic_order_cnt_lsb_,
							.non_reference_     = state.non_reference_,
							.field_picture_     = state.field_picture_,
							.bottom_field_      = state.bottom_field_,
					});
					pending_access_unit_start = std::numeric_limits<std::uint64_t>::max();
					have_open_picture         = true;
				} else if (!parsed.frame_metadata_.empty()) {
					if (dropping_open_picture) {
						continue;
					}
					auto &frame = parsed.frame_metadata_.back();
					auto &state = parsed.frame_states_.back();
					if (parsed.decode_frames_.empty()) {
						return std::unexpected(h265_error(logical_nal, logical_nal * 8U,
														  "H.265 slice lowering state has no open decoded picture"));
					}
					slice.pps = &parsed.decode_frames_.back().parameters_->pps_;
					parsed.decode_frames_.back().slices_.push_back(
							Parsed_Decode_Slice{.nalu_ = nalu, .logical_offset_ = logical_nal, .header_ = slice});
					frame.bitstream_.size_ = logical_end - frame.bitstream_.begin_;
					frame.primary_slice_type_ = std::min(frame.primary_slice_type_, to_internal_slice_type(slice.type));
					frame.irap_               = frame.irap_ || is_irap_type(nalu.type);
					if (frame.visible_width_ == 0U || frame.visible_height_ == 0U) {
						copy_visible_rect_from_sps(frame, slice.pps->sps);
					}
					auto reference_history = std::span<const Decoded_Frame_State>(decoded_frames);
					if (!reference_history.empty() &&
						reference_history.back().frame_index_ + 1U == parsed.frame_metadata_.size()) {
						reference_history = reference_history.first(reference_history.size() - 1U);
					}
					derive_reference_state(parsed, state, reference_history, nalu, slice, dpb);
				}
				non_vcl_after_open_picture = false;
			} else if (have_open_picture) {
				non_vcl_after_open_picture = true;
			}

		}

		finalize_open_frame();
		if (!parsed.has_sps_) { return std::unexpected(h265_error(0, 0, "H.265 bitstream does not contain an SPS")); }
		if (parsed.sequence_.max_width_ == 0U || parsed.sequence_.max_height_ == 0U) {
			return std::unexpected(h265_error(0, 0, "H.265 SPS did not expose valid coded dimensions"));
		}
		if (parsed.frame_metadata_.empty()) {
			mark_unsupported(parsed, 0, "H.265 bitstream does not contain decodable VCL frames");
		}
		return parsed;
	}
	} // namespace

	Result<Parsed_Bitstream> Annex_B_Parser::parse(std::span<const std::uint8_t> bitstream) const {
		if (bitstream.size() > std::numeric_limits<std::uint32_t>::max()) {
			return std::unexpected(h265_error(0, 0, "H.265 bitstream is too large for the parser backend"));
		}
		Parser_Ptr parser(gst_h265_parser_new());
		if (parser == nullptr) { return std::unexpected(h265_error(0, 0, "failed to allocate H.265 parser")); }
		auto nals = identify_annex_b_nals(parser.get(), bitstream);
		if (!nals) { return std::unexpected(nals.error()); }
		return parse_indexed_nals(std::move(parser), *nals);
	}

	Result<Parsed_Bitstream> Annex_B_Parser::parse_hevc_samples(
			std::span<const std::uint8_t> configuration,
			std::span<const av1::Borrowed_Bitstream_Extent> access_units,
			std::uint8_t nal_length_size) const {
		if (configuration.size() < 23U || configuration[0] != 1U || nal_length_size == 0U ||
			nal_length_size > 4U || ((configuration[21] & 3U) + 1U) != nal_length_size) {
			return std::unexpected(h265_error(0, 0, "invalid mmap-backed HEVC decoder configuration"));
		}
		Parser_Ptr parser(gst_h265_parser_new());
		if (parser == nullptr) { return std::unexpected(h265_error(0, 0, "failed to allocate H.265 parser")); }
		std::vector<Indexed_Nal> nals;
		std::size_t cursor = 23U;
		const auto array_count = configuration[22];
		for (std::uint8_t array = 0; array < array_count; ++array) {
			if (configuration.size() - cursor < 3U) {
				return std::unexpected(h265_error(cursor, cursor * 8U, "truncated hvcC NAL array"));
			}
			++cursor; // array completeness/reserved/NAL-unit type
			const auto count = static_cast<std::uint16_t>((configuration[cursor] << 8U) | configuration[cursor + 1U]);
			cursor += 2U;
			for (std::uint16_t i = 0; i < count; ++i) {
				if (configuration.size() - cursor < 2U) {
					return std::unexpected(h265_error(cursor, cursor * 8U, "truncated hvcC NAL length"));
				}
				const auto size = static_cast<std::uint16_t>((configuration[cursor] << 8U) | configuration[cursor + 1U]);
				if (size == 0U || configuration.size() - cursor - 2U < size) {
					return std::unexpected(h265_error(cursor, cursor * 8U, "hvcC NAL exceeds decoder configuration"));
				}
				auto appended = append_hevc_nals(parser.get(), configuration.subspan(cursor, size + 2U), 2U, 0U,
						nals, true);
				if (!appended) { return std::unexpected(appended.error()); }
				cursor += size + 2U;
			}
		}
		if (cursor != configuration.size()) {
			return std::unexpected(h265_error(cursor, cursor * 8U, "hvcC has trailing malformed bytes"));
		}
		std::uint64_t previous_end = 0;
		for (const auto &extent: access_units) {
			if (extent.bytes_.empty() || extent.logical_begin_ < previous_end ||
				extent.bytes_.size() > std::numeric_limits<std::uint32_t>::max()) {
				return std::unexpected(h265_error(extent.logical_begin_, extent.logical_begin_ * 8U,
						"HEVC mmap samples must be non-empty, ordered, and individually addressable"));
			}
			auto appended = append_hevc_nals(parser.get(), extent.bytes_, nal_length_size,
					extent.logical_begin_, nals);
			if (!appended) { return std::unexpected(appended.error()); }
			previous_end = extent.logical_begin_ + extent.bytes_.size();
		}
		if (access_units.empty()) { return std::unexpected(h265_error(0, 0, "HEVC sample index is empty")); }
		return parse_indexed_nals(std::move(parser), nals);
	}
} // namespace native_video::h265::decoder
