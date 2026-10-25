// Native framework integration fixture. All rows/plots are explicitly synthetic.
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"
#include "render_vulkan.hpp"
#include "test_check.hpp"
#include "text_atlas.hpp"
#include "ui_framework.hpp"
#include "wayland_window.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>
using namespace codec_gui;
using namespace codec_gui::gui;
using namespace codec_gui::gui::ui;
#define CHECK CODEC_VIS_CHECK
static ImageObject fixture_image() {
	auto raw   = std::make_shared<RawImage>();
	raw->width = raw->height = 256;
	raw->format              = PixelFormat::RGBP8;
	raw->color = {ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::Identity, ColorRange::Full,
				  std::nullopt};
	for (int p = 0; p < 3; ++p) {
		raw->planes[p].strideBytes = 256;
		raw->planes[p].bytes.resize(65536);
		for (int y = 0; y < 256; ++y)
			for (int x = 0; x < 256; ++x)
				raw->planes[p].bytes[y * 256 + x] =
						static_cast<unsigned char>(p == 0   ? x
												   : p == 1 ? y
															: ((x / 16 + y / 16) % 2) * 255);
	}
	ImageObject image;
	image.id      = {1};
	image.decoded = raw;
	image.width = image.height = 256;
	image.pixelFormat          = raw->format;
	return image;
}
static void verify_renderer(VulkanRenderer &renderer, TextShaper &shaper, TextAtlasCache &cache) {
	std::vector<DrawCommand> commands;
	auto                     box = [&](Rect r, Color color, int layer = 0) {
		DrawCommand c;
		c.rect  = r;
		c.color = color;
		c.layer = layer;
		commands.push_back(c);
	};
	box({0, 0, 2880, 1800}, {0, 0, 0, 1}, -10);
	DrawCommand parent;
	parent.kind = DrawCommandKind::ScissorBegin;
	parent.rect = {20, 20, 100, 100};
	commands.push_back(parent);
	auto child = parent;
	child.rect = {80, 80, 100, 100};
	commands.push_back(child);
	box({0, 0, 200, 200}, {1, 0, 0, 1});
	DrawCommand end;
	end.kind = DrawCommandKind::ScissorEnd;
	commands.push_back(end);
	commands.push_back(end);
	box({200, 20, 60, 60}, {0, 1, 0, 1}, 7);
	box({200, 20, 60, 60}, {1, 0, 0, 1}, 2);
	DrawCommand label;
	label.kind  = DrawCommandKind::Text;
	label.rect  = {300, 20, 200, 32};
	label.color = {1, 1, 1, 1};
	label.text  = "MMMMM";
	commands.push_back(label);
	box(label.rect, {0, 0, 1, 1}); // Must cover the earlier text, even within one layer.
	DrawCommand image;
	image.kind  = DrawCommandKind::Image;
	image.rect  = {600, 20, 80, 80};
	image.image = {1};
	image.color = {1, 1, 1, 1};
	commands.push_back(image);
	box({620, 40, 40, 40}, {1, 0, 0, 1});
	DrawCommand line;
	line.kind        = DrawCommandKind::Line;
	line.rect        = {800, 20, 0, 0};
	line.endPoint    = {900, 120};
	line.strokeWidth = 6;
	line.color       = {1, 1, 0, 1};
	commands.push_back(line);
	DrawCommand circle;
	circle.kind  = DrawCommandKind::Circle;
	circle.rect  = {950, 20, 80, 80};
	circle.color = {1, 0, 1, 1};
	commands.push_back(circle);
	DrawCommand rounded;
	rounded.kind   = DrawCommandKind::RoundedRect;
	rounded.rect   = {1100, 20, 80, 80};
	rounded.radius = 20;
	rounded.color  = {1, 1, 1, 1};
	commands.push_back(rounded);
	box({1220, 20, 60, 60}, {1, 1, 1, .5f});
	for (int i = 0; i < 10000; ++i) box({float(i % 100) * 6, 400 + float(i / 100) * 6, 4, 4}, {.2f, .4f, .6f, 1});
	const auto        &atlas = cache.build(commands, shaper);
	std::vector<Point> positions{{100, 100}, {150, 100}, {90, 30},  {220, 40},  {640, 60},  {850, 70},
								 {825, 70},  {990, 60},  {951, 21}, {1140, 60}, {1100, 20}, {1240, 40}};
	// Pick an actually opaque texel in an earlier glyph. A geometry/text split
	// renderer would incorrectly leave it white on top of the later blue box.
	bool found = false;
	for (const auto &q: atlas.quads)
		if (q.commandIndex == 7 || !found) {
			for (int y = 0; y < int(q.rect.h) && !found; ++y)
				for (int x = 0; x < int(q.rect.w) && !found; ++x) {
					int ax = int((q.uv.x + (x + .5f) / q.rect.w * q.uv.w) * atlas.width),
						ay = int((q.uv.y + (y + .5f) / q.rect.h * q.uv.h) * atlas.height);
					if (ax >= 0 && ay >= 0 && ax < atlas.width && ay < atlas.height &&
						atlas.alpha[std::size_t(ay) * atlas.width + ax] > 240) {
						positions.push_back({q.rect.x + x + .5f, q.rect.y + y + .5f});
						found = true;
					}
				}
			if (found) break;
		}
	CHECK(found);
	auto samples = renderer.render_draw_list_samples(commands, &atlas, positions);
	CHECK(samples.size() == positions.size());
	auto near = [](RenderPixel p, int r, int g, int b) {
		const float rr=.6274039f*r+.3292830f*g+.0433131f*b;
        const float gg=.0690973f*r+.9195404f*g+.0113623f*b;
        const float bb=.0163914f*r+.0880133f*g+.8955953f*b;
        return std::abs(float(p.r)-rr)<=4 && std::abs(float(p.g)-gg)<=4 && std::abs(float(p.b)-bb)<=4;
	};
	CHECK(near(samples[0], 255, 0, 0));
	CHECK(near(samples[1], 0, 0, 0));
	CHECK(near(samples[2], 0, 0, 0));
	CHECK(near(samples[3], 0, 255, 0));
	CHECK(near(samples[4], 255, 0, 0));
	CHECK(near(samples[5], 255, 255, 0));
	CHECK(near(samples[6], 0, 0, 0));
	CHECK(near(samples[7], 255, 0, 255));
	CHECK(near(samples[8], 0, 0, 0));
	CHECK(near(samples[9], 255, 255, 255));
	CHECK(near(samples[10], 0, 0, 0));
	CHECK(samples[11].r > 120 && samples[11].r < 195 && samples[11].r == samples[11].g &&
		  samples[11].g == samples[11].b);
	CHECK(near(samples.back(), 0, 0, 255));
	CHECK(renderer.statistics().instances > 10000 && renderer.statistics().dispatches < 20);
	auto rasterized = cache.rasterized_glyph_count();
	cache.build(commands, shaper);
	CHECK(cache.rasterized_glyph_count() == rasterized);
	renderer.render_draw_list(commands, &cache.atlas());
	CHECK(renderer.statistics().textUploadBytes == 0);
	auto high    = std::make_shared<RawImage>();
	high->width  = 2;
	high->height = 1;
	high->format = PixelFormat::RGBP16LE;
	high->color  = {.primaries = ColorPrimaries::BT2020,
					.transfer  = TransferCharacteristics::Linear,
					.matrix    = MatrixCoefficients::Identity,
					.range     = ColorRange::Full};
	for (int p = 0; p < 3; ++p) {
		high->planes[p].strideBytes = 4;
		high->planes[p].bytes.resize(4);
		put_sample(high->planes[p], 0, 0, 2, 32768, 65535);
		put_sample(high->planes[p], 1, 0, 2, 32769, 65535);
	}
	ImageObject precise;
	precise.id          = {200};
	precise.decoded     = high;
	precise.width       = 2;
	precise.height      = 1;
	precise.pixelFormat = high->format;
	std::vector<ImageObject> history(100, precise);
	for (std::size_t i = 0; i < history.size(); ++i) history[i].id = {200 + i};
	DrawCommand visible;
	visible.kind  = DrawCommandKind::Image;
	visible.rect  = {0, 0, 2, 1};
	visible.image = precise.id;
	renderer.sync_images(history, std::span(&visible, 1));
	CHECK(renderer.statistics().residentImages <= 2);
	const Point coordinates[] = {{0, 0}, {1, 0}};
	auto        actual        = renderer.read_image_samples(precise.id, coordinates);
	CHECK(actual.size() == 2 && actual[1][0] > actual[0][0]);
	CHECK(std::abs(actual[1][0] - 32769.f / 65535) < 1e-7);
	high                 = std::make_shared<RawImage>(*high);
	high->color.transfer = TransferCharacteristics::PQ;
	for (int p = 0; p < 3; ++p) put_sample(high->planes[p], 1, 0, 2, 65535, 65535);
	history[0].decoded = high;
	++history[0].previewRevision;
	renderer.sync_images(history, std::span(&visible, 1));
	actual = renderer.read_image_samples(precise.id, coordinates);
	CHECK(actual[1][0] > 40 && actual[1][1] > 40 && actual[1][2] > 40);
	auto replacement            = std::make_shared<RawImage>(*high);
	replacement->color.transfer = TransferCharacteristics::Linear;
	history[0].decoded          = replacement;
	renderer.sync_images(history, std::span(&visible, 1));
	actual = renderer.read_image_samples(precise.id, coordinates);
	CHECK(actual[1][0] <= 1.00001f); // Immutable source replacement invalidates a stale GPU texture.
	std::cout << "FP32 GPU texture verification passed: adjacent 16-bit codes remain distinct, PQ highlights remain "
				 "above 1, and 100 retained results upload only the visible image\n";
	std::cout << "GPU verification passed: nested clipping, arbitrary layers, text/geometry/image order, alpha "
				 "blending, lines/circles/rounded shapes; "
			  << renderer.statistics().instances << " instances in " << renderer.statistics().dispatches
			  << " compute dispatches; unchanged glyph atlas uploaded 0 bytes\n"
			  << std::flush;
}
static void verify_gpu_difference(VulkanRenderer &renderer) {
	auto a    = std::make_shared<RawImage>();
	a->width  = 2;
	a->height = 1;
	a->format = PixelFormat::RGBP16LE;
	a->color  = {ColorPrimaries::BT2020,
				 TransferCharacteristics::Linear,
				 MatrixCoefficients::Identity,
				 ColorRange::Full,
				 {}};
	for (auto &p: a->planes) {
		p.strideBytes = 4;
		p.bytes.resize(4);
		put_sample(p, 0, 0, 2, 32768, 65535);
		put_sample(p, 1, 0, 2, 32768, 65535);
	}
	auto b = std::make_shared<RawImage>(*a);
	for (auto &p: b->planes) put_sample(p, 1, 0, 2, 32769, 65535);
	std::vector<ImageObject> images(2);
	images[0].id      = {700};
	images[0].decoded = a;
	images[1].id      = {701};
	images[1].decoded = b;
	DrawCommand difference;
	difference.kind            = DrawCommandKind::Image;
	difference.rect            = {0, 0, 200, 100};
	difference.color           = {1, 1, 1, 1};
	difference.image           = images[0].id;
	difference.comparisonImage = images[1].id;
	difference.differenceGain  = 4096;
	renderer.sync_images(images, {&difference, 1});
	const Point points[] = {{25, 50}, {175, 50}};
	auto        pixels   = renderer.render_draw_list_samples({difference}, nullptr, points);
	CHECK(pixels[0].r < 2 && pixels[0].g < 2 && pixels[0].b < 2);
	CHECK(pixels[1].r >= 15 && pixels[1].r <= 17 && std::abs(int(pixels[1].r) - int(pixels[1].g)) <= 1 &&
		  std::abs(int(pixels[1].r) - int(pixels[1].b)) <= 1);
	std::cout << "Vulkan difference passed: exact zero and amplified adjacent 16-bit codes without a derived image or "
				 "integer heatmap\n";
}
static void verify_linear_presentation(VulkanRenderer& renderer){
    const Point positions[]={{50,50},{150,50},{250,50},{350,50}};
    bool sawHDR=false,sawNegative=false;
    for(int fixture=0;fixture<3;++fixture){
        auto raw=std::make_shared<RawImage>();
        raw->width=4;raw->height=1;
        raw->format=fixture==2?PixelFormat::YUV444P16LE:PixelFormat::RGBP16LE;
        raw->color={.primaries=ColorPrimaries::BT2020,
            .transfer=fixture==1?TransferCharacteristics::PQ:TransferCharacteristics::Linear,
            .matrix=fixture==2?MatrixCoefficients::BT2020NonConstant:MatrixCoefficients::Identity,.range=ColorRange::Full};
        for(int c=0;c<3;++c){raw->planes[c].strideBytes=8;raw->planes[c].bytes.resize(8);
            for(int i=0;i<4;++i)put_sample(raw->planes[c],i,0,2,(i==c||i==3)?65535:0,65535);}
        const auto expected=raw_image_to_linear_bt2020(*raw);
        ImageObject image;image.id={990};image.decoded=raw;image.previewRevision=fixture;
        DrawCommand draw;draw.kind=DrawCommandKind::Image;draw.image=image.id;
        draw.rect={0,0,400,100};draw.color={1,1,1,1};
        renderer.sync_images(std::span(&image,1),std::span(&draw,1));
        const auto samples=renderer.render_draw_list_linear_samples({draw},nullptr,positions);
        CHECK(samples.size()==4);
        for(int i=0;i<4;++i)for(int c=0;c<3;++c){
            const float want=expected.planes[c][i];
            CHECK(std::abs(samples[i][c]-want)<.001f*std::max(1.f,std::abs(want)));
            sawHDR|=want>1.f;sawNegative|=want<0.f;
        }
    }
    CHECK(sawHDR&&sawNegative);
    std::cout<<"FP16 presentation passed: BT.2020 primaries unchanged; values above 1 and negative components retained\n";
}
static void verify_mipmapping(VulkanRenderer& renderer, float outputScale) {
    auto raw=std::make_shared<RawImage>();
    raw->width=raw->height=64;raw->format=PixelFormat::RGBP16LE;
    raw->color={.primaries=ColorPrimaries::BT2020,.transfer=TransferCharacteristics::Linear,
        .matrix=MatrixCoefficients::Identity,.range=ColorRange::Full};
    for(auto& plane:raw->planes){
        plane.strideBytes=128;plane.bytes.resize(64*128);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)put_sample(plane,x,y,2,((x+y)&1)?65535:0,65535);
    }
    ImageObject image;image.id={995};image.decoded=raw;
    DrawCommand draw;draw.kind=DrawCommandKind::Image;draw.image=image.id;draw.color={1,1,1,1};
    auto render=[&](float width,float height,std::span<const Point> physicalPoints){
        draw.rect={0,0,width/outputScale,height/outputScale};
        renderer.sync_images(std::span(&image,1),std::span(&draw,1));
        std::vector<Point> logicalPoints;
        for(auto point:physicalPoints)logicalPoints.push_back({point.x/outputScale,point.y/outputScale});
        return renderer.render_draw_list_linear_samples({draw},nullptr,logicalPoints);
    };
    const Point small[]={{.5,.5},{1.5,2.5},{3.5,3.5},{6.5,5.5}};
    for(float width:{8.f,20.f}){ // Integer and fractional mip levels.
        const auto samples=render(width,width,small);
        for(auto pixel:samples)for(int c=0;c<3;++c)CHECK(std::abs(pixel[c]-.5f)<.002f);
    }
    // A UV crop has a smaller footprint than the complete image.
    draw.uvRect={.25f,0,.5f,1};
    for(auto pixel:render(8,8,small))for(int c=0;c<3;++c)CHECK(std::abs(pixel[c]-.5f)<.002f);
    draw.uvRect={0,0,1,1};
    const Point edges[]={{.5,.5},{1.5,.5},{2.5,.5},{3.5,.5},{4.5,.5},{5.5,.5},{7.5,.5},{8.5,.5}};
    for(float zoom:{1.f,4.f,3.3f}){
        const auto samples=render(64*zoom,64*zoom,edges);
        for(std::size_t i=0;i<samples.size();++i){
            const float expected=int(std::floor(edges[i].x/zoom))&1;
            for(int c=0;c<3;++c)CHECK(std::abs(samples[i][c]-expected)<.001f);
        }
    }
    // Comparing the same image must use the same minification on both inputs.
    draw.comparisonImage=image.id;
    for(auto pixel:render(8,8,small))for(int c=0;c<3;++c)CHECK(std::abs(pixel[c])<.001f);
    draw.comparisonImage={};
    // Replacing a live preview must regenerate every mip, while level zero
    // stays exact enough to inspect adjacent 16-bit codes.
    raw=std::make_shared<RawImage>(*raw);
    for(auto& plane:raw->planes)for(int y=0;y<64;++y)for(int x=0;x<64;++x)put_sample(plane,x,y,2,16384,65535);
    image.decoded=raw;++image.previewRevision;
    for(auto pixel:render(8,8,small))for(int c=0;c<3;++c)CHECK(std::abs(pixel[c]-.25f)<.002f);
    const Point native[]={{0,0},{1,0},{63,63}};
    for(auto pixel:renderer.read_image_samples(image.id,native))for(int c=0;c<3;++c)CHECK(std::abs(pixel[c]-16384.f/65535)<1e-7f);
    for(const auto size:{std::pair{63,17},std::pair{63,1},std::pair{1,1}}){
        auto thin=std::make_shared<RawImage>(*raw);thin->width=size.first;thin->height=size.second;
        for(auto& plane:thin->planes){plane.strideBytes=thin->width*2;plane.bytes.resize(plane.strideBytes*thin->height);
            for(int y=0;y<thin->height;++y)for(int x=0;x<thin->width;++x)put_sample(plane,x,y,2,16384,65535);}
        image.decoded=thin;
        const Point point[]={{.5,.5}};
        for(auto pixel:render(1,1,point))for(int c=0;c<3;++c)CHECK(std::abs(pixel[c]-.25f)<.002f);
    }
    std::cout<<"Mipmap GPU readback passed: checkerboard minification, fractional LOD, UV crops, difference, live updates, odd/thin images; exact nearest texels at 1:1 and integer/fractional magnification\n";
}

