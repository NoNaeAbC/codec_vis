#include "metrics.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

extern "C" {
#include <libvmaf/libvmaf.h>
}

namespace codec_gui::gui {
namespace {

QualityMetricRecord metric_record(
	std::string id,
	std::string label,
	std::optional<double> value,
	std::string unit,
	bool higherIsBetter,
	std::string unavailableReason = {}
) {
	QualityMetricRecord record;
	record.id = std::move(id);
	record.label = std::move(label);
	record.value = value;
	record.unit = std::move(unit);
	record.higherIsBetter = higherIsBetter;
	record.unavailableReason = std::move(unavailableReason);
	return record;
}

double psnr_from_mse(double mse, double peak) {
	if (mse == 0.0) {
		return std::numeric_limits<double>::infinity();
	}
	return 10.0 * std::log10((peak * peak) / mse);
}

std::optional<VmafPixelFormat> vmaf_pixel_format(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV420P12LE:
		case PixelFormat::YUV420P14LE:
			return VMAF_PIX_FMT_YUV420P;
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV422P12LE:
		case PixelFormat::YUV422P14LE:
			return VMAF_PIX_FMT_YUV422P;
		case PixelFormat::YUV444P8:
		case PixelFormat::YUV444P10LE:
		case PixelFormat::YUV444P12LE:
		case PixelFormat::YUV444P14LE:
		case PixelFormat::YUV444P16LE:
			return VMAF_PIX_FMT_YUV444P;
		case PixelFormat::Gray8:
		case PixelFormat::Gray10LE:
		case PixelFormat::Gray12LE:
		case PixelFormat::Gray14LE:
			return VMAF_PIX_FMT_YUV400P;
		default:
			break;
	}
	return std::nullopt;
}

int metric_bit_depth(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV444P8:
		case PixelFormat::Gray8:
			return 8;
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV444P10LE:
		case PixelFormat::Gray10LE:
			return 10;
		case PixelFormat::YUV420P12LE:
		case PixelFormat::YUV422P12LE:
		case PixelFormat::YUV444P12LE:
		case PixelFormat::Gray12LE:
			return 12;
		case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray14LE:
			return 14;
		case PixelFormat::YUV444P16LE:
			return 16;
		default:
			break;
	}
	return 8;
}

struct VmafPictureDeleter {
	void operator()(VmafPicture* picture) const noexcept {
		if (picture != nullptr) {
			(void)vmaf_picture_unref(picture);
			delete picture;
		}
	}
};

using VmafPicturePtr = std::unique_ptr<VmafPicture, VmafPictureDeleter>;

VmafPicturePtr make_vmaf_picture(const RawImage& image) {
	const std::optional<VmafPixelFormat> pixFmt = vmaf_pixel_format(image.format);
	if (!pixFmt) {
		throw std::runtime_error("unsupported pixel format for libvmaf");
	}
	auto picture = VmafPicturePtr(new VmafPicture{});
	const int ret = vmaf_picture_alloc(picture.get(), *pixFmt, static_cast<unsigned>(metric_bit_depth(image.format)), static_cast<unsigned>(image.width), static_cast<unsigned>(image.height));
	if (ret < 0) {
		throw std::runtime_error("libvmaf picture allocation failed");
	}

	const int planes = plane_count(image.format);
	const int bps = bytes_per_sample(image.format);
	for (int plane = 0; plane < planes; ++plane) {
		if (!plane_available(image, plane)) {
			throw std::runtime_error("plane buffer is incomplete");
		}
		const int w = plane_width(image, plane);
		const int h = plane_height(image, plane);
		const std::size_t rowBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(bps);
		const uint8_t* src = image.planes[plane].bytes.data();
		auto* dst = static_cast<uint8_t*>(picture->data[plane]);
		for (int y = 0; y < h; ++y) {
			std::memcpy(
				dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(picture->stride[plane]),
				src + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.planes[plane].strideBytes),
				rowBytes
			);
		}
	}
	return picture;
}

struct VmafContextDeleter {
	void operator()(VmafContext* context) const noexcept {
		if (context != nullptr) {
			(void)vmaf_close(context);
		}
	}
};

using VmafContextPtr = std::unique_ptr<VmafContext, VmafContextDeleter>;

