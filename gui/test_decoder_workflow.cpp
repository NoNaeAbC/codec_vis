#include "../codec_gui_image_io.hpp"
#include "decode_energy.hpp"
#include "decoder_runner.hpp"
#include "decoder_ui.hpp"
#include "encoder_param_state.hpp"
#include "raw_image_utils.hpp"
#include "raw_image_conversion.hpp"
#include "preview_decoders.hpp"
#include "test_check.hpp"
#include <cstring>
#include <cstdlib>
extern "C" {
#include <libavutil/log.h>
}
#include <iostream>
#include <thread>
using namespace codec_gui;
using namespace codec_gui::gui;
#define CHECK CODEC_VIS_CHECK
static RawImage pattern(PixelFormat format) {
	RawImage image;
	image.width = 256;
	image.height = 256;
	image.format = format;
	image.color = {.primaries = ColorPrimaries::BT709,
				   .transfer = TransferCharacteristics::SRGB,
				   .matrix = is_rgb(format) ? MatrixCoefficients::Identity : MatrixCoefficients::BT709,
				   .range = is_rgb(format) ? ColorRange::Full : ColorRange::Limited};
	for (int p = 0; p < plane_count(format); ++p) {
		auto &plane = image.planes[p];
		plane.strideBytes = plane_width(image, p) * bytes_per_sample(format);
		plane.bytes.resize(std::size_t(plane.strideBytes) * plane_height(image, p));
		for (int y = 0; y < plane_height(image, p); ++y)
			for (int x = 0; x < plane_width(image, p); ++x)
				put_sample(plane, x, y, bytes_per_sample(format),
						   ((x * 7 + y * 5 + p * 43) % 200 + 20) << (bit_depth(format) - 8), max_sample_value(format));
	}
	return image;
}
static std::vector<EncoderParam> defaults(const EncoderBackend &b) {
	std::vector<EncoderParam> out;
	for (const auto &p : b.queryCapabilities().params)
		out.push_back({p.name, resolved_param_value(AppState{}, b.id, p)});
	return out;
}
static void set(std::vector<EncoderParam> &params, const std::string &name, ParamValue v) {
	for (auto &p : params)
		if (p.name == name) {
			p.value = std::move(v);
			return;
		}
	params.push_back({name, std::move(v)});
}
static std::vector<Action> await_actions(DecoderRunner &runner, ActionKind terminal) {
	std::vector<Action> all;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (std::chrono::steady_clock::now() < deadline) {
		for (auto &action : runner.take_actions()) {
			bool done = action.kind == terminal;
			all.push_back(std::move(action));
			if (done)
				return all;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("decoder worker timed out");
}
int main(int argc, char **argv) {
    if(std::getenv("CODEC_VIS_DECODER_VERBOSE"))av_log_set_level(AV_LOG_VERBOSE);
	if (argc > 1) { // Real imported image, in memory after reading the requested input.
		auto raw = std::make_shared<RawImage>(load_input_image(argv[1]));
		ImageObject item;
		item.decoded = raw;
		item.width = raw->width;
		item.height = raw->height;
		item.pixelFormat = raw->format;
        if(argc>2 && std::string(argv[2])=="--vvenc") {
            const auto backends=initial_encoder_backends();const auto* backend=find_backend(backends,{2});CHECK(backend);
            auto encoded=run_backend_encode(*backend,*raw,defaults(*backend));auto preview=backend->decodePreview(encoded.encoded);CHECK(preview.image);
            item.encoded=encoded.metadata;item.decoded=preview.image;item.pixelFormat=preview.image->format;
            item.encoded->codedPixelFormat=item.pixelFormat;
            std::cout<<"VVenC default encoded "<<encoded.metadata.bytes.size()<<" bytes at "<<item.width<<"x"<<item.height<<'\n';
        }
		auto input = image_decode_input(item);
		int count = 0;
		for (const auto &decoder : image_decoder_candidates(input)) {
			auto result = decoder.decode(*input.bytes);
			bool supported = decoder_output_matches(input, result);
            if(supported && input.codec=="VVC") {
                int maximum=0;const auto& expected=*input.reference;const auto& actual=*result.image;
                for(int p=0;p<plane_count(expected.format);++p)for(int y=0;y<plane_height(expected,p);++y)for(int x=0;x<plane_width(expected,p);++x)
                    maximum=std::max(maximum,std::abs(int(sample_at(expected.planes[p],x,y,bytes_per_sample(expected.format)))-int(sample_at(actual.planes[p],x,y,bytes_per_sample(actual.format)))));
                std::cout<<decoder.info.name<<" maximum sample difference from VVdeC: "<<maximum<<'\n';CHECK(maximum==0);
            }

			std::cout << decoder.info.name << ": " << (supported ? "supported" : result.error) << '\n';
			if (supported)
				++count;
		}
		CHECK(count > 0);
		return 0;
	}
	const auto stats = sample_statistics({1, 2, 3, 100});
	CHECK(stats.count == 4 && stats.mean == 26.5);
	CHECK(std::abs(*stats.standardDeviation - 49.00680224893955) < 1e-9);
	CHECK(std::abs(*stats.standardError - *stats.standardDeviation / 2) < 1e-12);
	CHECK(!sample_statistics({1}).standardError);
	CHECK(sample_statistics({}).count == 0);
	CHECK(std::abs(*DecodeEnergy::delta(95, 5, 100) - 10e-6) < 1e-15);
	CHECK(!DecodeEnergy::delta(100, 5, 100));
	const auto backends = initial_encoder_backends();
    if(const auto* hevc=find_backend(backends,{4});hevc&&hevc->queryCapabilities().snapshot.available){
        auto p3=pattern(PixelFormat::YUV420P10LE);p3.color.primaries=ColorPrimaries::DisplayP3;
        auto encoded=run_backend_encode(*hevc,p3,defaults(*hevc));
        CHECK(encoded.encoded.codedColor&&encoded.encoded.codedColor->primaries==ColorPrimaries::DisplayP3);
        auto preview=hevc->decodePreview(encoded.encoded);CHECK(preview.image);
        CHECK(preview.image->color.primaries==ColorPrimaries::DisplayP3);
        CHECK(raw_image_to_linear_bt2020(*preview.image).width==p3.width);
        auto signaled=encoded.encoded;signaled.codedColor.reset();
        auto actual=decode_libavcodec(signaled,"hevc");CHECK(actual.image);
        CHECK(actual.image->color.primaries==ColorPrimaries::DisplayP3);
        std::cout<<"Display P3 HEVC preview interpretation and independent decoder metadata passed\n";
    }

	ImageObject vp9;
	int cases = 0;
	bool vaapi = false, vulkan = false;
	for (const auto &backend : backends) {
		if (backend.id.value == 6 || backend.id.value == 7 || backend.id.value == 3 ||
			backend.id.value == 21)
			continue;
		if (!backend.queryCapabilities().snapshot.available)
			continue;
		auto params = defaults(backend);
		const auto infos = backend.queryCapabilities().params;
		if (std::any_of(infos.begin(), infos.end(), [](const auto &p) { return p.name == "bit-depth"; }))
			set(params, "bit-depth", std::string{"8"});
		RawImage source = pattern(PixelFormat::YUV420P8);
		try {
			auto encoded = run_backend_encode(backend, source, params);
			auto reference = backend.decodePreview(encoded.encoded);
			CHECK(reference.image);
			ImageObject item;
			item.id = {1};
			item.type = ImageObjectType::EncodedResult;
			item.encoded = encoded.metadata;
			item.decoded = reference.image;
			item.width = reference.image->width;
			item.height = reference.image->height;
			item.pixelFormat = reference.image->format;
			item.encoded->codedPixelFormat = item.pixelFormat;
			auto input = image_decode_input(item);
			int count = 0;
			for (const auto &decoder : image_decoder_candidates(input)) {
				const auto start=std::chrono::steady_clock::now();
				const auto result = decoder.decode(*input.bytes);
				const double total=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
				bool supported = decoder_output_matches(input, result);
				std::cout << backend.name << " -> " << decoder.info.name << ": "
						  << (supported				 ? "supported"
							  : result.error.empty() ? "incompatible pixel format"
													 : result.error)
						  << '\n';
				if (!supported)
					continue;
				++count;
				CHECK(result.codecSeconds && std::isfinite(*result.codecSeconds) && *result.codecSeconds > 0);
				CHECK(*result.codecSeconds <= total);
				if (decoder.info.hardware) {
					vaapi |= decoder.info.id.find("vaapi") != std::string::npos;
					vulkan |= decoder.info.id.find("vulkan") != std::string::npos;
					CHECK(result.codecSeconds && *result.codecSeconds > 0);
				}
			}
			CHECK(count > 0);
			++cases;
			if (backend.id.value == 19)
				vp9 = item;
		} catch (const std::exception &e) {
			std::cerr << "Fixture failed: " << backend.name << ": " << e.what() << '\n';
			return 1;
		}
	}
    const auto* vvenc=find_backend(backends,{2});CHECK(vvenc);
    for(const auto& depth:{"8","10"}) {
        auto params=defaults(*vvenc);set(params,"bit-depth",std::string(depth));set(params,"chroma-subsampling",std::string("420"));
        auto encoded=run_backend_encode(*vvenc,pattern(PixelFormat::YUV420P10LE),params);auto reference=vvenc->decodePreview(encoded.encoded);CHECK(reference.image);
        ImageObject item;item.encoded=encoded.metadata;item.decoded=reference.image;item.width=256;item.height=256;item.pixelFormat=reference.image->format;item.encoded->codedPixelFormat=item.pixelFormat;
        auto input=image_decode_input(item);
        for(const auto& candidate:image_decoder_candidates(input))if(candidate.info.id.find(":vaapi:")!=std::string::npos) {
            auto result=candidate.decode(*input.bytes);
            if(!result.image)throw std::runtime_error("VVenC VA-API regression: "+result.error);
            CHECK(decoder_output_matches(input,result));
            for(int p=0;p<plane_count(item.pixelFormat);++p)for(int y=0;y<plane_height(*reference.image,p);++y)for(int x=0;x<plane_width(*reference.image,p);++x)
                CHECK(sample_at(reference.image->planes[p],x,y,bytes_per_sample(item.pixelFormat))==sample_at(result.image->planes[p],x,y,bytes_per_sample(item.pixelFormat)));
            std::cout<<"VVenC "<<depth<<"-bit VA-API bit-exact regression passed\n";
        }
        ++cases;
    }
	const auto *vpx = find_backend(backends, {19});
	CHECK(vpx);
	for (const auto &depth : {"8", "10", "12"})
		for (const auto &chroma : {"420", "422", "444"}) {
			auto params = defaults(*vpx);
			set(params, "bit-depth", std::string(depth));
			set(params, "chroma-subsampling", std::string(chroma));
			set(params, "lossless", true);
			auto encoded = run_backend_encode(*vpx, pattern(PixelFormat::YUV444P12LE), params);
			auto reference = vpx->decodePreview(encoded.encoded);
			CHECK(reference.image);
			ImageObject item;
			item.id = {42};
			item.encoded = encoded.metadata;
			item.encoded->codedPixelFormat = reference.image->format;
			item.decoded = reference.image;
			item.width = 256;
			item.height = 256;
			item.pixelFormat = reference.image->format;
			const auto input = image_decode_input(item);
			int supported = 0;
			for (const auto &candidate : image_decoder_candidates(input)) {
				auto decoded = candidate.decode(*input.bytes);
				if (!decoder_output_matches(input, decoded))
					continue;
				++supported;
				for (int p = 0; p < plane_count(item.pixelFormat); ++p)
					for (int y = 0; y < plane_height(*reference.image, p); ++y)
						for (int x = 0; x < plane_width(*reference.image, p); ++x)
							CHECK(sample_at(reference.image->planes[p], x, y, bytes_per_sample(item.pixelFormat)) ==
								  sample_at(decoded.image->planes[p], x, y, bytes_per_sample(item.pixelFormat)));
				std::cout << "VP9 " << depth << "-bit " << chroma << " bit-exact: " << candidate.info.name << '\n';
			}
			CHECK(supported > 0);
			++cases;
		}
	CHECK(vp9.encoded);
	AppState state;
	state.images = {vp9};
	const auto establishedPreview = vp9.decoded;
	auto &quality = *state.images[0].encoded;
	quality.metricState = MetricState::Ready;
	quality.metrics = {{.id="psnr-rgb", .label="PSNR RGB", .value=42.25, .unit="dB"},
		{.id="butteraugli", .label="Butteraugli", .value=1.25}};
	quality.psnrRgb = 42.25;
	quality.metricSeconds = 2.5;
	quality.encodeSeconds = 3.25;
	quality.decodeSeconds = .75;
	state.selection.selectedImage = vp9.id;
	state.decoderInspector = true;
	DecoderRunner runner;
	Action show;
	show.kind = ActionKind::ShowDecoderInspector;
	auto updated = update(state, show);
	state = std::move(updated.state);
	CHECK(updated.commands.size() == 1 && updated.commands[0].kind == CommandKind::QueryImageDecoders);
	runner.submit(updated.commands[0], state);
	for (const auto &a : await_actions(runner, ActionKind::DecodersReady))
		state = update(std::move(state), a).state;
	CHECK(!state.images[0].decoder.available.empty());
	Action measure;
	measure.kind = ActionKind::StartDecode;
	measure.image = vp9.id;
	state.images[0].decoder.repetitions = 3;
	updated = update(state, measure);
	state = std::move(updated.state);
	CHECK(updated.commands.size() == 1);
	runner.submit(updated.commands[0], state);
	auto notifications = await_actions(runner, ActionKind::DecodeFinished);
	CHECK(notifications.size() == 4);
	for (const auto &a : notifications)
		state = update(std::move(state), a).state;
	CHECK(!state.images[0].decoder.running);
	CHECK(state.images[0].decoder.history.size() == 1);
	CHECK(state.images[0].decoder.history[0].samples.size() == 3);
	CHECK(state.images[0].previewRevision == vp9.previewRevision);
	CHECK(state.images[0].decoded == establishedPreview);
	const auto &preserved = *state.images[0].encoded;
	CHECK(preserved.metricState == MetricState::Ready && preserved.metrics.size() == 2);
	CHECK(preserved.metrics[0].value == 42.25 && preserved.metrics[1].value == 1.25);
	CHECK(preserved.psnrRgb == 42.25 && preserved.metricSeconds == 2.5);
	CHECK(preserved.encodeSeconds == 3.25 && preserved.decodeSeconds == .75);
    if(std::getenv("CODEC_VIS_TEST_REQUIRE_ENERGY")) {
        const auto& measured=state.images[0].decoder.history.front();CHECK(!measured.energyDomain.empty());double total=0;
        std::cout<<"Energy domain: "<<measured.energyDomain<<'\n';
        for(const auto& sample:measured.samples){CHECK(sample.joules && sample.energySeconds>0);total+=*sample.joules;std::cout<<"Energy sample: "<<*sample.joules<<" J / "<<sample.energySeconds<<" s\n";}
        CHECK(total>0);
    }

	for (const auto &sample : state.images[0].decoder.history[0].samples)
		CHECK(sample.seconds > 0 && sample.codecSeconds && *sample.codecSeconds > 0 &&
			  *sample.codecSeconds <= sample.seconds && sample.error.empty());
	// Every explicit attempt is retained; stale completions cannot change state.
	auto stale = notifications.back();
	stale.decoder.generation = 0;
	const auto preview = state.images[0].decoded;
	state = update(state, stale).state;
	CHECK(state.images[0].decoded == preview);
	updated = update(state, measure);
	state = std::move(updated.state);
	auto cancelCommand = updated.commands[0];
	cancelCommand.value = 1000;
	runner.submit(cancelCommand, state);
	runner.cancel(vp9.id);
	for (const auto &a : await_actions(runner, ActionKind::DecodeFinished))
		state = update(std::move(state), a).state;
	CHECK(state.images[0].decoder.history.size() == 2);
	CHECK(state.images[0].decoder.history.back().canceled);
	CHECK(state.images[0].decoder.history.back().samples.size() < 1000);
	// An explicitly selected absent decoder fails without changing the preview.
	updated = update(state, measure);
	state = std::move(updated.state);
	auto bad = updated.commands[0];
	bad.text = "missing-decoder";
	runner.submit(bad, state);
	for (const auto &a : await_actions(runner, ActionKind::DecodeFinished))
		state = update(std::move(state), a).state;
	CHECK(!state.images[0].decoder.error.empty());
	CHECK(state.images[0].decoded == preview);
	// Real decode errors remain timed attempts, with the old preview retained.
	state.images[0].decoder.selected = "vpx9";
	state.images[0].encoded->bytes = {std::byte{0}};
	updated = update(state, measure);
	state = std::move(updated.state);
	runner.submit(updated.commands[0], state);
	for (const auto &a : await_actions(runner, ActionKind::DecodeFinished))
		state = update(std::move(state), a).state;
	const auto &failure = state.images[0].decoder.history.back();
	CHECK(failure.samples.size() == 1);
	CHECK(!failure.samples[0].error.empty());
	CHECK(failure.samples[0].seconds > 0);
	CHECK(state.images[0].decoded == preview);
	// Re-decoding an import preserves its compressed source for subsequent runs.
	auto imported = std::make_shared<RawImage>(decode_input_image(
		{reinterpret_cast<const unsigned char *>(vp9.encoded->bytes.data()), vp9.encoded->bytes.size()}, "image.ivf"));
	Action loaded;
	loaded.kind = ActionKind::SourceLoaded;
	loaded.sourceLoaded.image = imported;
	updated = update(AppState{}, loaded);
	AppState importedState = std::move(updated.state);
	auto &source = importedState.images.front();
	CHECK(source.sourceCompressed && source.sourceCodec == "VP9");
	auto input = image_decode_input(source);
	auto candidates = image_decoder_candidates(input);
	CHECK(!candidates.empty());
	source.decoder.selected = "vpx9";
	source.decoder.queried = true;
	const ImageId importedId=source.id;
	ImageObject dependent;
	dependent.id=next_image_id(importedState);
	dependent.parents={importedId};
	dependent.decoded=imported;
	dependent.encoded.emplace();
	dependent.encoded->metricState=MetricState::Ready;
	dependent.encoded->metrics={{.id="psnr-rgb",.value=38.5}};
	importedState.images.push_back(dependent);
	Action decodeImport;
	decodeImport.kind = ActionKind::StartDecode;
	decodeImport.image = importedId;
	decodeImport.value = 1;
	updated = update(importedState, decodeImport);
	importedState = std::move(updated.state);
	runner.submit(updated.commands[0], importedState);
	for (const auto &a : await_actions(runner, ActionKind::DecodeFinished))
		importedState = update(std::move(importedState), a).state;
	CHECK(image_decode_input(importedState.images.front()).bytes);
	CHECK(importedState.images.front().previewRevision == 0);
	CHECK(importedState.images.front().decoded == imported);
	CHECK(importedState.images.front().decoder.history.front().samples.front().codecSeconds);
	CHECK(importedState.images.front().decoded->color.primaries == imported->color.primaries);
	CHECK(importedState.images.front().decoded->importNote == imported->importNote);
	CHECK(importedState.images.back().encoded->metricState==MetricState::Ready);
	CHECK(importedState.images.back().encoded->metrics.front().value==38.5);
    auto removing=state;removing.images.front().decoder.running=true;
    Action remove;remove.kind=ActionKind::RemoveImage;remove.image=vp9.id;
    auto removed=update(std::move(removing),remove);
    CHECK(removed.state.images.empty());
    CHECK(std::any_of(removed.commands.begin(),removed.commands.end(),[&](const auto& c){return c.kind==CommandKind::CancelImageDecode && c.image==vp9.id;}));
	std::cout << "Decoder workflow: " << cases << " encoder fixtures; VA-API=" << vaapi << " Vulkan=" << vulkan
			  << "; repetition, cancellation, stale completions, failure preservation and statistics passed\n";
}
