#include <cstdint>
#include "h265/vulkan_video_decode.hpp"

#include <xf86drm.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <list>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace native_video::h265::decoder {
	namespace {
		[[nodiscard]] Media_Error error(Media_Error_Code code, std::string message) {
			return Media_Error{.code_ = code, .message_ = std::move(message)};
		}

		[[nodiscard]] Media_Error vk_error(std::string_view operation, VkResult result) {
			return error(Media_Error_Code::VULKAN_INTEROP_FAILED,
					std::string{operation} + " failed with VkResult " + std::to_string(static_cast<std::int32_t>(result)));
		}

		[[nodiscard]] bool has_point(av1::Drm_Syncobj_Timeline_Point point) {
			return point.timeline_.drm_fd_ >= 0 || point.timeline_.handle_ != 0U || point.point_ != 0U;
		}

		[[nodiscard]] std::uint32_t memory_type(VkPhysicalDevice physical, std::uint32_t bits,
				VkMemoryPropertyFlags required) {
			VkPhysicalDeviceMemoryProperties properties{};
			vkGetPhysicalDeviceMemoryProperties(physical, &properties);
			for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
				if ((bits & (1U << index)) != 0U &&
						(properties.memoryTypes[index].propertyFlags & required) == required) {
					return index;
				}
			}
			return std::numeric_limits<std::uint32_t>::max();
		}

		[[nodiscard]] VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
			return alignment <= 1U ? value : ((value + alignment - 1U) / alignment) * alignment;
		}

		[[nodiscard]] StdVideoH265LevelIdc level_idc(std::uint8_t value) {
			switch (value) {
				case 30: return STD_VIDEO_H265_LEVEL_IDC_1_0;
				case 60: return STD_VIDEO_H265_LEVEL_IDC_2_0;
				case 63: return STD_VIDEO_H265_LEVEL_IDC_2_1;
				case 90: return STD_VIDEO_H265_LEVEL_IDC_3_0;
				case 93: return STD_VIDEO_H265_LEVEL_IDC_3_1;
				case 120: return STD_VIDEO_H265_LEVEL_IDC_4_0;
				case 123: return STD_VIDEO_H265_LEVEL_IDC_4_1;
				case 150: return STD_VIDEO_H265_LEVEL_IDC_5_0;
				case 153: return STD_VIDEO_H265_LEVEL_IDC_5_1;
				case 156: return STD_VIDEO_H265_LEVEL_IDC_5_2;
				case 180: return STD_VIDEO_H265_LEVEL_IDC_6_0;
				case 183: return STD_VIDEO_H265_LEVEL_IDC_6_1;
				case 186: return STD_VIDEO_H265_LEVEL_IDC_6_2;
				default: return STD_VIDEO_H265_LEVEL_IDC_INVALID;
			}
		}

		void lower_profile(const GstH265ProfileTierLevel &source, StdVideoH265ProfileTierLevel &target) {
			target.flags.general_tier_flag = source.tier_flag;
			target.flags.general_progressive_source_flag = source.progressive_source_flag;
			target.flags.general_interlaced_source_flag = source.interlaced_source_flag;
			target.flags.general_non_packed_constraint_flag = source.non_packed_constraint_flag;
			target.flags.general_frame_only_constraint_flag = source.frame_only_constraint_flag;
			target.general_profile_idc = static_cast<StdVideoH265ProfileIdc>(source.profile_idc);
			target.general_level_idc = level_idc(source.level_idc);
		}

		template<typename Latency>
		void lower_dpb(const guint8 *buffering, const guint8 *reorder, const Latency *latency,
				StdVideoH265DecPicBufMgr &target) {
			for (std::size_t index = 0; index < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; ++index) {
				target.max_dec_pic_buffering_minus1[index] = buffering[index];
				target.max_num_reorder_pics[index] = reorder[index];
				target.max_latency_increase_plus1[index] = latency[index];
			}
		}

		void copy_scaling_list(std::span<std::uint8_t> destination, const guint8 *source, std::uint8_t edge) {
			(void) edge;
			std::copy_n(source, destination.size(), destination.begin());
		}

		void lower_scaling(const GstH265ScalingList &source, StdVideoH265ScalingLists &target) {
			for (std::size_t index = 0; index < 6U; ++index) {
				copy_scaling_list(target.ScalingList4x4[index], source.scaling_lists_4x4[index], 4U);
				copy_scaling_list(target.ScalingList8x8[index], source.scaling_lists_8x8[index], 8U);
				copy_scaling_list(target.ScalingList16x16[index], source.scaling_lists_16x16[index], 8U);
				target.ScalingListDCCoef16x16[index] =
						static_cast<std::uint8_t>(source.scaling_list_dc_coef_minus8_16x16[index] + 8);
				if (index < 2U) {
					copy_scaling_list(target.ScalingList32x32[index], source.scaling_lists_32x32[index], 8U);
					target.ScalingListDCCoef32x32[index] =
							static_cast<std::uint8_t>(source.scaling_list_dc_coef_minus8_32x32[index] + 8);
				}
			}
		}

		void lower_rps(const GstH265ShortTermRefPicSet &source, StdVideoH265ShortTermRefPicSet &target) {
			// Store a semantically equivalent explicit RPS. Slice headers select
			// the set by index; prediction syntax is irrelevant after parameters
			// have been materialized into the session object.
			target.num_negative_pics = std::min<std::uint8_t>(source.NumNegativePics, STD_VIDEO_H265_MAX_DPB_SIZE);
			target.num_positive_pics = std::min<std::uint8_t>(source.NumPositivePics, STD_VIDEO_H265_MAX_DPB_SIZE);
			std::int32_t previous = 0;
			for (std::uint8_t index = 0; index < target.num_negative_pics; ++index) {
				target.delta_poc_s0_minus1[index] = static_cast<std::uint16_t>(
						std::max<std::int32_t>(0, previous - source.DeltaPocS0[index] - 1));
				previous = source.DeltaPocS0[index];
				if (source.UsedByCurrPicS0[index] != 0U) {
					target.used_by_curr_pic_s0_flag |= static_cast<std::uint16_t>(1U << index);
				}
			}
			previous = 0;
			for (std::uint8_t index = 0; index < target.num_positive_pics; ++index) {
				target.delta_poc_s1_minus1[index] = static_cast<std::uint16_t>(
						std::max<std::int32_t>(0, source.DeltaPocS1[index] - previous - 1));
				previous = source.DeltaPocS1[index];
				if (source.UsedByCurrPicS1[index] != 0U) {
					target.used_by_curr_pic_s1_flag |= static_cast<std::uint16_t>(1U << index);
				}
			}
		}

		struct Parameter_Bundle {
			StdVideoH265ProfileTierLevel vps_profile_{};
			StdVideoH265DecPicBufMgr vps_dpb_{};
			StdVideoH265VideoParameterSet vps_{};
			StdVideoH265ProfileTierLevel sps_profile_{};
			StdVideoH265DecPicBufMgr sps_dpb_{};
			StdVideoH265ScalingLists sps_scaling_{};
			std::array<StdVideoH265ShortTermRefPicSet, STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS> rps_{};
			StdVideoH265LongTermRefPicsSps long_term_{};
			StdVideoH265SequenceParameterSetVui vui_{};
			StdVideoH265PredictorPaletteEntries sps_palette_{};
			StdVideoH265SequenceParameterSet sps_{};
			StdVideoH265ScalingLists pps_scaling_{};
			StdVideoH265PredictorPaletteEntries pps_palette_{};
			StdVideoH265PictureParameterSet pps_{};

			explicit Parameter_Bundle(const H265_Parameter_Snapshot &source) { lower(source); }

			void lower(const H265_Parameter_Snapshot &source) {
				const auto &vps = source.vps_;
				const auto &sps = source.sps_;
				const auto &pps = source.pps_;
				const auto profile_compatible = [&](std::uint8_t profile_idc) {
					return sps.profile_tier_level.profile_idc == profile_idc ||
						   (profile_idc < std::size(sps.profile_tier_level.profile_compatibility_flag) &&
							sps.profile_tier_level.profile_compatibility_flag[profile_idc] != 0U);
				};
				// PPS extension bits cannot enable coding tools that the active SPS
				// profile does not permit.  In particular, PS_A_VIDYO_3 carries an
				// intentionally incomplete Main-profile PPS extension; forwarding its
				// parsed tail as RExt/SCC state changes the hardware reconstruction.
				const bool range_profile = profile_compatible(GST_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSION) ||
						profile_compatible(GST_H265_PROFILE_IDC_HIGH_THROUGHPUT) || sps.sps_range_extension_flag != 0U;
				const bool scc_profile = profile_compatible(GST_H265_PROFILE_IDC_SCREEN_CONTENT_CODING) ||
						sps.sps_scc_extension_flag != 0U;
				const bool has_sps_range_extension = (range_profile || scc_profile) && sps.sps_range_extension_flag;
				const bool has_sps_scc_extension = scc_profile && sps.sps_scc_extension_flag;
				const bool has_pps_range_extension = (range_profile || scc_profile) && pps.pps_range_extension_flag;
				const bool has_pps_scc_extension = scc_profile && pps.pps_scc_extension_flag;
				lower_profile(vps.profile_tier_level, vps_profile_);
				lower_dpb(vps.max_dec_pic_buffering_minus1, vps.max_num_reorder_pics,
						vps.max_latency_increase_plus1, vps_dpb_);
				vps_.flags.vps_temporal_id_nesting_flag = vps.temporal_id_nesting_flag;
				vps_.flags.vps_sub_layer_ordering_info_present_flag = vps.sub_layer_ordering_info_present_flag;
				vps_.flags.vps_timing_info_present_flag = vps.timing_info_present_flag;
				vps_.flags.vps_poc_proportional_to_timing_flag = vps.poc_proportional_to_timing_flag;
				vps_.vps_video_parameter_set_id = vps.id;
				vps_.vps_max_sub_layers_minus1 = vps.max_sub_layers_minus1;
				vps_.vps_num_units_in_tick = vps.num_units_in_tick;
				vps_.vps_time_scale = vps.time_scale;
				vps_.vps_num_ticks_poc_diff_one_minus1 = vps.num_ticks_poc_diff_one_minus1;
				vps_.pDecPicBufMgr = &vps_dpb_;
				vps_.pProfileTierLevel = &vps_profile_;

				lower_profile(sps.profile_tier_level, sps_profile_);
				lower_dpb(sps.max_dec_pic_buffering_minus1, sps.max_num_reorder_pics,
						sps.max_latency_increase_plus1, sps_dpb_);
				sps_.flags.sps_temporal_id_nesting_flag = sps.temporal_id_nesting_flag;
				sps_.flags.separate_colour_plane_flag = sps.separate_colour_plane_flag;
				sps_.flags.conformance_window_flag = sps.conformance_window_flag;
				sps_.flags.sps_sub_layer_ordering_info_present_flag = sps.sub_layer_ordering_info_present_flag;
				sps_.flags.scaling_list_enabled_flag = sps.scaling_list_enabled_flag;
				sps_.flags.sps_scaling_list_data_present_flag = sps.scaling_list_data_present_flag;
				sps_.flags.amp_enabled_flag = sps.amp_enabled_flag;
				sps_.flags.sample_adaptive_offset_enabled_flag = sps.sample_adaptive_offset_enabled_flag;
				sps_.flags.pcm_enabled_flag = sps.pcm_enabled_flag;
				sps_.flags.pcm_loop_filter_disabled_flag = sps.pcm_loop_filter_disabled_flag;
				sps_.flags.long_term_ref_pics_present_flag = sps.long_term_ref_pics_present_flag;
				sps_.flags.sps_temporal_mvp_enabled_flag = sps.temporal_mvp_enabled_flag;
				sps_.flags.strong_intra_smoothing_enabled_flag = sps.strong_intra_smoothing_enabled_flag;
				sps_.flags.vui_parameters_present_flag = sps.vui_parameters_present_flag;
				sps_.flags.sps_extension_present_flag = has_sps_range_extension || has_sps_scc_extension;
				sps_.flags.sps_range_extension_flag = has_sps_range_extension;
				const auto &range = sps.sps_extension_params;
				if (has_sps_range_extension) {
					sps_.flags.transform_skip_rotation_enabled_flag = range.transform_skip_rotation_enabled_flag;
					sps_.flags.transform_skip_context_enabled_flag = range.transform_skip_context_enabled_flag;
					sps_.flags.implicit_rdpcm_enabled_flag = range.implicit_rdpcm_enabled_flag;
					sps_.flags.explicit_rdpcm_enabled_flag = range.explicit_rdpcm_enabled_flag;
					sps_.flags.extended_precision_processing_flag = range.extended_precision_processing_flag;
					sps_.flags.intra_smoothing_disabled_flag = range.intra_smoothing_disabled_flag;
					sps_.flags.high_precision_offsets_enabled_flag = range.high_precision_offsets_enabled_flag;
					sps_.flags.persistent_rice_adaptation_enabled_flag = range.persistent_rice_adaptation_enabled_flag;
					sps_.flags.cabac_bypass_alignment_enabled_flag = range.cabac_bypass_alignment_enabled_flag;
				}
				const auto &scc = sps.sps_scc_extension_params;
				sps_.flags.sps_scc_extension_flag = has_sps_scc_extension;
				if (has_sps_scc_extension) {
					sps_.flags.sps_curr_pic_ref_enabled_flag = scc.sps_curr_pic_ref_enabled_flag;
					sps_.flags.palette_mode_enabled_flag = scc.palette_mode_enabled_flag;
					sps_.flags.sps_palette_predictor_initializers_present_flag =
							scc.sps_palette_predictor_initializers_present_flag;
					sps_.flags.intra_boundary_filtering_disabled_flag = scc.intra_boundary_filtering_disabled_flag;
				}
				sps_.chroma_format_idc = static_cast<StdVideoH265ChromaFormatIdc>(sps.chroma_format_idc);
				sps_.pic_width_in_luma_samples = sps.pic_width_in_luma_samples;
				sps_.pic_height_in_luma_samples = sps.pic_height_in_luma_samples;
				sps_.sps_video_parameter_set_id = sps.vps_id;
				sps_.sps_max_sub_layers_minus1 = sps.max_sub_layers_minus1;
				sps_.sps_seq_parameter_set_id = sps.id;
				sps_.bit_depth_luma_minus8 = sps.bit_depth_luma_minus8;
				sps_.bit_depth_chroma_minus8 = sps.bit_depth_chroma_minus8;
				sps_.log2_max_pic_order_cnt_lsb_minus4 = sps.log2_max_pic_order_cnt_lsb_minus4;
				sps_.log2_min_luma_coding_block_size_minus3 = sps.log2_min_luma_coding_block_size_minus3;
				sps_.log2_diff_max_min_luma_coding_block_size = sps.log2_diff_max_min_luma_coding_block_size;
				sps_.log2_min_luma_transform_block_size_minus2 = sps.log2_min_transform_block_size_minus2;
				sps_.log2_diff_max_min_luma_transform_block_size = sps.log2_diff_max_min_transform_block_size;
				sps_.max_transform_hierarchy_depth_inter = sps.max_transform_hierarchy_depth_inter;
				sps_.max_transform_hierarchy_depth_intra = sps.max_transform_hierarchy_depth_intra;
				sps_.num_short_term_ref_pic_sets = std::min<std::uint8_t>(
						sps.num_short_term_ref_pic_sets, STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS);
				sps_.num_long_term_ref_pics_sps = sps.num_long_term_ref_pics_sps;
				sps_.pcm_sample_bit_depth_luma_minus1 = sps.pcm_sample_bit_depth_luma_minus1;
				sps_.pcm_sample_bit_depth_chroma_minus1 = sps.pcm_sample_bit_depth_chroma_minus1;
				sps_.log2_min_pcm_luma_coding_block_size_minus3 = sps.log2_min_pcm_luma_coding_block_size_minus3;
				sps_.log2_diff_max_min_pcm_luma_coding_block_size = sps.log2_diff_max_min_pcm_luma_coding_block_size;
				if (has_sps_scc_extension) {
					sps_.palette_max_size = scc.palette_max_size;
					sps_.delta_palette_max_predictor_size = scc.delta_palette_max_predictor_size;
					sps_.motion_vector_resolution_control_idc = scc.motion_vector_resolution_control_idc;
					sps_.sps_num_palette_predictor_initializers_minus1 =
							scc.sps_num_palette_predictor_initializer_minus1;
				}
				sps_.conf_win_left_offset = sps.conf_win_left_offset;
				sps_.conf_win_right_offset = sps.conf_win_right_offset;
				sps_.conf_win_top_offset = sps.conf_win_top_offset;
				sps_.conf_win_bottom_offset = sps.conf_win_bottom_offset;
				sps_.pProfileTierLevel = &sps_profile_;
				sps_.pDecPicBufMgr = &sps_dpb_;
				if (sps.scaling_list_enabled_flag != 0U && sps.scaling_list_data_present_flag != 0U) {
					lower_scaling(sps.scaling_list, sps_scaling_);
					sps_.pScalingLists = &sps_scaling_;
				}
				for (std::uint8_t index = 0; index < sps_.num_short_term_ref_pic_sets; ++index) {
					lower_rps(sps.short_term_ref_pic_set[index], rps_[index]);
				}
				sps_.pShortTermRefPicSet = rps_.data();
				for (std::uint8_t index = 0; index < sps_.num_long_term_ref_pics_sps; ++index) {
					long_term_.lt_ref_pic_poc_lsb_sps[index] = sps.lt_ref_pic_poc_lsb_sps[index];
					if (sps.used_by_curr_pic_lt_sps_flag[index] != 0U) {
						long_term_.used_by_curr_pic_lt_sps_flag |= 1U << index;
					}
				}
				if (sps.long_term_ref_pics_present_flag != 0U) { sps_.pLongTermRefPicsSps = &long_term_; }
				if (sps.vui_parameters_present_flag != 0U) {
					const auto &vui = sps.vui_params;
					vui_.flags.aspect_ratio_info_present_flag = vui.aspect_ratio_info_present_flag;
					vui_.flags.overscan_info_present_flag = vui.overscan_info_present_flag;
					vui_.flags.overscan_appropriate_flag = vui.overscan_appropriate_flag;
					vui_.flags.video_signal_type_present_flag = vui.video_signal_type_present_flag;
					vui_.flags.video_full_range_flag = vui.video_full_range_flag;
					vui_.flags.colour_description_present_flag = vui.colour_description_present_flag;
					vui_.flags.chroma_loc_info_present_flag = vui.chroma_loc_info_present_flag;
					vui_.flags.neutral_chroma_indication_flag = vui.neutral_chroma_indication_flag;
					vui_.flags.field_seq_flag = vui.field_seq_flag;
					vui_.flags.frame_field_info_present_flag = vui.frame_field_info_present_flag;
					vui_.flags.default_display_window_flag = vui.default_display_window_flag;
					vui_.flags.vui_timing_info_present_flag = vui.timing_info_present_flag;
					vui_.flags.vui_poc_proportional_to_timing_flag = vui.poc_proportional_to_timing_flag;
					vui_.flags.bitstream_restriction_flag = vui.bitstream_restriction_flag;
					vui_.flags.tiles_fixed_structure_flag = vui.tiles_fixed_structure_flag;
					vui_.flags.motion_vectors_over_pic_boundaries_flag = vui.motion_vectors_over_pic_boundaries_flag;
					vui_.flags.restricted_ref_pic_lists_flag = vui.restricted_ref_pic_lists_flag;
					vui_.aspect_ratio_idc = static_cast<StdVideoH265AspectRatioIdc>(vui.aspect_ratio_idc);
					vui_.sar_width = vui.sar_width;
					vui_.sar_height = vui.sar_height;
					vui_.video_format = vui.video_format;
					vui_.colour_primaries = vui.colour_primaries;
					vui_.transfer_characteristics = vui.transfer_characteristics;
					vui_.matrix_coeffs = vui.matrix_coefficients;
					vui_.chroma_sample_loc_type_top_field = vui.chroma_sample_loc_type_top_field;
					vui_.chroma_sample_loc_type_bottom_field = vui.chroma_sample_loc_type_bottom_field;
					vui_.def_disp_win_left_offset = static_cast<std::uint16_t>(vui.def_disp_win_left_offset);
					vui_.def_disp_win_right_offset = static_cast<std::uint16_t>(vui.def_disp_win_right_offset);
					vui_.def_disp_win_top_offset = static_cast<std::uint16_t>(vui.def_disp_win_top_offset);
					vui_.def_disp_win_bottom_offset = static_cast<std::uint16_t>(vui.def_disp_win_bottom_offset);
					vui_.vui_num_units_in_tick = vui.num_units_in_tick;
					vui_.vui_time_scale = vui.time_scale;
					vui_.vui_num_ticks_poc_diff_one_minus1 = vui.num_ticks_poc_diff_one_minus1;
					vui_.min_spatial_segmentation_idc = vui.min_spatial_segmentation_idc;
					vui_.max_bytes_per_pic_denom = vui.max_bytes_per_pic_denom;
					vui_.max_bits_per_min_cu_denom = vui.max_bits_per_min_cu_denom;
					vui_.log2_max_mv_length_horizontal = vui.log2_max_mv_length_horizontal;
					vui_.log2_max_mv_length_vertical = vui.log2_max_mv_length_vertical;
					sps_.pSequenceParameterSetVui = &vui_;
				}
				if (has_sps_scc_extension && scc.sps_palette_predictor_initializers_present_flag != 0U) {
					for (std::size_t component = 0; component < 3U; ++component) {
						for (std::size_t index = 0; index < 128U; ++index) {
							sps_palette_.PredictorPaletteEntries[component][index] =
									static_cast<std::uint16_t>(scc.sps_palette_predictor_initializer[component][index]);
						}
					}
					sps_.pPredictorPaletteEntries = &sps_palette_;
				}

				pps_.flags.dependent_slice_segments_enabled_flag = pps.dependent_slice_segments_enabled_flag;
				pps_.flags.output_flag_present_flag = pps.output_flag_present_flag;
				pps_.flags.sign_data_hiding_enabled_flag = pps.sign_data_hiding_enabled_flag;
				pps_.flags.cabac_init_present_flag = pps.cabac_init_present_flag;
				pps_.flags.constrained_intra_pred_flag = pps.constrained_intra_pred_flag;
				pps_.flags.transform_skip_enabled_flag = pps.transform_skip_enabled_flag;
				pps_.flags.cu_qp_delta_enabled_flag = pps.cu_qp_delta_enabled_flag;
				pps_.flags.pps_slice_chroma_qp_offsets_present_flag = pps.slice_chroma_qp_offsets_present_flag;
				pps_.flags.weighted_pred_flag = pps.weighted_pred_flag;
				pps_.flags.weighted_bipred_flag = pps.weighted_bipred_flag;
				pps_.flags.transquant_bypass_enabled_flag = pps.transquant_bypass_enabled_flag;
				pps_.flags.tiles_enabled_flag = pps.tiles_enabled_flag;
				pps_.flags.entropy_coding_sync_enabled_flag = pps.entropy_coding_sync_enabled_flag;
				pps_.flags.uniform_spacing_flag = pps.uniform_spacing_flag;
				pps_.flags.loop_filter_across_tiles_enabled_flag = pps.loop_filter_across_tiles_enabled_flag;
				pps_.flags.pps_loop_filter_across_slices_enabled_flag = pps.loop_filter_across_slices_enabled_flag;
				pps_.flags.deblocking_filter_control_present_flag = pps.deblocking_filter_control_present_flag;
				pps_.flags.deblocking_filter_override_enabled_flag = pps.deblocking_filter_override_enabled_flag;
				pps_.flags.pps_deblocking_filter_disabled_flag = pps.deblocking_filter_disabled_flag;
				pps_.flags.pps_scaling_list_data_present_flag = pps.scaling_list_data_present_flag;
				pps_.flags.lists_modification_present_flag = pps.lists_modification_present_flag;
				pps_.flags.slice_segment_header_extension_present_flag = pps.slice_segment_header_extension_present_flag;
				pps_.flags.pps_extension_present_flag = has_pps_range_extension || has_pps_scc_extension;
				pps_.flags.pps_range_extension_flag = has_pps_range_extension;
				const auto &pps_range = pps.pps_extension_params;
				if (has_pps_range_extension) {
					pps_.flags.cross_component_prediction_enabled_flag = pps_range.cross_component_prediction_enabled_flag;
					pps_.flags.chroma_qp_offset_list_enabled_flag = pps_range.chroma_qp_offset_list_enabled_flag;
				}
				const auto &pps_scc = pps.pps_scc_extension_params;
				if (has_pps_scc_extension) {
					pps_.flags.pps_curr_pic_ref_enabled_flag = pps_scc.pps_curr_pic_ref_enabled_flag;
					pps_.flags.residual_adaptive_colour_transform_enabled_flag =
							pps_scc.residual_adaptive_colour_transform_enabled_flag;
					pps_.flags.pps_slice_act_qp_offsets_present_flag = pps_scc.pps_slice_act_qp_offsets_present_flag;
					pps_.flags.pps_palette_predictor_initializers_present_flag =
							pps_scc.pps_palette_predictor_initializers_present_flag;
					pps_.flags.monochrome_palette_flag = pps_scc.monochrome_palette_flag;
				}
				pps_.pps_pic_parameter_set_id = static_cast<std::uint8_t>(pps.id);
				pps_.pps_seq_parameter_set_id = static_cast<std::uint8_t>(pps.sps_id);
				pps_.sps_video_parameter_set_id = sps.vps_id;
				pps_.num_extra_slice_header_bits = pps.num_extra_slice_header_bits;
				pps_.num_ref_idx_l0_default_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
				pps_.num_ref_idx_l1_default_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
				pps_.init_qp_minus26 = pps.init_qp_minus26;
				pps_.diff_cu_qp_delta_depth = pps.diff_cu_qp_delta_depth;
				pps_.pps_cb_qp_offset = pps.cb_qp_offset;
				pps_.pps_cr_qp_offset = pps.cr_qp_offset;
				pps_.pps_beta_offset_div2 = pps.beta_offset_div2;
				pps_.pps_tc_offset_div2 = pps.tc_offset_div2;
				pps_.log2_parallel_merge_level_minus2 = pps.log2_parallel_merge_level_minus2;
				if (has_pps_range_extension) {
					pps_.log2_max_transform_skip_block_size_minus2 =
							static_cast<std::uint8_t>(pps_range.log2_max_transform_skip_block_size_minus2);
					pps_.diff_cu_chroma_qp_offset_depth = pps_range.diff_cu_chroma_qp_offset_depth;
					pps_.chroma_qp_offset_list_len_minus1 = pps_range.chroma_qp_offset_list_len_minus1;
					std::memcpy(pps_.cb_qp_offset_list, pps_range.cb_qp_offset_list, sizeof(pps_.cb_qp_offset_list));
					std::memcpy(pps_.cr_qp_offset_list, pps_range.cr_qp_offset_list, sizeof(pps_.cr_qp_offset_list));
					pps_.log2_sao_offset_scale_luma = pps_range.log2_sao_offset_scale_luma;
					pps_.log2_sao_offset_scale_chroma = pps_range.log2_sao_offset_scale_chroma;
				}
				if (has_pps_scc_extension) {
					pps_.pps_act_y_qp_offset_plus5 = static_cast<std::int8_t>(pps_scc.pps_act_y_qp_offset_plus5);
					pps_.pps_act_cb_qp_offset_plus5 = static_cast<std::int8_t>(pps_scc.pps_act_cb_qp_offset_plus5);
					pps_.pps_act_cr_qp_offset_plus3 = static_cast<std::int8_t>(pps_scc.pps_act_cr_qp_offset_plus3);
					pps_.pps_num_palette_predictor_initializers = pps_scc.pps_num_palette_predictor_initializer;
					pps_.luma_bit_depth_entry_minus8 = pps_scc.luma_bit_depth_entry_minus8;
					pps_.chroma_bit_depth_entry_minus8 = static_cast<std::uint8_t>(pps_scc.chroma_bit_depth_entry_minus8);
				}
				pps_.num_tile_columns_minus1 = pps.num_tile_columns_minus1;
				pps_.num_tile_rows_minus1 = pps.num_tile_rows_minus1;
				for (std::size_t index = 0; index < std::size(pps_.column_width_minus1); ++index) {
					pps_.column_width_minus1[index] = static_cast<std::uint16_t>(pps.column_width_minus1[index]);
				}
				for (std::size_t index = 0; index < std::size(pps_.row_height_minus1); ++index) {
					pps_.row_height_minus1[index] = static_cast<std::uint16_t>(pps.row_height_minus1[index]);
				}
				if (pps.scaling_list_data_present_flag != 0U) {
					lower_scaling(pps.scaling_list, pps_scaling_);
					pps_.pScalingLists = &pps_scaling_;
				}
				if (has_pps_scc_extension && pps_scc.pps_palette_predictor_initializers_present_flag != 0U) {
					for (std::size_t component = 0; component < 3U; ++component) {
						for (std::size_t index = 0; index < 128U; ++index) {
							pps_palette_.PredictorPaletteEntries[component][index] = static_cast<std::uint16_t>(
									pps_scc.pps_palette_predictor_initializer[component][index]);
						}
					}
					pps_.pPredictorPaletteEntries = &pps_palette_;
				}
			}
		};
	} // namespace

	struct Vulkan_Video_Decode_Context::Impl {
		struct Image_State {
			VkImage image_ = VK_NULL_HANDLE;
			VkImageView view_ = VK_NULL_HANDLE;
			VkFormat format_ = VK_FORMAT_UNDEFINED;
			bool initialized_ = false;
			std::int32_t slot_ = -1;
		};

		struct Submission {
			VkCommandBuffer command_ = VK_NULL_HANDLE;
			VkFence fence_ = VK_NULL_HANDLE;
			VkBuffer bitstream_buffer_ = VK_NULL_HANDLE;
			VkDeviceMemory bitstream_memory_ = VK_NULL_HANDLE;
			void *bitstream_mapping_ = nullptr;
			VkDeviceSize bitstream_capacity_ = 0U;
			bool pending_ = false;
		};

		H265_Sequence_Info sequence_;
		Vulkan_Device_Context device_;
		VkVideoDecodeH265ProfileInfoKHR h265_profile_{};
		VkVideoProfileInfoKHR profile_{};
		VkVideoDecodeH265CapabilitiesKHR h265_capabilities_{};
		VkVideoDecodeCapabilitiesKHR decode_capabilities_{};
		VkVideoCapabilitiesKHR capabilities_{};
		VkVideoSessionKHR session_ = VK_NULL_HANDLE;
		VkVideoSessionParametersKHR parameters_ = VK_NULL_HANDLE;
		const H265_Parameter_Snapshot *parameter_identity_ = nullptr;
		std::unique_ptr<Parameter_Bundle> parameter_bundle_;
		std::vector<VkVideoSessionParametersKHR> retired_parameters_;
		std::vector<std::unique_ptr<Parameter_Bundle>> retired_parameter_bundles_;
		std::vector<VkDeviceMemory> session_memory_;
		VkCommandPool command_pool_ = VK_NULL_HANDLE;
		std::list<Submission> submissions_;
		VkFormat session_format_ = VK_FORMAT_UNDEFINED;
		VkDeviceSize bitstream_offset_alignment_ = 1U;
		VkDeviceSize bitstream_size_alignment_ = 1U;
		VkDeviceSize max_bitstream_size_ = 0U;
		std::uint32_t max_active_references_ = 0U;
		bool initialized_ = false;
		bool reset_ = true;
		std::map<std::uintptr_t, Image_State> images_;
		std::array<Image_State *, 16> dpb_slots_{};
		std::vector<std::uint8_t> source_bytes_;
		std::vector<std::uint32_t> slice_offsets_;
		std::mutex mutex_;

		PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_capabilities_ = nullptr;
		PFN_vkCreateVideoSessionKHR create_session_ = nullptr;
		PFN_vkDestroyVideoSessionKHR destroy_session_ = nullptr;
		PFN_vkGetVideoSessionMemoryRequirementsKHR get_memory_requirements_ = nullptr;
		PFN_vkBindVideoSessionMemoryKHR bind_memory_ = nullptr;
		PFN_vkCreateVideoSessionParametersKHR create_parameters_ = nullptr;
		PFN_vkDestroyVideoSessionParametersKHR destroy_parameters_ = nullptr;
		PFN_vkCmdBeginVideoCodingKHR begin_coding_ = nullptr;
		PFN_vkCmdControlVideoCodingKHR control_coding_ = nullptr;
		PFN_vkCmdDecodeVideoKHR decode_video_ = nullptr;
		PFN_vkCmdEndVideoCodingKHR end_coding_ = nullptr;

		~Impl() {
			for (auto &submission: submissions_) {
				if (submission.pending_ && vkGetFenceStatus(device_.device_, submission.fence_) == VK_NOT_READY) {
					(void) vkWaitForFences(device_.device_, 1U, &submission.fence_, VK_TRUE, UINT64_MAX);
				}
			}
			for (auto &submission: submissions_) {
				if (submission.bitstream_mapping_ != nullptr) {
					vkUnmapMemory(device_.device_, submission.bitstream_memory_);
				}
				if (submission.bitstream_buffer_ != VK_NULL_HANDLE) {
					vkDestroyBuffer(device_.device_, submission.bitstream_buffer_, nullptr);
				}
				if (submission.bitstream_memory_ != VK_NULL_HANDLE) {
					vkFreeMemory(device_.device_, submission.bitstream_memory_, nullptr);
				}
				if (submission.fence_ != VK_NULL_HANDLE) {
					vkDestroyFence(device_.device_, submission.fence_, nullptr);
				}
			}
			for (const auto &[key, image]: images_) {
				(void) key;
				if (image.view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_.device_, image.view_, nullptr); }
			}
			if (parameters_ != VK_NULL_HANDLE && destroy_parameters_ != nullptr) {
				destroy_parameters_(device_.device_, parameters_, nullptr);
			}
			if (destroy_parameters_ != nullptr) {
				for (const auto parameters: retired_parameters_) {
					destroy_parameters_(device_.device_, parameters, nullptr);
				}
			}
			if (session_ != VK_NULL_HANDLE && destroy_session_ != nullptr) {
				destroy_session_(device_.device_, session_, nullptr);
			}
			for (const auto memory: session_memory_) { vkFreeMemory(device_.device_, memory, nullptr); }
			if (command_pool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device_.device_, command_pool_, nullptr); }
		}

		[[nodiscard]] Result<Image_State *> image_state(const Exported_Dmabuf_Image &storage) {
			if (!storage.memory_objects_.empty() || storage.image_planes_.empty() ||
					storage.image_planes_[0].image_ == VK_NULL_HANDLE) {
				return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
						"H.265 Vulkan Video requires a frame allocated directly on the editor VkDevice"));
			}
			const auto image = storage.image_planes_[0].image_;
			auto [position, inserted] = images_.try_emplace(reinterpret_cast<std::uintptr_t>(image));
			auto &state = position->second;
			if (inserted) {
				state.image_ = image;
				state.format_ = storage.format_;
				VkImageViewCreateInfo view_info{
						.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
						.image = image,
						.viewType = VK_IMAGE_VIEW_TYPE_2D,
						.format = storage.format_,
						.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U},
				};
				if (const auto result = vkCreateImageView(device_.device_, &view_info, nullptr, &state.view_);
						result != VK_SUCCESS) {
					images_.erase(position);
					return std::unexpected(vk_error("vkCreateImageView(H.265 picture)", result));
				}
			} else if (state.format_ != storage.format_) {
				return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
						"H.265 Vulkan Video image was rebound with a different format"));
			}
			return &state;
		}

		[[nodiscard]] Result<void> ensure_bitstream_buffer(Submission &submission, VkDeviceSize required_size) {
			if (required_size <= submission.bitstream_capacity_) { return {}; }
			if (required_size > max_bitstream_size_) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_BITSTREAM_FEATURE,
						"H.265 picture exceeds the hardware Vulkan Video bitstream-buffer limit"));
			}
			VkDeviceSize capacity = std::max<VkDeviceSize>(submission.bitstream_capacity_, 1U << 20U);
			while (capacity < required_size && capacity <= max_bitstream_size_ / 2U) { capacity *= 2U; }
			capacity = align_up(std::max(capacity, required_size), bitstream_size_alignment_);
			capacity = std::min(capacity, max_bitstream_size_);

			VkVideoProfileListInfoKHR buffer_profile{.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
					.profileCount = 1U, .pProfiles = &profile_};
			VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					.pNext = &buffer_profile, .size = capacity,
					.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
			VkBuffer new_buffer = VK_NULL_HANDLE;
			if (const auto result = vkCreateBuffer(device_.device_, &buffer_info, nullptr, &new_buffer);
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkCreateBuffer(H.265 persistent bitstream)", result));
			}
			VkMemoryRequirements requirements{};
			vkGetBufferMemoryRequirements(device_.device_, new_buffer, &requirements);
			const auto type = memory_type(device_.physical_device_, requirements.memoryTypeBits,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (type == std::numeric_limits<std::uint32_t>::max()) {
				vkDestroyBuffer(device_.device_, new_buffer, nullptr);
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
						"H.265 bitstream buffer has no coherent host-visible memory"));
			}
			VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					.allocationSize = requirements.size, .memoryTypeIndex = type};
			VkDeviceMemory new_memory = VK_NULL_HANDLE;
			if (const auto result = vkAllocateMemory(device_.device_, &allocation, nullptr, &new_memory);
					result != VK_SUCCESS) {
				vkDestroyBuffer(device_.device_, new_buffer, nullptr);
				return std::unexpected(vk_error("vkAllocateMemory(H.265 persistent bitstream)", result));
			}
			if (const auto result = vkBindBufferMemory(device_.device_, new_buffer, new_memory, 0U);
					result != VK_SUCCESS) {
				vkFreeMemory(device_.device_, new_memory, nullptr);
				vkDestroyBuffer(device_.device_, new_buffer, nullptr);
				return std::unexpected(vk_error("vkBindBufferMemory(H.265 persistent bitstream)", result));
			}
			void *new_mapping = nullptr;
			if (const auto result = vkMapMemory(device_.device_, new_memory, 0U, capacity, 0U, &new_mapping);
					result != VK_SUCCESS) {
				vkFreeMemory(device_.device_, new_memory, nullptr);
				vkDestroyBuffer(device_.device_, new_buffer, nullptr);
				return std::unexpected(vk_error("vkMapMemory(H.265 persistent bitstream)", result));
			}
			if (submission.bitstream_mapping_ != nullptr) {
				vkUnmapMemory(device_.device_, submission.bitstream_memory_);
			}
			if (submission.bitstream_buffer_ != VK_NULL_HANDLE) {
				vkDestroyBuffer(device_.device_, submission.bitstream_buffer_, nullptr);
			}
			if (submission.bitstream_memory_ != VK_NULL_HANDLE) {
				vkFreeMemory(device_.device_, submission.bitstream_memory_, nullptr);
			}
			submission.bitstream_buffer_ = new_buffer;
			submission.bitstream_memory_ = new_memory;
			submission.bitstream_mapping_ = new_mapping;
			submission.bitstream_capacity_ = capacity;
			return {};
		}

		[[nodiscard]] Result<void> initialize(VkFormat format) {
			if (initialized_) {
				if (format != session_format_) {
					return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
							"H.265 output format differs from the Vulkan Video session format"));
				}
				return {};
			}
			if (device_.instance_ == VK_NULL_HANDLE || device_.physical_device_ == VK_NULL_HANDLE ||
					device_.device_ == VK_NULL_HANDLE || device_.queue_ == VK_NULL_HANDLE || device_.queue_mutex_ == nullptr) {
				return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
						"H.265 Vulkan Video requires a live borrowed editor VkDevice/video queue"));
			}
			if ((device_.video_codec_operations_ & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) == 0U) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
						"shared editor VkDevice does not expose H.265 decode on its video queue"));
			}
			const auto exposed_component_depth = [](std::uint8_t depth) {
				return depth == 8U || depth == 10U || depth == 12U;
			};
			if (!exposed_component_depth(sequence_.bit_depth_) ||
				(sequence_.chroma_format_ != H265_Chroma_Format::MONOCHROME &&
				 !exposed_component_depth(sequence_.chroma_bit_depth_))) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_BITSTREAM_FEATURE,
						"Vulkan Video exposes only 8-bit, 10-bit, and 12-bit H.265 component depths"));
			}
			StdVideoH265ProfileIdc std_profile = STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
			switch (sequence_.profile_) {
				case H265_Profile::MAIN: std_profile = STD_VIDEO_H265_PROFILE_IDC_MAIN; break;
				case H265_Profile::MAIN_10: std_profile = STD_VIDEO_H265_PROFILE_IDC_MAIN_10; break;
				case H265_Profile::MAIN_STILL_PICTURE:
					std_profile = STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE;
					break;
				case H265_Profile::SCC_MAIN:
				case H265_Profile::SCC_MAIN_10:
				case H265_Profile::SCC_MAIN_444:
				case H265_Profile::SCC_MAIN_444_10:
					std_profile = STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS;
					break;
				case H265_Profile::UNSUPPORTED_EXTENSION:
					return std::unexpected(error(Media_Error_Code::UNSUPPORTED_BITSTREAM_FEATURE,
							"Vulkan Video does not define this H.265 bitstream profile"));
				default: break;
			}
			h265_profile_.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
			h265_profile_.stdProfileIdc = std_profile;
			profile_.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
			profile_.pNext = &h265_profile_;
			profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
			profile_.chromaSubsampling = static_cast<VkVideoChromaSubsamplingFlagBitsKHR>(
					VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR << static_cast<std::uint32_t>(sequence_.chroma_format_));
			profile_.lumaBitDepth = sequence_.bit_depth_ <= 8U ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR
					: (sequence_.bit_depth_ <= 10U ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
							: VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR);
			profile_.chromaBitDepth = sequence_.chroma_bit_depth_ <= 8U ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR
					: (sequence_.chroma_bit_depth_ <= 10U ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
							: VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR);
			h265_capabilities_.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
			decode_capabilities_.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
			decode_capabilities_.pNext = &h265_capabilities_;
			capabilities_.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
			capabilities_.pNext = &decode_capabilities_;
			get_capabilities_ = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
					vkGetInstanceProcAddr(device_.instance_, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
			if (get_capabilities_ == nullptr) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
						"Vulkan loader has no video capability query"));
			}
			if (const auto result = get_capabilities_(device_.physical_device_, &profile_, &capabilities_);
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkGetPhysicalDeviceVideoCapabilitiesKHR(H.265)", result));
			}
			if ((decode_capabilities_.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) == 0U ||
					capabilities_.maxDpbSlots < 16U || capabilities_.maxActiveReferencePictures == 0U) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
						"H.265 backend requires coincident output/DPB images and the full 16-slot HEVC DPB"));
			}
			max_active_references_ = capabilities_.maxActiveReferencePictures;
			bitstream_offset_alignment_ = std::max<VkDeviceSize>(1U, capabilities_.minBitstreamBufferOffsetAlignment);
			bitstream_size_alignment_ = std::max<VkDeviceSize>(1U, capabilities_.minBitstreamBufferSizeAlignment);
			max_bitstream_size_ = (std::numeric_limits<std::uint32_t>::max() /
					bitstream_size_alignment_) * bitstream_size_alignment_;
			const auto load = [&](auto &function, const char *name) {
				function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(vkGetDeviceProcAddr(device_.device_, name));
				return function != nullptr;
			};
			if (!load(create_session_, "vkCreateVideoSessionKHR") ||
					!load(destroy_session_, "vkDestroyVideoSessionKHR") ||
					!load(get_memory_requirements_, "vkGetVideoSessionMemoryRequirementsKHR") ||
					!load(bind_memory_, "vkBindVideoSessionMemoryKHR") ||
					!load(create_parameters_, "vkCreateVideoSessionParametersKHR") ||
					!load(destroy_parameters_, "vkDestroyVideoSessionParametersKHR") ||
					!load(begin_coding_, "vkCmdBeginVideoCodingKHR") ||
					!load(control_coding_, "vkCmdControlVideoCodingKHR") ||
					!load(decode_video_, "vkCmdDecodeVideoKHR") ||
					!load(end_coding_, "vkCmdEndVideoCodingKHR")) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
						"shared VkDevice is missing a required H.265 Vulkan Video command"));
			}
			VkVideoSessionCreateInfoKHR session_info{
					.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
					.queueFamilyIndex = device_.queue_family_, .pVideoProfile = &profile_,
					.pictureFormat = format,
					.maxCodedExtent = {sequence_.max_width_, sequence_.max_height_},
					.referencePictureFormat = format, .maxDpbSlots = 16U,
					.maxActiveReferencePictures = max_active_references_,
					.pStdHeaderVersion = &capabilities_.stdHeaderVersion};
			if (const auto result = create_session_(device_.device_, &session_info, nullptr, &session_);
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkCreateVideoSessionKHR(H.265)", result));
			}
			std::uint32_t memory_count = 0U;
			if (const auto result = get_memory_requirements_(device_.device_, session_, &memory_count, nullptr);
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkGetVideoSessionMemoryRequirementsKHR(H.265 count)", result));
			}
			std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(memory_count);
			for (auto &requirement: requirements) { requirement.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR; }
			if (const auto result = get_memory_requirements_(device_.device_, session_, &memory_count, requirements.data());
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkGetVideoSessionMemoryRequirementsKHR(H.265)", result));
			}
			std::vector<VkBindVideoSessionMemoryInfoKHR> bindings;
			bindings.reserve(requirements.size());
			for (const auto &requirement: requirements) {
				auto type = memory_type(device_.physical_device_, requirement.memoryRequirements.memoryTypeBits,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				// Session state may require host-local memory even though output images stay on the GPU.
				if (type == std::numeric_limits<std::uint32_t>::max()) {
					type = memory_type(device_.physical_device_, requirement.memoryRequirements.memoryTypeBits, 0U);
				}
				if (type == std::numeric_limits<std::uint32_t>::max()) {
					return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
							"H.265 video session has no compatible memory type"));
				}
				VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
						.allocationSize = requirement.memoryRequirements.size, .memoryTypeIndex = type};
				VkDeviceMemory memory = VK_NULL_HANDLE;
				if (const auto result = vkAllocateMemory(device_.device_, &allocation, nullptr, &memory);
						result != VK_SUCCESS) {
					return std::unexpected(vk_error("vkAllocateMemory(H.265 session)", result));
				}
				session_memory_.push_back(memory);
				bindings.push_back(VkBindVideoSessionMemoryInfoKHR{
						.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
						.memoryBindIndex = requirement.memoryBindIndex, .memory = memory,
						.memoryOffset = 0U, .memorySize = requirement.memoryRequirements.size});
			}
			if (const auto result = bind_memory_(device_.device_, session_, memory_count, bindings.data());
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkBindVideoSessionMemoryKHR(H.265)", result));
			}
			VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					.queueFamilyIndex = device_.queue_family_};
			if (const auto result = vkCreateCommandPool(device_.device_, &pool_info, nullptr, &command_pool_);
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkCreateCommandPool(H.265)", result));
			}
			session_format_ = format;
			initialized_ = true;
			return {};
		}

		[[nodiscard]] Result<void> ensure_parameters(const H265_Parameter_Snapshot &source) {
			if (parameter_identity_ == &source && parameters_ != VK_NULL_HANDLE) { return {}; }
			if (parameters_ != VK_NULL_HANDLE) {
				// Commands already submitted with this object may still be executing.
				// Retain both the Vulkan object and the StdVideo backing storage;
				// decode_frame() is forbidden from waiting for their retirement.
				retired_parameters_.push_back(parameters_);
				retired_parameter_bundles_.push_back(std::move(parameter_bundle_));
				parameters_ = VK_NULL_HANDLE;
			}
			parameter_bundle_ = std::make_unique<Parameter_Bundle>(source);
			VkVideoDecodeH265SessionParametersAddInfoKHR add{
					.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR,
					.stdVPSCount = 1U, .pStdVPSs = &parameter_bundle_->vps_,
					.stdSPSCount = 1U, .pStdSPSs = &parameter_bundle_->sps_,
					.stdPPSCount = 1U, .pStdPPSs = &parameter_bundle_->pps_};
			VkVideoDecodeH265SessionParametersCreateInfoKHR h265_create{
					.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,
					.maxStdVPSCount = 1U, .maxStdSPSCount = 1U, .maxStdPPSCount = 1U,
					.pParametersAddInfo = &add};
			VkVideoSessionParametersCreateInfoKHR create_info{
					.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
					.pNext = &h265_create, .videoSession = session_};
			if (const auto result = create_parameters_(device_.device_, &create_info, nullptr, &parameters_);
					result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkCreateVideoSessionParametersKHR(H.265)", result));
			}
			parameter_identity_ = &source;
			return {};
		}

		[[nodiscard]] Result<Decode_Frame_Result> decode(const Parsed_Decode_Frame &frame,
				const Frame_Reference_State &state, const H265_Frame_Decode_Metadata &metadata,
				std::span<const std::uint8_t> bitstream, std::uint64_t logical_bitstream_begin,
				Decode_Frame_Request request) {
			std::lock_guard context_lock{mutex_};
			if (frame.slices_.empty() || frame.parameters_ == nullptr) {
				return std::unexpected(error(Media_Error_Code::INVALID_H265_BITSTREAM,
						"H.265 Vulkan Video decode requires parsed slices and parameter sets"));
			}
			if (auto result = initialize(request.output_.image_.format_); !result) {
				return std::unexpected(result.error());
			}
			if (auto result = ensure_parameters(*frame.parameters_); !result) {
				return std::unexpected(result.error());
			}
			if ((metadata.unavailable_reference_mask_ & metadata.active_reference_mask_) != 0U) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_BITSTREAM_FEATURE,
						"H.265 Vulkan Video unavailable-reference concealment is not implemented yet"));
			}
			if (has_point(request.output_.safe_to_write_) || has_point(request.output_.decode_complete_) ||
					std::ranges::any_of(request.references_, [](const auto &reference) {
						return has_point(reference.acquire_for_read_) || has_point(reference.release_after_read_);
					})) {
				return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
						"native H.265 Vulkan Video accepts only same-device Vulkan timeline semaphores"));
			}
			const auto validate_native_point = [](av1::Vulkan_Semaphore_Point point,
					std::string_view name) -> Result<void> {
				if (point.semaphore_ == VK_NULL_HANDLE) {
					return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
							std::string{name} + " must belong to the editor VkDevice"));
				}
				return {};
			};
			if (auto result = validate_native_point(request.output_.vulkan_safe_to_write_,
					"H.265 output safe-to-write"); !result) {
				return std::unexpected(result.error());
			}
			if (auto result = validate_native_point(request.output_.vulkan_decode_complete_,
					"H.265 output decode-complete"); !result) {
				return std::unexpected(result.error());
			}
			for (const auto &reference: request.references_) {
				if (auto result = validate_native_point(reference.vulkan_acquire_for_read_,
						"H.265 reference acquire-for-read"); !result) {
					return std::unexpected(result.error());
				}
				if (auto result = validate_native_point(reference.vulkan_release_after_read_,
						"H.265 reference release-after-read"); !result) {
					return std::unexpected(result.error());
				}
			}
			Submission *selected_submission = nullptr;
			for (auto &candidate: submissions_) {
				if (candidate.pending_) {
					const auto status = vkGetFenceStatus(device_.device_, candidate.fence_);
					if (status == VK_NOT_READY) { continue; }
					if (status != VK_SUCCESS) {
						return std::unexpected(vk_error("vkGetFenceStatus(H.265 acquire)", status));
					}
					candidate.pending_ = false;
				}
				selected_submission = &candidate;
				break;
			}
			if (selected_submission == nullptr) {
				submissions_.emplace_back();
				auto &created = submissions_.back();
				VkCommandBufferAllocateInfo command_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
						.commandPool = command_pool_, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
						.commandBufferCount = 1U};
				if (const auto result = vkAllocateCommandBuffers(device_.device_, &command_info, &created.command_);
						result != VK_SUCCESS) {
					submissions_.pop_back();
					return std::unexpected(vk_error("vkAllocateCommandBuffers(H.265 submission)", result));
				}
				VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				if (const auto result = vkCreateFence(device_.device_, &fence_info, nullptr, &created.fence_);
						result != VK_SUCCESS) {
					vkFreeCommandBuffers(device_.device_, command_pool_, 1U, &created.command_);
					submissions_.pop_back();
					return std::unexpected(vk_error("vkCreateFence(H.265 submission)", result));
				}
				selected_submission = &created;
			}
			auto &submission = *selected_submission;
			(void) vkResetFences(device_.device_, 1U, &submission.fence_);
			(void) vkResetCommandBuffer(submission.command_, 0U);
			auto output_result = image_state(request.output_.image_);
			if (!output_result) { return std::unexpected(output_result.error()); }
			auto &output = **output_result;

			std::array<Image_State *, 15> reference_images{};
			for (const auto &reference: request.references_) {
				if (reference.reference_slot_ >= reference_images.size()) {
					return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
							"H.265 Vulkan Video reference slot is outside the compact DPB"));
				}
				const auto expected = metadata.reference_frame_index_[reference.reference_slot_];
				if (expected == kInvalid_H265_Frame_Index || reference.frame_.value_ != expected) {
					return std::unexpected(error(Media_Error_Code::INVALID_REFERENCE_STATE,
							"H.265 Vulkan Video reference does not match the indexed DPB binding"));
				}
				auto image = image_state(reference.image_);
				if (!image) { return std::unexpected(image.error()); }
				if (!(*image)->initialized_) {
					return std::unexpected(error(Media_Error_Code::INVALID_REFERENCE_STATE,
							"H.265 Vulkan Video reference image has not been decoded in this session"));
				}
				reference_images[reference.reference_slot_] = *image;
			}
			for (std::uint8_t slot = 0; slot < reference_images.size(); ++slot) {
				if ((metadata.active_reference_mask_ & (1U << slot)) != 0U && reference_images[slot] == nullptr) {
					return std::unexpected(error(Media_Error_Code::INVALID_REFERENCE_STATE,
							"H.265 Vulkan Video request is missing an active indexed reference image"));
				}
			}
			const auto active_reference_count = std::ranges::count_if(request.references_, [&](const auto &reference) {
				return reference.reference_slot_ < state.reference_va_flags_.size() &&
						state.reference_va_flags_[reference.reference_slot_] != 0U;
			});
			if (active_reference_count > max_active_references_) {
				return std::unexpected(error(Media_Error_Code::UNSUPPORTED_DRIVER_FEATURE,
						"H.265 picture exceeds the hardware active-reference limit"));
			}
			const auto is_current_reference = [&](const Image_State *candidate) {
				return std::ranges::any_of(reference_images,
						[candidate](const Image_State *reference) { return reference == candidate; });
			};
			// A caller-owned image is the durable DPB state.  Vulkan's numeric DPB
			// slots are transient bindings and may be reconstructed for every
			// submission, including after decoding a completely different source.
			auto planned_dpb_slots = request.independent_random_access_
					? std::array<Image_State *, 16>{}
					: dpb_slots_;
			std::map<Image_State *, std::int32_t> planned_reference_slots;
			for (auto *reference: reference_images) {
				if (reference == nullptr || planned_reference_slots.contains(reference)) { continue; }
				if (reference->slot_ >= 0 &&
						planned_dpb_slots[static_cast<std::size_t>(reference->slot_)] == reference) {
					planned_reference_slots.emplace(reference, reference->slot_);
					continue;
				}
				std::int32_t slot_index = -1;
				for (std::size_t slot = 0; slot < planned_dpb_slots.size(); ++slot) {
					if (planned_dpb_slots[slot] == nullptr || !is_current_reference(planned_dpb_slots[slot])) {
						slot_index = static_cast<std::int32_t>(slot);
						break;
					}
				}
				if (slot_index < 0) {
					return std::unexpected(error(Media_Error_Code::INVALID_REFERENCE_STATE,
							"H.265 references exceed the Vulkan DPB slot capacity"));
				}
				planned_dpb_slots[static_cast<std::size_t>(slot_index)] = reference;
				planned_reference_slots.emplace(reference, slot_index);
			}
			std::int32_t output_slot_index =
					output.slot_ >= 0 && planned_dpb_slots[static_cast<std::size_t>(output.slot_)] == &output
							? output.slot_ : -1;
			if (output_slot_index < 0 || is_current_reference(&output)) {
				output_slot_index = -1;
				for (std::size_t slot = 0; slot < planned_dpb_slots.size(); ++slot) {
					if (planned_dpb_slots[slot] == nullptr) {
						output_slot_index = static_cast<std::int32_t>(slot);
						break;
					}
				}
				if (output_slot_index < 0) {
					for (std::size_t slot = 0; slot < planned_dpb_slots.size(); ++slot) {
						if (!is_current_reference(planned_dpb_slots[slot])) {
							output_slot_index = static_cast<std::int32_t>(slot);
							break;
						}
					}
				}
			}
			if (output_slot_index < 0) {
				return std::unexpected(error(Media_Error_Code::INVALID_REFERENCE_STATE,
						"H.265 picture has no evictable Vulkan DPB slot"));
			}
			planned_dpb_slots[static_cast<std::size_t>(output_slot_index)] = &output;

			source_bytes_.clear();
			slice_offsets_.clear();
			slice_offsets_.reserve(frame.slices_.size());
			for (const auto &slice: frame.slices_) {
				if (slice.logical_offset_ < logical_bitstream_begin ||
						slice.logical_offset_ - logical_bitstream_begin > bitstream.size() ||
						slice.nalu_.size > bitstream.size() -
								static_cast<std::size_t>(slice.logical_offset_ - logical_bitstream_begin)) {
					return std::unexpected(error(Media_Error_Code::INVALID_H265_BITSTREAM,
							"H.265 slice [" + std::to_string(slice.logical_offset_) + ", " +
							std::to_string(slice.logical_offset_ + slice.nalu_.size) + ") lies outside source [" +
							std::to_string(logical_bitstream_begin) + ", " +
							std::to_string(logical_bitstream_begin + bitstream.size()) + ")"));
				}
				if (source_bytes_.size() > std::numeric_limits<std::uint32_t>::max()) {
					return std::unexpected(error(Media_Error_Code::UNSUPPORTED_BITSTREAM_FEATURE,
							"H.265 picture bitstream exceeds Vulkan's 32-bit slice offset range"));
				}
				slice_offsets_.push_back(static_cast<std::uint32_t>(source_bytes_.size()));
				source_bytes_.insert(source_bytes_.end(), {0U, 0U, 1U});
				const auto payload = static_cast<std::size_t>(slice.logical_offset_ - logical_bitstream_begin);
				source_bytes_.insert(source_bytes_.end(), bitstream.begin() + payload,
						bitstream.begin() + payload + slice.nalu_.size);
			}
			if (source_bytes_.empty()) {
				return std::unexpected(error(Media_Error_Code::INVALID_H265_BITSTREAM,
						"H.265 frame has an empty Vulkan Video source range"));
			}
			const auto source_size = source_bytes_.size();
			const auto aligned_size = align_up(source_size, bitstream_size_alignment_);
			if (auto result = ensure_bitstream_buffer(submission, aligned_size); !result) {
				return std::unexpected(result.error());
			}
			std::memcpy(submission.bitstream_mapping_, source_bytes_.data(), source_size);
			std::memset(static_cast<std::uint8_t *>(submission.bitstream_mapping_) + source_size, 0,
					static_cast<std::size_t>(aligned_size - source_size));

			StdVideoDecodeH265PictureInfo standard{};
			standard.flags.IrapPicFlag = GST_H265_IS_NAL_TYPE_IRAP(frame.slices_.front().nalu_.type);
			standard.flags.IdrPicFlag = GST_H265_IS_NAL_TYPE_IDR(frame.slices_.front().nalu_.type);
			standard.flags.IsReference = true;
			standard.flags.short_term_ref_pic_set_sps_flag =
					frame.slices_.front().header_.short_term_ref_pic_set_sps_flag;
			standard.sps_video_parameter_set_id = parameter_bundle_->sps_.sps_video_parameter_set_id;
			standard.pps_seq_parameter_set_id = parameter_bundle_->pps_.pps_seq_parameter_set_id;
			standard.pps_pic_parameter_set_id = parameter_bundle_->pps_.pps_pic_parameter_set_id;
			standard.NumDeltaPocsOfRefRpsIdx =
					frame.slices_.front().header_.short_term_ref_pic_sets.NumDeltaPocsOfRefRpsIdx;
			standard.PicOrderCntVal = state.pic_order_cnt_;
			standard.NumBitsForSTRefPicSetInSlice = standard.flags.short_term_ref_pic_set_sps_flag
					? 0U : static_cast<std::uint16_t>(frame.slices_.front().header_.short_term_ref_pic_set_size);
			std::ranges::fill(standard.RefPicSetStCurrBefore, STD_VIDEO_H265_NO_REFERENCE_PICTURE);
			std::ranges::fill(standard.RefPicSetStCurrAfter, STD_VIDEO_H265_NO_REFERENCE_PICTURE);
			std::ranges::fill(standard.RefPicSetLtCurr, STD_VIDEO_H265_NO_REFERENCE_PICTURE);
			const auto lower_reference_set = [&](const auto &source, auto &target) {
				for (std::size_t index = 0; index < source.size() && index < std::size(target); ++index) {
					const auto compact_slot = source[index];
					if (compact_slot == 0xFFU || compact_slot >= reference_images.size() ||
							reference_images[compact_slot] == nullptr) {
						continue;
					}
					target[index] = static_cast<std::uint8_t>(
							planned_reference_slots.at(reference_images[compact_slot]));
				}
			};
			lower_reference_set(state.ref_pic_set_st_curr_before_, standard.RefPicSetStCurrBefore);
			lower_reference_set(state.ref_pic_set_st_curr_after_, standard.RefPicSetStCurrAfter);
			lower_reference_set(state.ref_pic_set_lt_curr_, standard.RefPicSetLtCurr);
			if (std::getenv("CODEC_VIS_TRACE_H265_VULKAN_PARAMS") != nullptr) {
				const auto &slice = frame.slices_.front().header_;
				std::cerr << "H.265 Vulkan frame=" << request.frame_.value_ << " poc=" << state.pic_order_cnt_
						<< " nal=" << unsigned(frame.slices_.front().nalu_.type)
						<< " temporal=" << unsigned(frame.slices_.front().nalu_.temporal_id_plus1)
						<< " reference=" << standard.flags.IsReference << " sps_rps=" << standard.flags.short_term_ref_pic_set_sps_flag
						<< " rps_index=" << unsigned(slice.short_term_ref_pic_set_idx)
						<< " rps_bits=" << standard.NumBitsForSTRefPicSetInSlice
						<< " delta_pocs=" << unsigned(standard.NumDeltaPocsOfRefRpsIdx)
						<< " extra_bits=" << unsigned(parameter_bundle_->pps_.num_extra_slice_header_bits)
						<< " out_slot=" << output_slot_index << " refs=";
				for (const auto &reference: request.references_) {
					const auto slot = reference.reference_slot_;
					std::cerr << ' ' << reference.frame_.value_ << ':' << state.reference_pic_order_cnt_[slot]
							<< ':' << planned_reference_slots.at(reference_images[slot]) << ':' << state.reference_va_flags_[slot];
				}
				std::cerr << '\n';
			}
			VkVideoDecodeH265PictureInfoKHR h265_picture{
					.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR,
					.pStdPictureInfo = &standard,
					.sliceSegmentCount = static_cast<std::uint32_t>(slice_offsets_.size()),
					.pSliceSegmentOffsets = slice_offsets_.data()};

			std::vector<StdVideoDecodeH265ReferenceInfo> standard_references;
			std::vector<VkVideoDecodeH265DpbSlotInfoKHR> dpb_infos;
			std::vector<VkVideoPictureResourceInfoKHR> picture_resources;
			std::vector<VkVideoReferenceSlotInfoKHR> reference_slots;
			standard_references.reserve(request.references_.size());
			dpb_infos.reserve(request.references_.size());
			picture_resources.reserve(request.references_.size());
			reference_slots.reserve(request.references_.size());
			for (const auto &reference: request.references_) {
				const auto slot = reference.reference_slot_;
				if (state.reference_va_flags_[slot] == 0U) { continue; }
				standard_references.push_back(StdVideoDecodeH265ReferenceInfo{
						.flags = {.used_for_long_term_reference = state.reference_long_term_[slot]},
						.PicOrderCntVal = state.reference_pic_order_cnt_[slot]});
				dpb_infos.push_back(VkVideoDecodeH265DpbSlotInfoKHR{
						.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR,
						.pStdReferenceInfo = &standard_references.back()});
				picture_resources.push_back(VkVideoPictureResourceInfoKHR{
						.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
						.codedExtent = {reference.image_.width_, reference.image_.height_}, .baseArrayLayer = 0U,
						.imageViewBinding = reference_images[slot]->view_});
				reference_slots.push_back(VkVideoReferenceSlotInfoKHR{
						.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, .pNext = &dpb_infos.back(),
						.slotIndex = planned_reference_slots.at(reference_images[slot]),
						.pPictureResource = &picture_resources.back()});
			}
			StdVideoDecodeH265ReferenceInfo output_reference{
					.flags = {.unused_for_reference = false}, .PicOrderCntVal = state.pic_order_cnt_};
			VkVideoDecodeH265DpbSlotInfoKHR output_dpb{
					.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR,
					.pStdReferenceInfo = &output_reference};
			VkVideoPictureResourceInfoKHR output_picture{
					.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
					.codedExtent = {request.output_.image_.width_, request.output_.image_.height_},
					.baseArrayLayer = 0U, .imageViewBinding = output.view_};
			VkVideoReferenceSlotInfoKHR output_slot{
					.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, .pNext = &output_dpb,
					.slotIndex = output_slot_index, .pPictureResource = &output_picture};
			std::vector<VkVideoReferenceSlotInfoKHR> begin_slots = reference_slots;
			auto output_binding = output_slot;
			const bool output_already_bound = output.slot_ == output_slot_index &&
					dpb_slots_[static_cast<std::size_t>(output_slot_index)] == &output;
			if (!output_already_bound) { output_binding.slotIndex = -1; }
			begin_slots.push_back(output_binding);
			VkVideoDecodeInfoKHR decode_info{
					.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, .pNext = &h265_picture,
					.srcBuffer = submission.bitstream_buffer_, .srcBufferOffset = 0U, .srcBufferRange = aligned_size,
					.dstPictureResource = output_picture, .pSetupReferenceSlot = &output_slot,
					.referenceSlotCount = static_cast<std::uint32_t>(reference_slots.size()),
					.pReferenceSlots = reference_slots.data()};

			VkCommandBufferBeginInfo command_begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
			if (const auto result = vkBeginCommandBuffer(submission.command_, &command_begin); result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkBeginCommandBuffer(H.265)", result));
			}
			std::vector<VkImageMemoryBarrier2> acquire_barriers;
			const auto add_acquire = [&](const Image_State &image, bool initialized, VkAccessFlags2 access) {
				if (std::ranges::any_of(acquire_barriers, [&](const auto &barrier) { return barrier.image == image.image_; })) {
					return;
				}
				acquire_barriers.push_back(VkImageMemoryBarrier2{
						.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
						.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
						.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
						.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, .dstAccessMask = access,
						.oldLayout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
						.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
						.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image.image_,
						.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U}});
			};
			add_acquire(output, output.initialized_, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
			for (const auto *reference: reference_images) {
				if (reference != nullptr) { add_acquire(*reference, true, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR); }
			}
			VkDependencyInfo acquire_dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.imageMemoryBarrierCount = static_cast<std::uint32_t>(acquire_barriers.size()),
					.pImageMemoryBarriers = acquire_barriers.data()};
			vkCmdPipelineBarrier2(submission.command_, &acquire_dependency);
			VkVideoBeginCodingInfoKHR coding_begin{
					.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR, .videoSession = session_,
					.videoSessionParameters = parameters_,
					.referenceSlotCount = static_cast<std::uint32_t>(begin_slots.size()),
					.pReferenceSlots = begin_slots.data()};
			begin_coding_(submission.command_, &coding_begin);
			if (reset_ || request.independent_random_access_) {
				VkVideoCodingControlInfoKHR control{.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
						.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR};
				control_coding_(submission.command_, &control);
				reset_ = false;
			}
			decode_video_(submission.command_, &decode_info);
			VkVideoEndCodingInfoKHR coding_end{.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
			end_coding_(submission.command_, &coding_end);
			std::vector<VkImageMemoryBarrier2> release_barriers = acquire_barriers;
			for (auto &barrier: release_barriers) {
				barrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
				barrier.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
				barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
				barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			}
			VkDependencyInfo release_dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.imageMemoryBarrierCount = static_cast<std::uint32_t>(release_barriers.size()),
					.pImageMemoryBarriers = release_barriers.data()};
			vkCmdPipelineBarrier2(submission.command_, &release_dependency);
			if (const auto result = vkEndCommandBuffer(submission.command_); result != VK_SUCCESS) {
				return std::unexpected(vk_error("vkEndCommandBuffer(H.265)", result));
			}
			std::map<VkSemaphore, std::uint64_t> wait_points;
			std::map<VkSemaphore, std::uint64_t> signal_points;
			const auto add_point = [](auto &points, av1::Vulkan_Semaphore_Point point) {
				auto [position, inserted] = points.try_emplace(point.semaphore_, point.value_);
				if (!inserted) { position->second = std::max(position->second, point.value_); }
			};
			add_point(wait_points, request.output_.vulkan_safe_to_write_);
			add_point(signal_points, request.output_.vulkan_decode_complete_);
			for (const auto &reference: request.references_) {
				add_point(wait_points, reference.vulkan_acquire_for_read_);
				add_point(signal_points, reference.vulkan_release_after_read_);
			}
			std::vector<VkSemaphore> wait_semaphores;
			std::vector<std::uint64_t> wait_values;
			std::vector<VkPipelineStageFlags> wait_stages;
			std::vector<VkSemaphore> signal_semaphores;
			std::vector<std::uint64_t> signal_values;
			wait_semaphores.reserve(wait_points.size());
			wait_values.reserve(wait_points.size());
			wait_stages.reserve(wait_points.size());
			for (const auto &[semaphore, value]: wait_points) {
				wait_semaphores.push_back(semaphore);
				wait_values.push_back(value);
				wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			}
			signal_semaphores.reserve(signal_points.size());
			signal_values.reserve(signal_points.size());
			for (const auto &[semaphore, value]: signal_points) {
				signal_semaphores.push_back(semaphore);
				signal_values.push_back(value);
			}
			VkTimelineSemaphoreSubmitInfo timeline{
					.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
					.waitSemaphoreValueCount = static_cast<std::uint32_t>(wait_values.size()),
					.pWaitSemaphoreValues = wait_values.data(),
					.signalSemaphoreValueCount = static_cast<std::uint32_t>(signal_values.size()),
					.pSignalSemaphoreValues = signal_values.data()};
			VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
					.waitSemaphoreCount = static_cast<std::uint32_t>(wait_semaphores.size()),
					.pWaitSemaphores = wait_semaphores.data(), .pWaitDstStageMask = wait_stages.data(),
					.commandBufferCount = 1U, .pCommandBuffers = &submission.command_,
					.signalSemaphoreCount = static_cast<std::uint32_t>(signal_semaphores.size()),
					.pSignalSemaphores = signal_semaphores.data()};
			{
				std::scoped_lock queue_lock{*device_.queue_mutex_};
				if (const auto result = vkQueueSubmit(device_.queue_, 1U, &submit, submission.fence_);
						result != VK_SUCCESS) {
					return std::unexpected(vk_error("vkQueueSubmit(H.265 Vulkan Video)", result));
				}
			}
			submission.pending_ = true;
			for (auto *resident: dpb_slots_) {
				if (resident != nullptr && !std::ranges::contains(planned_dpb_slots, resident)) {
					resident->slot_ = -1;
				}
			}
			dpb_slots_ = planned_dpb_slots;
			for (std::size_t slot = 0; slot < dpb_slots_.size(); ++slot) {
				if (dpb_slots_[slot] != nullptr) {
					dpb_slots_[slot]->slot_ = static_cast<std::int32_t>(slot);
				}
			}
			// An *_N picture is non-reference within its temporal sub-layer, but
			// a higher sub-layer may still use it. Preserve its slot association
			// so subsequent bindings retain the driver's temporal prediction state.
			dpb_slots_[static_cast<std::size_t>(output_slot_index)] = &output;
			output.slot_ = output_slot_index;
			output.initialized_ = true;
			return Decode_Frame_Result{.frame_ = request.frame_};
		}
	};

	Vulkan_Video_Decode_Context::Vulkan_Video_Decode_Context(std::unique_ptr<Impl> implementation)
		: implementation_(std::move(implementation)) {}

	Vulkan_Video_Decode_Context::~Vulkan_Video_Decode_Context() = default;

	Result<std::unique_ptr<Vulkan_Video_Decode_Context>>
	Vulkan_Video_Decode_Context::create(H265_Sequence_Info sequence, Vulkan_Device_Context device) {
		if (device.device_ == VK_NULL_HANDLE || device.queue_ == VK_NULL_HANDLE) {
			return std::unexpected(error(Media_Error_Code::INVALID_ARGUMENT,
					"H.265 Vulkan Video context requires the editor's borrowed VkDevice/video queue"));
		}
		auto implementation = std::make_unique<Impl>();
		implementation->sequence_ = std::move(sequence);
		implementation->device_ = std::move(device);
		return std::unique_ptr<Vulkan_Video_Decode_Context>(
				new Vulkan_Video_Decode_Context(std::move(implementation)));
	}

	void Vulkan_Video_Decode_Context::release_source(Source_Id source) { (void) source; }

	Result<Decode_Frame_Result> Vulkan_Video_Decode_Context::decode(
			const Parsed_Decode_Frame &frame, const Frame_Reference_State &state,
			const H265_Frame_Decode_Metadata &metadata, std::span<const std::uint8_t> bitstream,
			std::uint64_t logical_bitstream_begin, Decode_Frame_Request request) {
		return implementation_->decode(frame, state, metadata, bitstream, logical_bitstream_begin, std::move(request));
	}
} // namespace native_video::h265::decoder