std::optional<double> compute_vmaf_feature(
	const RawImage& reference,
	const RawImage& candidate,
	std::string_view feature,
	std::span<const std::string_view> scoreNames,
	std::string& error
) {
	try {
		VmafContext* rawContext = nullptr;
		VmafConfiguration config{};
		config.log_level = VMAF_LOG_LEVEL_NONE;
		config.n_threads = 1;
		config.n_subsample = 0;
		if (const int ret = vmaf_init(&rawContext, config); ret < 0) {
			error = "libvmaf initialization failed";
			return std::nullopt;
		}
		VmafContextPtr context{rawContext};
		if (const int ret = vmaf_use_feature(context.get(), std::string(feature).c_str(), nullptr); ret < 0) {
			error = "libvmaf feature unavailable: " + std::string(feature);
			return std::nullopt;
		}

			VmafPicturePtr ref = make_vmaf_picture(reference);
			VmafPicturePtr dist = make_vmaf_picture(candidate);
			VmafPicture* rawRef = ref.get();
			VmafPicture* rawDist = dist.get();
			if (const int ret = vmaf_read_pictures(context.get(), rawRef, rawDist, 0); ret < 0) {
				error = "libvmaf could not read pictures";
				return std::nullopt;
			}
			ref.release();
			dist.release();
			std::unique_ptr<VmafPicture> refStruct(rawRef);
			std::unique_ptr<VmafPicture> distStruct(rawDist);
			if (const int ret = vmaf_read_pictures(context.get(), nullptr, nullptr, 0); ret < 0) {
				error = "libvmaf flush failed";
				return std::nullopt;
			}

		for (std::string_view scoreName : scoreNames) {
			double score = 0.0;
			if (vmaf_feature_score_at_index(context.get(), std::string(scoreName).c_str(), &score, 0) == 0) {
				return score;
			}
		}
		error = "libvmaf did not produce score for " + std::string(feature);
		return std::nullopt;
	} catch (const std::exception& e) {
		error = e.what();
		return std::nullopt;
	}
}

void append_vmaf_metric(
	MetricResult& result,
	const RawImage& reference,
	const RawImage& candidate,
	std::string id,
	std::string label,
	std::string_view feature,
	std::initializer_list<std::string_view> scoreNames
) {
	if (feature == "float_ms_ssim" && (reference.width < 256 || reference.height < 256)) {
		result.metrics.push_back(metric_record(std::move(id), std::move(label), std::nullopt, {}, true, "image is too small for MS-SSIM"));
		return;
	}
	std::string error;
	const std::optional<double> value = compute_vmaf_feature(reference, candidate, feature, std::span<const std::string_view>(scoreNames.begin(), scoreNames.size()), error);
	result.metrics.push_back(metric_record(std::move(id), std::move(label), value, {}, true, value ? std::string{} : error));
}

} // namespace

MetricResult compute_psnr(const RawImage& reference, const RawImage& candidate) {
	MetricResult result;
	if (reference.width != candidate.width || reference.height != candidate.height) {
		result.unavailableReason = "image dimensions differ";
		result.metrics.push_back(metric_record("psnr-y", "PSNR-Y", std::nullopt, "dB", true, result.unavailableReason));
		result.metrics.push_back(metric_record("psnr-all", "PSNR all", std::nullopt, "dB", true, result.unavailableReason));
		return result;
	}
	if (reference.format != candidate.format) {
		result.unavailableReason = "pixel formats differ";
		result.metrics.push_back(metric_record("psnr-y", "PSNR-Y", std::nullopt, "dB", true, result.unavailableReason));
		result.metrics.push_back(metric_record("psnr-all", "PSNR all", std::nullopt, "dB", true, result.unavailableReason));
		return result;
	}
	const int planes = plane_count(reference.format);
	const int bps = bytes_per_sample(reference.format);
	const double peak = max_sample_value(reference.format);
	double allSquaredError = 0.0;
	uint64_t allSamples = 0;

	for (int plane = 0; plane < planes; ++plane) {
		if (!plane_available(reference, plane) || !plane_available(candidate, plane)) {
			result.unavailableReason = "plane buffer is incomplete";
			result.metrics.push_back(metric_record("psnr-y", "PSNR-Y", std::nullopt, "dB", true, result.unavailableReason));
			result.metrics.push_back(metric_record("psnr-all", "PSNR all", std::nullopt, "dB", true, result.unavailableReason));
			return result;
		}
		const int w = plane_width(reference, plane);
		const int h = plane_height(reference, plane);
		double squaredError = 0.0;
		uint64_t samples = 0;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const double diff = static_cast<double>(sample_at(reference.planes[plane], x, y, bps)) -
				                    static_cast<double>(sample_at(candidate.planes[plane], x, y, bps));
				squaredError += diff * diff;
				++samples;
			}
		}
		if (plane == 0 && samples > 0) {
			result.psnrY = psnr_from_mse(squaredError / static_cast<double>(samples), peak);
		}
		allSquaredError += squaredError;
		allSamples += samples;
	}
	if (allSamples > 0) {
		result.psnrAll = psnr_from_mse(allSquaredError / static_cast<double>(allSamples), peak);
	}
	result.metrics.push_back(metric_record("psnr-y", "PSNR-Y", result.psnrY, "dB", true));
	result.metrics.push_back(metric_record("psnr-all", "PSNR all", result.psnrAll, "dB", true));
	return result;
}

