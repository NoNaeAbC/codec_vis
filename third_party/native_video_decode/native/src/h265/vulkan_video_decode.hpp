#pragma once

#include "h265/decoder_internal.hpp"

#include <memory>

namespace native_video::h265::decoder {
	class Vulkan_Video_Decode_Context final : public H265_Decode_Context {
	public:
		Vulkan_Video_Decode_Context(const Vulkan_Video_Decode_Context &) = delete;
		Vulkan_Video_Decode_Context &operator=(const Vulkan_Video_Decode_Context &) = delete;
		~Vulkan_Video_Decode_Context() override;

		[[nodiscard]] static Result<std::unique_ptr<Vulkan_Video_Decode_Context>>
		create(H265_Sequence_Info sequence, Vulkan_Device_Context device);

		void release_source(Source_Id source) override;

		[[nodiscard]] Result<Decode_Frame_Result> decode(
				const Parsed_Decode_Frame &frame, const Frame_Reference_State &state,
				const H265_Frame_Decode_Metadata &metadata, std::span<const std::uint8_t> bitstream,
				std::uint64_t logical_bitstream_begin,
				Decode_Frame_Request request);

	private:
		struct Impl;
		explicit Vulkan_Video_Decode_Context(std::unique_ptr<Impl> implementation);
		std::unique_ptr<Impl> implementation_;
	};
} // namespace native_video::h265::decoder
