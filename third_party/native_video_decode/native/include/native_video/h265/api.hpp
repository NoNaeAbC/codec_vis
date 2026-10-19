#pragma once

#include "native_video/av1/api.hpp"
#include "native_video/color/hdr10plus.hpp"
#include "native_video/color/dynamic_hdr.hpp"
#include "native_video/encode/api.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace native_video::h265 {
	using av1::Chroma_Sample_Position;
	using av1::Color_Encoding;
	using av1::Color_Primaries;
	using av1::Color_Range;
	using av1::Dma_Buf_Object;
	using av1::Drm_Syncobj_Timeline;
	using av1::Drm_Syncobj_Timeline_Point;
	using av1::Encoded_Byte_Interval;
	using av1::Exported_Dmabuf_Image;
	using av1::Matrix_Coefficients;
	using av1::Media_Error;
	using av1::Media_Error_Code;
	using av1::Preflight_Status;
	using av1::Result;
	using av1::Transfer_Function;
	using av1::Unsupported_Diagnostic;
	using av1::Unsupported_Reason;
	using av1::Vulkan_Decode_Target;
	using av1::Vulkan_Semaphore_Point;
	using av1::Vulkan_Encode_Source;
	using av1::Vulkan_Image_Plane;
	using av1::Vulkan_Timeline_Semaphore_Point;
	using av1::Vulkan_Device_Context;
	using av1::Vulkan_Video_Decode_Profile;

	struct Source_Id {
		std::uint64_t value_                           = 0;
		friend bool   operator==(Source_Id, Source_Id) = default;
	};

	struct Frame_Id {
		std::uint64_t value_                         = 0;
		friend bool   operator==(Frame_Id, Frame_Id) = default;
	};

	struct Decode_Reference_Binding {
		std::uint8_t reference_slot_ = 0;
		Frame_Id     frame_;
	};

	struct Decode_Reference_Frame {
		std::uint8_t               reference_slot_ = 0;
		Frame_Id                   frame_;
		Exported_Dmabuf_Image      image_;
		Drm_Syncobj_Timeline_Point acquire_for_read_;
		Drm_Syncobj_Timeline_Point release_after_read_;
		Vulkan_Semaphore_Point vulkan_acquire_for_read_;
		Vulkan_Semaphore_Point vulkan_release_after_read_;
	};

	struct Decode_Frame_Request {
		Source_Id                               source_;
		Frame_Id                                frame_;
		std::span<const Decode_Reference_Frame> references_;
		Vulkan_Decode_Target                    output_;
		// Cold random access is valid only for an indexed IRAP picture. In this
		// mode the decoder starts with an empty DPB; references retained solely
		// for leading pictures from the preceding open GOP are not supplied.
		bool independent_random_access_ = false;
	};

	struct Decode_Frame_Result {
		Frame_Id frame_;
	};

	class H265_Decode_Context : public av1::Decode_Context {
	public:
		~H265_Decode_Context() override               = default;
		virtual void release_source(Source_Id source) = 0;
	};

	inline constexpr std::uint32_t kInvalid_H265_Frame_Index = std::numeric_limits<std::uint32_t>::max();

	enum class H265_Profile : std::uint8_t {
		MAIN,
		MAIN_10,
		MAIN_12,
		MAIN_STILL_PICTURE,
		MAIN_422_10,
		MAIN_422_12,
		MAIN_444,
		MAIN_444_10,
		MAIN_444_12,
		SCC_MAIN,
		SCC_MAIN_10,
		SCC_MAIN_444,
		SCC_MAIN_444_10,
		UNSUPPORTED_EXTENSION,
	};
	enum class H265_Chroma_Format : std::uint8_t { MONOCHROME = 0, YUV420 = 1, YUV422 = 2, YUV444 = 3 };
	enum class H265_Slice_Type : std::uint8_t { B = 0, P = 1, I = 2 };

	struct H265_Sequence_Info {
		H265_Profile       profile_                       = H265_Profile::MAIN;
		std::uint32_t      max_width_                     = 0;
		std::uint32_t      max_height_                    = 0;
		std::uint8_t       bit_depth_                     = 8;
		std::uint8_t       chroma_bit_depth_              = 8;
		H265_Chroma_Format chroma_format_                 = H265_Chroma_Format::YUV420;
		bool               range_extension_               = false;
		bool               screen_content_                = false;
		bool               high_precision_offsets_        = false;
		bool               extended_precision_processing_ = false;
		Color_Encoding     color_;
	};

	struct Decode_Capabilities {
		bool          main_400_8bit_            = false;
		bool          main_420_8bit_            = false;
		bool          main10_420_8bit_          = false;
		bool          main10_420_10bit_         = false;
		bool          main12_420_12bit_         = false;
		bool          main422_10_422_10bit_     = false;
		bool          main422_12_422_12bit_     = false;
		bool          main444_420_8bit_         = false;
		bool          main444_444_8bit_         = false;
		bool          main444_400_8bit_         = false;
		bool          main444_10_420_10bit_     = false;
		bool          main444_10_444_10bit_     = false;
		bool          main444_12_420_12bit_     = false;
		bool          main444_12_444_12bit_     = false;
		bool          scc_main_420_8bit_        = false;
		bool          scc_main10_420_10bit_     = false;
		bool          scc_main444_444_8bit_     = false;
		bool          scc_main444_10_444_10bit_ = false;
		std::uint32_t max_width_                = 0;
		std::uint32_t max_height_               = 0;
		std::uint32_t max_slice_count_          = 0;
		// Empty means the implementation does not impose a fixed active-source limit.
		std::optional<std::uint32_t> max_active_sources_per_context_;
	};

	struct Preflight_Report {
		Preflight_Status                      status_ = Preflight_Status::SUPPORTED;
		H265_Sequence_Info                    sequence_;
		std::optional<Unsupported_Diagnostic> unsupported_;
	};

	struct H265_Frame_Decode_Metadata {
		color::Dynamic_HDR dynamic_hdr_ = {};
		std::optional<color::Hdr10_Plus> hdr10_plus_ = std::nullopt;
		Encoded_Byte_Interval         bitstream_;
		std::array<std::uint32_t, 15> reference_frame_index_{
				kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index,
				kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index,
				kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index,
				kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index,
				kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index, kInvalid_H265_Frame_Index};
		std::uint16_t   used_reference_mask_        = 0;
		// Subset of used_reference_mask_ which the current picture actually
		// addresses through RefPicSetStCurrBefore/After or RefPicSetLtCurr.
		// Other used slots describe retained virtual-DPB occupancy.
		std::uint16_t   active_reference_mask_      = 0;
		std::uint16_t   unavailable_reference_mask_ = 0;
		H265_Slice_Type primary_slice_type_         = H265_Slice_Type::I;
		bool            irap_                       = false;
		std::uint32_t   visible_x_                  = 0;
		std::uint32_t   visible_y_                  = 0;
		std::uint32_t   visible_width_              = 0;
		std::uint32_t   visible_height_             = 0;
	};

	struct H265_Presentation_Record {
		std::uint32_t decoded_frame_index_ = 0;
	};

	class H265_Bitstream_Index {
	public:
		H265_Bitstream_Index() = default;
		H265_Bitstream_Index(Source_Id source, H265_Sequence_Info sequence,
							 std::vector<H265_Frame_Decode_Metadata> decode_frames,
							 std::vector<H265_Presentation_Record>   presentation)
			: source_(source), sequence_(std::move(sequence)), decode_frames_(std::move(decode_frames)),
			  presentation_(std::move(presentation)) { color::resolve_dynamic_hdr(decode_frames_, presentation_); }

		[[nodiscard]] Source_Id                 source() const noexcept { return source_; }
		[[nodiscard]] const H265_Sequence_Info &sequence() const noexcept { return sequence_; }
		[[nodiscard]] std::size_t               decoded_picture_count() const noexcept { return decode_frames_.size(); }
		[[nodiscard]] std::size_t presentation_frame_count() const noexcept { return presentation_.size(); }
		[[nodiscard]] Frame_Id    decoded_picture_for_presentation(std::size_t presentation_frame) const noexcept {
			return Frame_Id{.value_ = presentation_[presentation_frame].decoded_frame_index_};
		}
		[[nodiscard]] const H265_Frame_Decode_Metadata &unsafe_decode_metadata(Frame_Id frame) const noexcept {
			return decode_frames_[static_cast<std::size_t>(frame.value_)];
		}

	private:
		Source_Id                               source_;
		H265_Sequence_Info                      sequence_;
		std::vector<H265_Frame_Decode_Metadata> decode_frames_;
		std::vector<H265_Presentation_Record>   presentation_;
	};

	struct Decode_Frame_Facts {
		Frame_Id                              frame_;
		Encoded_Byte_Interval                 bitstream_;
		std::uint32_t                         width_            = 0;
		std::uint32_t                         height_           = 0;
		std::uint32_t                         visible_x_        = 0;
		std::uint32_t                         visible_y_        = 0;
		std::uint32_t                         visible_width_    = 0;
		std::uint32_t                         visible_height_   = 0;
		VkFormat                              requested_format_ = VK_FORMAT_UNDEFINED;
		Vulkan_Video_Decode_Profile           video_profile_;
		std::vector<Frame_Id>                 dependencies_;
		std::vector<Decode_Reference_Binding> reference_bindings_;
		std::uint16_t                         active_reference_slot_mask_ = 0;
		std::uint32_t                         minimum_cache_frames_ = 1;
		H265_Slice_Type                       primary_slice_type_   = H265_Slice_Type::I;
		bool                                  irap_                 = false;
	};

	struct H265_Rate_Control {
		std::uint32_t target_bitrate_       = 0;
		std::uint32_t maximum_bitrate_      = 0;
		std::uint32_t target_percentage_    = 100;
		std::uint32_t cpb_size_bits_        = 0;
		std::uint8_t  initial_qp_            = 26;
		std::uint8_t  minimum_qp_            = 1;
		std::uint8_t  maximum_qp_            = 51;
		std::uint8_t  driver_quality_level_  = 0;
	};

	struct H265_Encode_Plan {
		encode::Resolved_Stream_Plan stream_;
		Color_Encoding               output_color_;
		H265_Rate_Control            rate_control_;
		std::uint32_t                intra_period_      = 30;
		std::uint32_t                intra_idr_period_  = 30;
		std::uint32_t                ip_period_         = 1;
		std::uint8_t                 level_idc_         = 120;
		bool                         high_tier_         = false;
		std::uint8_t                 picture_order_count_lsb_bits_ = 16;
		std::uint8_t                 slice_count_       = 1;
		bool                         sample_adaptive_offset_ = true;
		bool                         temporal_mvp_           = true;
		bool                         strong_intra_smoothing_ = true;
		bool                         transform_skip_         = false;
		bool                         constrained_intra_prediction_ = false;
		bool                         palette_mode_           = false;
		bool                         intra_block_copy_       = false;
	};

	struct H265_Encode_Picture {
		Frame_Id                 frame_;
		H265_Slice_Type          type_ = H265_Slice_Type::P;
		std::int32_t             picture_order_count_ = 0;
		bool                     idr_                  = false;
		bool                     reference_            = true;
		std::vector<Frame_Id>    references_l0_;
		std::vector<Frame_Id>    references_l1_;
		std::optional<std::uint8_t> cqp_;
	};

	struct H265_Encoded_Picture {
		Frame_Id                  frame_;
		std::vector<std::uint8_t> annex_b_;
	};

	class H265_Encode_Context {
	public:
		virtual ~H265_Encode_Context() = default;
		[[nodiscard]] virtual Result<H265_Encoded_Picture>
		encode(const Vulkan_Encode_Source &frame, const H265_Encode_Picture &picture) = 0;
		[[nodiscard]] virtual Result<void> release_reference(Frame_Id frame) = 0;
	};

	class H265_Editor_Bitstream_Api {
	public:
		virtual ~H265_Editor_Bitstream_Api() = default;

		[[nodiscard]] virtual Decode_Capabilities decode_capabilities() const = 0;
		[[nodiscard]] virtual encode::Encode_Plan_Restrictions encode_plan_restrictions() const = 0;
		[[nodiscard]] virtual Result<std::unique_ptr<H265_Encode_Context>>
		create_encode_context(const H265_Encode_Plan &plan) = 0;
		[[nodiscard]] virtual Result<std::unique_ptr<H265_Decode_Context>>
												   create_decode_context(const H265_Sequence_Info &sequence)    = 0;
		[[nodiscard]] virtual Result<std::unique_ptr<H265_Decode_Context>>
		create_vulkan_decode_context(const H265_Sequence_Info &sequence, Vulkan_Device_Context device) = 0;
		[[nodiscard]] virtual Result<Preflight_Report> preflight_decode(std::span<const std::uint8_t> bitstream)    = 0;
		[[nodiscard]] virtual Result<H265_Bitstream_Index> index_bitstream(std::span<const std::uint8_t> bitstream) = 0;
		[[nodiscard]] virtual Result<H265_Bitstream_Index> index_borrowed_bitstream(
				std::span<const av1::Borrowed_Bitstream_Extent> extents) = 0;
		[[nodiscard]] virtual Result<H265_Bitstream_Index> index_borrowed_hevc_samples(
				std::span<const std::uint8_t> decoder_configuration,
				std::span<const av1::Borrowed_Bitstream_Extent> access_units,
				std::uint8_t nal_length_size) = 0;
		[[nodiscard]] virtual Result<void> bind_borrowed_bitstream(
				Source_Id source, std::span<const av1::Borrowed_Bitstream_Extent> extents) = 0;
		[[nodiscard]] virtual Result<av1::Compressed_Source_Storage>
		compressed_source_storage(Source_Id source) const = 0;
		[[nodiscard]] virtual Result<void>                 release_source(Source_Id source)                         = 0;
		[[nodiscard]] virtual Result<Decode_Frame_Facts>   query_decode_frame(Source_Id source, Frame_Id frame)     = 0;
		[[nodiscard]] virtual Result<Decode_Frame_Result>  decode_frame(H265_Decode_Context &context,
																		Decode_Frame_Request request)               = 0;
	};

	[[nodiscard]] std::unique_ptr<H265_Editor_Bitstream_Api> open_h265_editor_bitstream_api();
} // namespace native_video::h265