MetricResult compute_quality_metrics(const RawImage& reference, const RawImage& candidate) {
	if (reference.width != candidate.width || reference.height != candidate.height) {
		return compute_psnr(reference, candidate);
	}
	if (reference.color.primaries != candidate.color.primaries ||
	    reference.color.transfer != candidate.color.transfer ||
	    reference.color.matrix != candidate.color.matrix ||
	    reference.color.range != candidate.color.range) {
		MetricResult result;
		result.unavailableReason = "color descriptions differ; an explicit comparison color transform is required";
		for (const auto& [id, label, unit] : {
			std::tuple{"psnr-y", "PSNR-Y", "dB"},
			std::tuple{"psnr-all", "PSNR all", "dB"},
			std::tuple{"psnr-hvs", "PSNR-HVS", "dB"},
			std::tuple{"ssim", "SSIM", ""},
			std::tuple{"ms-ssim", "MS-SSIM", ""},
			std::tuple{"ciede2000", "CIEDE2000", ""},
		}) {
			result.metrics.push_back(metric_record(id, label, std::nullopt, unit, std::string_view{id} != "ciede2000", result.unavailableReason));
		}
		return result;
	}
	const int comparisonDepth = std::max(metric_bit_depth(reference.format), metric_bit_depth(candidate.format));
	const PixelFormat comparisonFormat = pixel_format_for(comparisonDepth, true);
	const RawImage normalizedReference = convert_raw_image_format(reference, comparisonFormat);
	const RawImage normalizedCandidate = convert_raw_image_format(candidate, comparisonFormat);
	MetricResult result = compute_psnr(normalizedReference, normalizedCandidate);
	if (!result.unavailableReason.empty()) {
		const std::string reason = result.unavailableReason;
		result.metrics.push_back(metric_record("psnr-hvs", "PSNR-HVS", std::nullopt, "dB", true, reason));
		result.metrics.push_back(metric_record("ssim", "SSIM", std::nullopt, {}, true, reason));
		result.metrics.push_back(metric_record("ms-ssim", "MS-SSIM", std::nullopt, {}, true, reason));
		result.metrics.push_back(metric_record("ciede2000", "CIEDE2000", std::nullopt, {}, false, reason));
		return result;
	}

	append_vmaf_metric(result, normalizedReference, normalizedCandidate, "psnr-hvs", "PSNR-HVS", "psnr_hvs", {"psnr_hvs_y", "psnr_hvs"});
	append_vmaf_metric(result, normalizedReference, normalizedCandidate, "ssim", "SSIM", "float_ssim", {"float_ssim", "ssim"});
	append_vmaf_metric(result, normalizedReference, normalizedCandidate, "ms-ssim", "MS-SSIM", "float_ms_ssim", {"float_ms_ssim", "ms_ssim"});
	append_vmaf_metric(result, normalizedReference, normalizedCandidate, "ciede2000", "CIEDE2000", "ciede", {"ciede2000", "ciede"});
	return result;
}

const QualityMetricRecord* metric_by_id(const EncodedMetadata& metadata, std::string_view id) {
	const auto it = std::find_if(metadata.metrics.begin(), metadata.metrics.end(), [id](const QualityMetricRecord& metric) {
		return metric.id == id;
	});
	return it == metadata.metrics.end() ? nullptr : &*it;
}

const QualityMetricRecord* primary_metric(const EncodedMetadata& metadata) {
	for (std::string_view id : {"ms-ssim", "ssim", "psnr-hvs", "psnr-y"}) {
		if (const QualityMetricRecord* metric = metric_by_id(metadata, id); metric != nullptr && metric->value) {
			return metric;
		}
	}
	for (const QualityMetricRecord& metric : metadata.metrics) {
		if (metric.value) {
			return &metric;
		}
	}
	return metadata.metrics.empty() ? nullptr : &metadata.metrics.front();
}

} // namespace codec_gui::gui
