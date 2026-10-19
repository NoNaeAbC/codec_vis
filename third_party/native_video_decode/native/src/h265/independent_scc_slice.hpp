#pragma once

// HEVC SCC omits prediction-weight flags for the current picture. GStreamer's
// reader consumes those flags and loses alignment on weighted P IDR slices.
// Read the independent IDR case with a sole current-picture reference here;
// retain the installed reader for headers with other reference or tile syntax.
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth265parser.h>

#include <cstdint>
#include <stdexcept>
namespace native_video::h265::decoder {
	struct Independent_Scc_Bits {
		const uint8_t *data;
		unsigned       size, byte = 0, bit = 8, zeros = 0, epb = 0;
		uint8_t        current = 0;
		unsigned       read(unsigned n) {
			if (n > 32) throw std::runtime_error("SCC slice read exceeds 32 bits");
			unsigned v = 0;
			while (n--) {
				if (bit == 8) {
					if (byte >= size) throw std::runtime_error("truncated header");
					current = data[byte++];
					if (zeros >= 2 && current == 3) {
						if (byte >= size || data[byte] > 3) throw std::runtime_error("invalid EPB");
						epb++;
						zeros   = 0;
						current = data[byte++];
					}
					zeros = current == 0 ? zeros + 1 : 0;
					bit   = 0;
				}
				v = (v << 1) | ((current >> (7 - bit++)) & 1);
			}
			return v;
		}
		unsigned ue(unsigned max) {
			unsigned n = 0;
			while (!read(1)) {
				if (++n > 30) throw std::runtime_error("Exp-Golomb overflow");
			}
			unsigned v = ((1u << n) - 1) + read(n);
			if (v > max) throw std::runtime_error("SCC slice value exceeds syntax range");
			return v;
		}
		int se(int low, int high) {
			unsigned u = ue(100000);
			int      v = (u & 1) ? int((u + 1) / 2) : -int(u / 2);
			if (v < low || v > high) throw std::runtime_error("signed header range");
			return v;
		}
		unsigned position() const { return byte * 8 - (8 - bit); }
		void     align() {
			if (read(1) != 1) throw std::runtime_error("alignment one");
			while (bit != 8)
				if (read(1) != 0) throw std::runtime_error("alignment zero");
		}
	};
	inline GstH265ParserResult parse_independent_scc_slice(GstH265Parser *parser, GstH265NalUnit *n,
														   GstH265SliceHdr *result) {
		try {
			if (!GST_H265_IS_NAL_TYPE_IDR(n->type) || n->layer_id || n->size <= n->header_bytes)
				return GST_H265_PARSER_ERROR;
			Independent_Scc_Bits b{n->data + n->offset + n->header_bytes, n->size - n->header_bytes};
			GstH265SliceHdr      h{};
			h.first_slice_segment_in_pic_flag = b.read(1);
			if (!h.first_slice_segment_in_pic_flag) return GST_H265_PARSER_ERROR;
			h.no_output_of_prior_pics_flag = b.read(1);
			h.pps_id                       = b.ue(GST_H265_MAX_PPS_COUNT - 1);
			auto *p                        = &parser->pps[h.pps_id];
			auto *s                        = p->sps;
			if (!p->valid || !s || !s->valid || !p->pps_scc_extension_params.pps_curr_pic_ref_enabled_flag)
				return GST_H265_PARSER_ERROR;
			// This reader handles independent slices without tile entry points.
			if (p->tiles_enabled_flag || p->entropy_coding_sync_enabled_flag) return GST_H265_PARSER_ERROR;
			h.pps                                    = p;
			h.pic_output_flag                        = 1;
			h.collocated_from_l0_flag                = 1;
			h.deblocking_filter_disabled_flag        = p->deblocking_filter_disabled_flag;
			h.beta_offset_div2                       = p->beta_offset_div2;
			h.tc_offset_div2                         = p->tc_offset_div2;
			h.loop_filter_across_slices_enabled_flag = p->loop_filter_across_slices_enabled_flag;
			b.read(p->num_extra_slice_header_bits);
			h.type = b.ue(2);
			if (h.type != GST_H265_P_SLICE) return GST_H265_PARSER_ERROR;
			if (p->output_flag_present_flag) h.pic_output_flag = b.read(1);
			if (s->separate_colour_plane_flag) h.colour_plane_id = b.read(2);
			if (s->sample_adaptive_offset_enabled_flag) {
				h.sao_luma_flag = b.read(1);
				if (s->chroma_array_type) h.sao_chroma_flag = b.read(1);
			}
			h.num_ref_idx_active_override_flag = b.read(1);
			h.num_ref_idx_l0_active_minus1 =
					h.num_ref_idx_active_override_flag ? b.ue(14) : p->num_ref_idx_l0_default_active_minus1;
			if (h.num_ref_idx_l0_active_minus1 != 0) return GST_H265_PARSER_ERROR;
			h.NumPocTotalCurr = 1; // Current-picture reference contributes to the reference list.
			if (p->cabac_init_present_flag) h.cabac_init_flag = b.read(1);
			if (p->weighted_pred_flag) {
				h.pred_weight_table.luma_log2_weight_denom = b.ue(7);
				if (s->chroma_array_type)
					h.pred_weight_table.delta_chroma_log2_weight_denom =
							b.se(-h.pred_weight_table.luma_log2_weight_denom,
								 7 - h.pred_weight_table.luma_log2_weight_denom);
			}
			// The sole reference is the current picture: weight flags and offsets are inferred zero.
			h.five_minus_max_num_merge_cand = b.ue(4);
			if (s->sps_scc_extension_params.motion_vector_resolution_control_idc == 2)
				h.use_integer_mv_flag = b.read(1);
			h.qp_delta = b.se(-87, 77);
			if (p->slice_chroma_qp_offsets_present_flag) {
				h.cb_qp_offset = b.se(-12, 12);
				h.cr_qp_offset = b.se(-12, 12);
			}
			if (p->pps_scc_extension_params.pps_slice_act_qp_offsets_present_flag) {
				h.slice_act_y_qp_offset  = b.se(-12, 12);
				h.slice_act_cb_qp_offset = b.se(-12, 12);
				h.slice_act_cr_qp_offset = b.se(-12, 12);
			}
			if (p->pps_extension_params.chroma_qp_offset_list_enabled_flag)
				h.cu_chroma_qp_offset_enabled_flag = b.read(1);
			if (p->deblocking_filter_override_enabled_flag) h.deblocking_filter_override_flag = b.read(1);
			if (h.deblocking_filter_override_flag) {
				h.deblocking_filter_disabled_flag = b.read(1);
				if (!h.deblocking_filter_disabled_flag) {
					h.beta_offset_div2 = b.se(-6, 6);
					h.tc_offset_div2   = b.se(-6, 6);
				}
			}
			if (p->loop_filter_across_slices_enabled_flag &&
				(h.sao_luma_flag || h.sao_chroma_flag || !h.deblocking_filter_disabled_flag))
				h.loop_filter_across_slices_enabled_flag = b.read(1);
			if (p->slice_segment_header_extension_present_flag) {
				unsigned length = b.ue(n->size - n->header_bytes);
				while (length--) b.read(8);
			}
			b.align();
			h.header_size                  = b.position();
			h.n_emulation_prevention_bytes = b.epb;
			*result                        = h;
			return GST_H265_PARSER_OK;
		} catch (const std::runtime_error &) { return GST_H265_PARSER_ERROR; }
	}
} // namespace native_video::h265::decoder