static void verify_color_conversion(VulkanRenderer &renderer) {
	const PixelFormat formats[] = {
			PixelFormat::RGBP8,       PixelFormat::RGBP14LE,    PixelFormat::RGBP16LE,    PixelFormat::YUV420P8,
			PixelFormat::YUV420P10LE, PixelFormat::YUV420P12LE, PixelFormat::YUV420P14LE, PixelFormat::YUV422P8,
			PixelFormat::YUV422P10LE, PixelFormat::YUV422P12LE, PixelFormat::YUV422P14LE, PixelFormat::YUV444P8,
			PixelFormat::YUV444P10LE, PixelFormat::YUV444P12LE, PixelFormat::YUV444P14LE, PixelFormat::YUV444P16LE,
			PixelFormat::Gray8,       PixelFormat::Gray10LE,    PixelFormat::Gray12LE,    PixelFormat::Gray14LE};
	const TransferCharacteristics transfers[] = {TransferCharacteristics::Linear,    TransferCharacteristics::SRGB,
												 TransferCharacteristics::BT709,     TransferCharacteristics::BT601,
												 TransferCharacteristics::BT2020_10, TransferCharacteristics::BT2020_12,
												 TransferCharacteristics::Gamma22,   TransferCharacteristics::Gamma28,
												 TransferCharacteristics::PQ,        TransferCharacteristics::HLG};
	const ColorPrimaries primaries[] = {ColorPrimaries::BT709,  ColorPrimaries::BT2020,  ColorPrimaries::DisplayP3,
										ColorPrimaries::BT470M, ColorPrimaries::BT470BG, ColorPrimaries::BT601_525,
										ColorPrimaries::Film,   ColorPrimaries::DCIP3,   ColorPrimaries::XYZ};
	int                  cases       = 0;
	for (auto format: formats)
		for (auto transfer: transfers) {
			auto raw    = std::make_shared<RawImage>();
			raw->width  = 17;
			raw->height = 11;
			raw->format = format;
			raw->color  = {primaries[cases % 9], transfer,
						   is_rgb(format)   ? MatrixCoefficients::Identity
						   : cases % 3 == 0 ? MatrixCoefficients::BT601
						   : cases % 3 == 1 ? MatrixCoefficients::BT709
											: MatrixCoefficients::BT2020NonConstant,
						   cases % 2 ? ColorRange::Full : ColorRange::Limited, Chroma420SampleLocation(cases % 6)};
			raw->nominalPeakNits = cases % 2 ? 203. : 1000.;
			for (int p = 0; p < plane_count(format); ++p) {
				auto &plane = raw->planes[p];
				plane.strideBytes =
						plane_width(*raw, p) * bytes_per_sample(format) + 3; // Includes odd 16-bit row alignment.
				plane.bytes.resize(std::size_t(plane.strideBytes) * plane_height(*raw, p));
				for (int y = 0; y < plane_height(*raw, p); ++y)
					for (int x = 0; x < plane_width(*raw, p); ++x) {
						uint32_t code = uint32_t((x * 199 + y * 71 + p * 311 + cases * 13) % 1024);
						// Valid PQ signal domain; other transfers exercise extended
						// negative/out-of-range RGB after YUV reconstruction.
						if (transfer == TransferCharacteristics::PQ)
							code = p && !is_rgb(format) ? 460 + code / 10 : 200 + code / 2;
						put_sample(plane, x, y, bytes_per_sample(format), code * max_sample_value(format) / 1023,
								   max_sample_value(format));
					}
			}
			auto        expected = raw_image_to_linear_bt2020(*raw);
			ImageObject image;
			image.id              = {500};
			image.decoded         = raw;
			image.previewRevision = cases;
			renderer.sync_images({&image, 1});
			std::vector<Point> points;
			for (int y = 0; y < raw->height; ++y)
				for (int x = 0; x < raw->width; ++x) points.push_back({float(x), float(y)});
			auto actual = renderer.read_image_samples(image.id, points);
			for (std::size_t i = 0; i < actual.size(); ++i)
				for (int p = 0; p < 3; ++p) {
					auto reference = expected.planes[p][i];
					// FP32 PQ followed by a wide-gamut matrix can cancel large terms
					// into a near-zero channel. Bound error against the pixel's linear
					// magnitude, not that cancelled channel alone (well below FP16).
					double magnitude = 0;
					for (int c = 0; c < 3; ++c)
						magnitude = std::max(magnitude, double(std::abs(expected.planes[c][i])));
					if (!std::isfinite(actual[i][p]) || std::abs(actual[i][p] - reference) > 5e-6 + 3e-4 * magnitude) {
						std::cerr << "Vulkan color mismatch case=" << cases << " pixel=" << i << " plane=" << p
								  << " actual=" << actual[i][p] << " expected=" << reference << '\n';
						CHECK(false);
					}
				}
			++cases;
		}
	auto invalid   = std::make_shared<RawImage>();
	invalid->width = invalid->height = 4;
	invalid->format                  = PixelFormat::YUV444P16LE;
	invalid->color                   = {ColorPrimaries::BT2020,
										TransferCharacteristics::PQ,
										MatrixCoefficients::BT2020NonConstant,
										ColorRange::Limited,
										{}};
	for (auto &plane: invalid->planes) {
		plane.strideBytes = 8;
		plane.bytes.resize(32, 255);
	}
	ImageObject image;
	image.id         = {501};
	image.decoded    = invalid;
	bool cpuRejected = false, gpuRejected = false;
	try {
		(void) raw_image_to_linear_bt2020(*invalid);
	} catch (const std::invalid_argument &) { cpuRejected = true; }
	try {
		renderer.sync_images({&image, 1});
	} catch (const std::invalid_argument &) { gpuRejected = true; }
	CHECK(cpuRejected && gpuRejected);
	std::cout << "Vulkan/FP64 color parity passed: " << cases
			  << " format/transfer cases, all primaries, range/chroma locations, odd strides, unclipped HDR\n";
}
int main(int argc, char **argv) try {
	const bool verify = argc > 1 && std::string(argv[1]) == "--verify";
	auto window = WaylandWindow::create(2880, 1800,
										verify ? "codec_vis renderer verification" : "codec_vis framework laboratory");
	auto renderer    = VulkanRenderer::create(window);
	const auto image = fixture_image();
	renderer.sync_images(std::span(&image, 1));
	TextShaper     defaultFont(default_font_path(), 16 * window.output_scale());
	TextAtlasCache cache;
	if (verify) {
		verify_renderer(renderer, defaultFont, cache);
		verify_gpu_difference(renderer);
		verify_color_conversion(renderer);
        verify_linear_presentation(renderer);
        verify_mipmapping(renderer,window.output_scale());
		return 0;
	}
	std::map<std::pair<int, FontRole>, std::unique_ptr<TextShaper>> fonts;
	Context               ui([&](std::string_view text, float size, FontRole role) {
		auto &font = fonts[{int(size * 64 * window.output_scale()), role}];
		if (!font)
			font = std::make_unique<TextShaper>(role == FontRole::Monospace ? default_monospace_font_path()
																			: default_font_path(),
												size * window.output_scale());
		return font->shape(text).width / window.output_scale();
	});
	DockSizes             sizes;
	bool                  focus = false, linked = true, expanded = false;
	std::string           search  = "Find a fixture";
	double                qp      = 35;
	std::size_t           profile = 0;
	int                   actions = 0;
	std::vector<Choice>   choices{{"Main 10 fixture"},
								  {"SCC fixture"},
								  {"Disabled choice", false},
								  {"Long UTF-8 label: ΔΕ λ — 5600 × 3728"}};
	std::vector<Column>   columns{{"id", "Run", 90, 60},
								  {"name", "Synthetic backend / long label", 500, 100},
								  {"size", "Synthetic bytes", 200, 80, TextAlign::Right},
								  {"time", "Synthetic time (ms)", 200, 100, TextAlign::Right},
								  {"state", "State fixture", 220, 100}};
	std::vector<uint64_t> order(100000);
	std::iota(order.begin(), order.end(), 1);
	std::vector<std::size_t> index(order.size() + 1);
	for (std::size_t i = 0; i < order.size(); ++i) index[order[i]] = i;
	TableModel             model{order.size(), [&](auto i) { return order.at(i); },
								 [&](uint64_t id) -> std::optional<std::size_t> {
						 return id && id < index.size() ? std::optional<std::size_t>(index[id]) : std::nullopt;
								 },
								 [&](auto row, auto col) {
						 auto id = order[row];
						 if (col == 0) return std::to_string(id);
						 if (col == 1)
							 return std::string("Fixture ") + std::to_string(id) +
									" — VA-API / long implementation name / 5600 × 3728";
						 if (col == 2) return std::to_string(id * 413);
						 if (col == 3) return std::to_string((id * 23) % 1000);
						 return id % 7 == 0   ? std::string("Failed: fixture error")
								: id % 5 == 0 ? std::string("Not measured")
											  : std::string("Completed fixture");
								 }};
	std::vector<PlotPoint> points;
	for (uint64_t i = 1; i <= 200; ++i) points.push_back({i, double(i), double((i * 37) % 100)});
	auto build = [&]() {
		ui.begin_frame({0, 0, float(window.width()), float(window.height())});
		sizes.focus       = focus;
		const auto layout = dock_layout({0, 0, float(window.width()), float(window.height())}, sizes);
		ui.fill({0, 0, float(window.width()), float(window.height())}, ui.theme.background);
		ui.fill(layout.bar, ui.theme.surface);
		ui.label({12, 10, 520, 28}, "Framework laboratory · synthetic data", ui.theme.text, 20);
		ui.checkbox("focus", "Focus canvas", {650, 6, 180, 32}, focus);
		ui.label({900, 10, 1400, 26},
				 "100,000 virtual rows · no codec measurements · drag panel/table dividers · Tab to navigate",
				 ui.theme.muted);
		if (!focus) {
			ui.fill(layout.left, ui.theme.surface);
			ui.label({12, layout.left.y + 12, layout.left.w - 24, 32}, "Sources", ui.theme.text, 22);
			ui.button("source", "In-memory colour fixture", {12, layout.left.y + 58, layout.left.w - 24, 36});
			ui.fill(layout.right, ui.theme.surface);
			const float x = layout.right.x + 16, w = layout.right.w - 32;
			float       y = layout.right.y + 16;
			ui.label({x, y, w, 32}, "Controls", ui.theme.text, 22);
			y += 48;
			ui.label({x, y, w, 24}, "Text editing / UTF-8", ui.theme.muted);
			y += 28;
			ui.text_input("search", {x, y, w, 36}, search);
			y += 52;
			ui.label({x, y, w, 24}, "Validated integer (0…63)", ui.theme.muted);
			y += 28;
			ui.number_input("qp", {x, y, w, 36}, qp, {0, 63, 1, true});
			y += 44;
			auto error = ui.edit_error("qp");
			if (!error.empty()) {
				ui.label({x, y, w, 52}, error, ui.theme.danger, 0, TextAlign::Left, TextOverflow::Wrap);
				y += 60;
			}
			ui.label({x, y, w, 24}, "Dropdown / disabled option fixture", ui.theme.muted);
			y += 28;
			ui.combo("profile", {x, y, w, 36}, choices, profile);
			y += 52;
			ui.checkbox("linked", "Linked navigation fixture", {x, y, w, 36}, linked);
			y += 48;
			if (ui.button("action", "Exercise action", {x, y, w, 36})) ++actions;
			y += 44;
			ui.button("disabled", "Disabled action", {x, y, w, 36}, false);
			y += 52;
			ui.disclosure("advanced", "More controls", {x, y, w, 36}, expanded);
			y += 44;
			if (expanded)
				ui.label({x, y, w, 130},
						 "Measured wrapping keeps long backend names, capability explanations and numeric units inside "
						 "their allotted space. ΔE — 5600 × 3728.",
						 ui.theme.muted, 0, TextAlign::Left, TextOverflow::Wrap);
			ui.label({x, layout.right.y + layout.right.h - 64, w, 52},
					 "Actions: " + std::to_string(actions) + "  QP fixture: " + std::to_string(int(qp)), ui.theme.text,
					 16, TextAlign::Left, TextOverflow::Wrap, FontRole::Monospace);
		}
		Rect canvas = inset(layout.canvas, 16);
		ui.label({canvas.x, canvas.y, canvas.w, 30}, "A: in-memory image       B: synthetic scatter selection",
				 ui.theme.text, 18);
		Rect picture{canvas.x, canvas.y + 44, canvas.w * .48f, std::max(0.f, canvas.h - 100)};
		ui.push_clip(picture);
		ui.image(picture, image.id);
		ui.pop_clip();
		Rect plot{picture.x + picture.w + 16, picture.y, canvas.w - picture.w - 16, picture.h};
		if (auto picked = ui.plot("plot", plot, points, {0, 200, 0, 100, false, false, "Synthetic X", "Synthetic Y"},
								  ui.table_state("results").active)) {
			auto &state     = ui.table_state("results");
			state.active    = *picked;
			state.selection = {*picked};
		}
		if (!focus) {
			auto result = ui.table("results", inset(layout.bottom, 8), columns, model);
			if (result.sortChanged) {
				const auto &state = ui.table_state("results");
				auto        less  = [&](uint64_t a, uint64_t b) {
					if (state.sortColumn == "time" && (a * 23) % 1000 != (b * 23) % 1000)
						return (a * 23) % 1000 < (b * 23) % 1000;
					if (state.sortColumn == "name") return std::to_string(a) < std::to_string(b);
					if (state.sortColumn == "state") {
						auto rank = [](uint64_t n) { return n % 7 == 0 ? 1 : n % 5 == 0 ? 2 : 0; };
						if (rank(a) != rank(b)) return rank(a) < rank(b);
					}
					return a < b;
				};
				std::sort(order.begin(), order.end(),
						  [&](auto a, auto b) { return state.ascending ? less(a, b) : less(b, a); });
				for (std::size_t i = 0; i < order.size(); ++i) index[order[i]] = i;
			}
			ui.splitter("dock:left", layout.leftHandle, sizes.left, Axis::Horizontal, 180, 600);
			ui.splitter("dock:right", layout.rightHandle, sizes.right, Axis::Horizontal, 280, 760, true);
			ui.splitter("dock:bottom", layout.bottomHandle, sizes.bottom, Axis::Vertical, 160, 800, true);
		}
		ui.fill(layout.status, ui.theme.surface);
		auto stats = renderer.statistics();
		ui.label(inset(layout.status, 4),
				 "Native Vulkan · " + std::to_string(stats.instances) + " instances / " +
						 std::to_string(stats.dispatches) + " compute dispatches · atlas upload " +
						 std::to_string(stats.textUploadBytes) + " bytes · focus: " + ui.focus(),
				 ui.theme.muted);
		ui.end_frame();
		cache.build(ui.commands(), defaultFont, 1024, window.output_scale());
	};
	std::string pasteTarget;
	build();
	while (!window.close_requested()) {
		window.dispatch_pending();
		for (const auto &action: window.take_actions()) {
			if (action.kind == ActionKind::ClipboardText) {
				ui.paste(pasteTarget, action.text);
				pasteTarget.clear();
			} else if (auto input = input_from_action(action))
				ui.handle(*input);
		}
		for (auto &request: ui.take_clipboard_requests()) {
			if (request.kind == ClipboardRequest::Copy) window.set_clipboard_text(std::move(request.text));
			else if (window.request_clipboard_text())
				pasteTarget = request.widget;
		}
		if (renderer.width() != uint32_t(window.framebuffer_width()) ||
			renderer.height() != uint32_t(window.framebuffer_height()))
			renderer.recreate_swapchain(window);
		build();
		renderer.render_draw_list(ui.commands(), &cache.atlas());
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
	}
	return 0;
} catch (const std::exception &e) {
	std::cerr << e.what() << '\n';
	return 1;
}
