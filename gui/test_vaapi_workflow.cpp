#include "test_check.hpp"
#include "app_state.hpp"
#include "encode_runner.hpp"
#include "encoder_backends.hpp"
#include "metrics.hpp"
#include "preview_decoders.hpp"
#include "raw_image_conversion.hpp"
#include "../codec_gui_image_io.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <thread>

using namespace codec_gui;
using namespace codec_gui::gui;

static std::size_t fd_count() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{});
}

int main(int argc, char** argv) {
    if (argc>=2 && std::string(argv[1])=="--target-usage") {
        const auto backends=initial_encoder_backends();
        const auto* hevc=find_backend(backends,BackendId{4});
        RawImage input;
        if(argc==3) input=load_input_image(argv[2]);
        else {
            input.width=5600; input.height=3728; input.format=PixelFormat::RGBP8;
            input.color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,
                         .matrix=MatrixCoefficients::BT709,.range=ColorRange::Full};
            for(int p=0;p<3;++p) {
                auto& plane=input.planes[p];
                plane.strideBytes=input.width;
                plane.bytes.resize(std::size_t(input.width)*input.height);
                for(int y=0;y<input.height;++y) for(int x=0;x<input.width;++x)
                    plane.bytes[std::size_t(y)*input.width+x]=uint8_t((x/3+y/5+p*47+((x/16)^(y/16))*5)&255);
            }
        }
        std::cout<<"Target-usage benchmark "<<input.width<<'x'<<input.height<<" HEVC ICQ 40, 10-bit 4:2:0; no decoding or metrics"<<std::endl;
        for(int usage : {1,7,7,1}) {
            const std::vector<EncoderParam> params{
                {.name="rate-control",.value=std::string{"icq"}}, {.name="qpi",.value=int64_t{40}},
                {.name="target-usage",.value=int64_t{usage}}, {.name="bit-depth",.value=std::string{"10"}},
                {.name="chroma-subsampling",.value=std::string{"420"}}
            };
            const auto start=std::chrono::steady_clock::now();
            const auto result=run_backend_encode(*hevc,input,params);
            const double total=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"TU="<<usage<<" encode="<<result.metadata.encodeSeconds<<" s preprocessing="
                     <<total-result.metadata.encodeSeconds<<" s bytes="<<result.encoded.hevcAnnexB.size()<<std::endl;
        }
        return 0;
    }
    if (argc==3 && (std::string(argv[1])=="--image" || std::string(argv[1])=="--image-validated" || std::string(argv[1])=="--image-no-sao" || std::string(argv[1])=="--image-no-tiles" || std::string(argv[1])=="--image-no-filters")) {
        const auto backends=initial_encoder_backends();
        const auto* hevc=find_backend(backends,BackendId{4});
        const auto input=load_input_image(argv[2]);
        int mismatches=0;
        const bool validatedOnly=std::string(argv[1])=="--image-validated";
        for(const std::string depth : {"8","10"}) for(int recipe=0;recipe<(validatedOnly?2:4);++recipe) {
            std::vector<EncoderParam> params{
                {.name="scc",.value=recipe!=0}, {.name="scc-palette",.value=recipe==1 || recipe==3},
                {.name="scc-ibc",.value=recipe==2 || recipe==3}, {.name="capture-reconstruction",.value=true},
                {.name="bit-depth",.value=depth}, {.name="chroma-subsampling",.value=std::string{"420"}},
                {.name="qpi",.value=int64_t{35}}, {.name="num-tile-cols",.value=int64_t{2}}, {.name="num-tile-rows",.value=int64_t{2}}
            };
            if(validatedOnly) std::erase_if(params,[](const EncoderParam& p){ return p.name=="scc-ibc"; });
            if(std::string(argv[1])=="--image-no-sao") params.push_back({.name="sao",.value=false});
            if(std::string(argv[1])=="--image-no-filters") {
                params.push_back({.name="sao",.value=false});
                params.push_back({.name="deblock",.value=false});
            }
            if(std::string(argv[1])=="--image-no-tiles") {
                for(auto& p:params) if(p.name=="num-tile-cols" || p.name=="num-tile-rows") p.value=int64_t{1};
            }
            std::cout<<"image reconstruction depth="<<depth<<" recipe="<<recipe<<std::endl;
            auto result=run_backend_encode(*hevc,input,params);
            const auto reconstruction=result.encoded.previewImage;
            result.encoded.previewImage.reset();
            const auto decoded=hevc->decodePreview(result.encoded);
            CODEC_VIS_CHECK(reconstruction && decoded.image);
            if(recipe>=2) {
                const auto qsv=decode_hevc_qsv_preview(result.encoded);
                if(!qsv.image) std::cerr<<qsv.error<<std::endl;
                else {
                    bool same=true;
                    for(int p=0;p<3;++p) same &= reconstruction->planes[p].bytes==qsv.image->planes[p].bytes;
                    std::cout<<"QSV decoder matches encoder reconstruction="<<same<<std::endl;
                }
            }
            bool same=true;
            for(int p=0;p<3;++p) {
                const auto& a=reconstruction->planes[p];
                const auto& b=decoded.image->planes[p];
                std::size_t count=0,first=0,last=0;
                for(std::size_t i=0;i<a.bytes.size();++i) if(a.bytes[i]!=b.bytes[i]) {
                    if(!count) first=i;
                    last=i; ++count;
                }
                if(count) {
                    same=false;
                    std::cout<<"plane="<<p<<" mismatched bytes="<<count<<" first="<<first<<" last="<<last<<" stride="<<a.strideBytes<<std::endl;
                }
            }
            mismatches+=!same;
            std::cout<<"decoder/reconstruction match="<<same<<", bytes="<<result.encoded.hevcAnnexB.size()<<std::endl;
        }
        return mismatches ? 1 : 0;
    }
    const int onlyFormat = argc > 1 ? std::stoi(argv[1]) : -1;
    const auto backends = initial_encoder_backends();
    auto source = std::make_shared<RawImage>();
    source->width = 640; source->height = 480; source->format = PixelFormat::YUV420P8;
    source->color.primaries = ColorPrimaries::BT709;
    source->color.transfer = TransferCharacteristics::SRGB;
    source->color.matrix = MatrixCoefficients::BT709;
    source->color.range = ColorRange::Limited;
    for (int p = 0; p < 3; ++p) {
        const int w = p ? 320 : 640, h = p ? 240 : 480;
        source->planes[p].strideBytes = w;
        source->planes[p].bytes.resize(w*h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            source->planes[p].bytes[y*w+x] = p ? 100 + (x/8+y/8)%56 : 32+(x/8+y/8)%180;
    }
    const auto original = source->planes;
    if (onlyFormat < 0) for (const uint64_t id : {4,5}) {
        const auto* backend = find_backend(backends, BackendId{id});
        CODEC_VIS_CHECK(backend && backend->queryCapabilities().snapshot.available);
        if (id == 4) {
            for (const auto& params : std::vector<std::vector<EncoderParam>>{
                {{.name="qpi",.value=int64_t{0}}},
                {{.name="qpi",.value=int64_t{52}}},
                {{.name="entropy-coding-sync",.value=true}},
                {{.name="scc",.value=true},{.name="chroma-subsampling",.value=std::string{"422"}}},
                {{.name="bit-depth",.value=std::string{"12"}}}
            }) {
                bool rejected=false;
                try { (void)backend->encode(*source,params); }
                catch (const std::exception& error) { rejected=!std::string(error.what()).empty(); }
                CODEC_VIS_CHECK(rejected);
            }
        }
        // Warm up the same process before comparing resource counts.
        const auto warm = run_backend_encode(*backend, *source, {});
        CODEC_VIS_CHECK(backend->decodePreview(warm.encoded).image);
        const auto before = fd_count();
        AppState state;
        ImageObject image;
        image.id = next_image_id(state); image.type = ImageObjectType::Source;
        image.decoded = source; image.width=640; image.height=480;
        state.images.push_back(image);
        {
            EncodeRunner runner(backends);
            std::set<uint64_t> pending;
            std::map<uint64_t, int> indices;
            // Per group: successful job, canceled queued job, invalid native
            // request, successful recovery. Repeat in one process for leaks.
            for (int i = 0; i < 24; ++i) {
                EncodeRun run;
                run.id = next_run_id(state); run.source=image.id; run.backend=backend->id;
                state.encodeRuns.push_back(run);
                Command command;
                command.kind=CommandKind::RunEncode; command.run=run.id;
                command.image=image.id; command.backend=backend->id;
                if (i%4==2) command.params={{.name="rate-control",.value=std::string{"invalid-test-mode"}}};
                if (i%4==1) runner.request_cancel(run.id);
                runner.submit(command,state);
                pending.insert(run.id.value);
                indices[run.id.value] = i;
            }
            unsigned completed=0,canceled=0,failed=0;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
            while (!pending.empty() && std::chrono::steady_clock::now()<deadline) {
                for (const auto& action : runner.take_actions()) {
                    if (action.kind==ActionKind::EncodeRunStarted) continue;
                    const bool done=action.kind==ActionKind::EncodeRunCompleted;
                    const auto run=done?action.encodeCompleted.run:action.run;
                    CODEC_VIS_CHECK(pending.erase(run.value)==1);
                    const int index=indices.at(run.value);
                    if (index%4==1) {
                        CODEC_VIS_CHECK(action.kind==ActionKind::EncodeRunCanceled); ++canceled;
                    } else if (index%4==2) {
                        CODEC_VIS_CHECK(action.kind==ActionKind::EncodeRunFailed);
                        CODEC_VIS_CHECK(!action.text.empty()); ++failed;
                    } else {
                        CODEC_VIS_CHECK(done); ++completed;
                        const auto& result=action.encodeCompleted;
                        CODEC_VIS_CHECK(result.preview);
                        CODEC_VIS_CHECK(result.metadata.previewError.empty());
                        CODEC_VIS_CHECK(result.metadata.metricError.empty());
                        CODEC_VIS_CHECK(result.metadata.byteSize==result.metadata.bytes.size());
                        CODEC_VIS_CHECK(!result.metadata.psnrY && result.metadata.metrics.empty());
                        CODEC_VIS_CHECK(result.metadata.metricState==MetricState::NotRequested);
                        CODEC_VIS_CHECK(result.metadata.metricSeconds==0);
                        CODEC_VIS_CHECK(result.metadata.encodeSeconds>0 && result.metadata.decodeSeconds>0);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CODEC_VIS_CHECK(pending.empty());
            CODEC_VIS_CHECK(completed==12 && canceled==6 && failed==6);
            std::cout<<"backend="<<id<<" completed="<<completed<<" canceled="<<canceled<<" failed-as-requested="<<failed<<'\n';
        }
        const auto after=fd_count();
        std::cout<<"backend="<<id<<" descriptors-before="<<before<<" after="<<after<<'\n';
        CODEC_VIS_CHECK(after==before);
        for (int p=0;p<3;++p) CODEC_VIS_CHECK(source->planes[p].bytes==original[p].bytes);
    }
    const auto* hevc=find_backend(backends, BackendId{4});
    unsigned sccCases=0,tileCases=0;
    unsigned sccRateCases=0;
    for (const auto format : {PixelFormat::YUV420P8, PixelFormat::YUV420P10LE, PixelFormat::YUV422P8, PixelFormat::YUV422P10LE, PixelFormat::YUV444P8, PixelFormat::YUV444P10LE}) {
        if (onlyFormat >= 0 && int(format) != onlyFormat) continue;
        const auto baseInput=convert_raw_image_format(*source,format);
        RawImage large=baseInput;
        large.width*=2; large.height*=2;
        for(int p=0;p<3;++p) {
            const int oldStride=baseInput.planes[p].strideBytes;
            const int oldRows=baseInput.planes[p].bytes.size()/oldStride;
            large.planes[p].strideBytes=oldStride*2;
            large.planes[p].bytes.resize(baseInput.planes[p].bytes.size()*4);
            for(int y=0;y<oldRows*2;++y) for(int x=0;x<oldStride*2;++x)
                large.planes[p].bytes[y*oldStride*2+x]=baseInput.planes[p].bytes[(y%oldRows)*oldStride+x%oldStride];
        }
        for (const bool scc : {false,true}) for (int recipe=0;recipe<(scc?4:1);++recipe) {
            if (scc && (format==PixelFormat::YUV422P8 || format==PixelFormat::YUV422P10LE)) continue;
            for (const int tiles : {1,2,4}) {
                const auto& input=tiles==4?large:baseInput;
                const auto reference=raw_image_to_rgba8(input);
                const std::vector<EncoderParam> params{
                    {.name="scc",.value=scc}, {.name="scc-palette",.value=recipe!=1 && recipe!=3},
                    {.name="scc-ibc",.value=recipe!=0 && recipe!=3},
                    {.name="num-tile-cols",.value=int64_t{tiles}}, {.name="num-tile-rows",.value=int64_t{tiles}},
                    {.name="qpi",.value=int64_t{20}}
                };
                auto diagnosticParams=params;
                if (scc && (format==PixelFormat::YUV420P8 || format==PixelFormat::YUV420P10LE)) diagnosticParams.push_back({.name="capture-reconstruction",.value=true});
                std::cout<<"testing HEVC format="<<int(format)<<" SCC="<<scc<<" recipe="<<recipe<<" tiles="<<tiles<<std::endl;
                auto encoded=hevc->encode(input,diagnosticParams);
                const auto reconstruction=encoded.previewImage;
                encoded.previewImage.reset();
                const auto decoded=hevc->decodePreview(encoded);
                if (!decoded.image) std::cerr<<"decode failed scc="<<scc<<" recipe="<<recipe<<" tiles="<<tiles<<": "<<decoded.error<<'\n';
                CODEC_VIS_CHECK(decoded.image);
                CODEC_VIS_CHECK(decoded.image->width==input.width && decoded.image->height==input.height);
                CODEC_VIS_CHECK(decoded.image->format==input.format);
                CODEC_VIS_CHECK(decoded.image->color.range==input.color.range);
                CODEC_VIS_CHECK(decoded.image->color.primaries==input.color.primaries);
                CODEC_VIS_CHECK(decoded.image->color.transfer==input.color.transfer);
                CODEC_VIS_CHECK(decoded.image->color.matrix==input.color.matrix);
                if (scc && (format==PixelFormat::YUV420P8 || format==PixelFormat::YUV420P10LE)) {
                    CODEC_VIS_CHECK(reconstruction);
                    for(int p=0;p<3;++p) {
                        const auto& a=reconstruction->planes[p].bytes;
                        const auto& b=decoded.image->planes[p].bytes;
                        if (a!=b) {
                            unsigned count=0;
                            std::cerr<<"reconstruction mismatch plane="<<p<<" sizes="<<a.size()<<','<<b.size()<<'\n';
                            for(std::size_t i=0;i<std::min(a.size(),b.size()) && count<12;++i) if(a[i]!=b[i]) {
                                std::cerr<<i<<':'<<unsigned(a[i])<<'/'<<unsigned(b[i])<<' '; ++count;
                            }
                            std::cerr<<std::endl;
                        }
                        CODEC_VIS_CHECK(a==b);
                    }
                }
                const auto pixels=raw_image_to_rgba8(*decoded.image);
                uint64_t error=0;
                unsigned maximum=0;
                for (std::size_t i=0;i<pixels.size();++i) if (i%4!=3) {
                    const auto delta=unsigned(std::abs(int(pixels[i])-int(reference[i])));
                    error+=delta; maximum=std::max(maximum,delta);
                }
                std::cout<<"source RGB mean absolute error="<<double(error)/(pixels.size()/4*3)<<" peak="<<maximum<<'\n';
                CODEC_VIS_CHECK(double(error)/(pixels.size()/4*3)<10);
                if (!scc) {
                    // Independently compare libde265 to the hardware decoder.
                    const auto hardware=decode_hevc_vaapi_preview(encoded);
                    if (!hardware.image) std::cerr<<hardware.error<<std::endl;
                    CODEC_VIS_CHECK(hardware.image && hardware.image->format==input.format);
                    for(int p=0;p<3;++p) CODEC_VIS_CHECK(hardware.image->planes[p].bytes==decoded.image->planes[p].bytes);
                }
                scc ? ++sccCases : ++tileCases;
                std::cout<<"in-memory HEVC format="<<int(format)<<" SCC="<<scc<<" recipe="<<recipe<<" tiles="<<tiles<<" PASS\n";
            }
        }
        if (format!=PixelFormat::YUV422P8 && format!=PixelFormat::YUV422P10LE) {
            const auto caps=hevc->queryCapabilities();
            for (const std::string mode : {"icq","cbr","vbr"}) {
                const auto sccOption=std::find_if(caps.params.begin(),caps.params.end(),[](const auto& p){return p.name=="scc";});
                bool enabled=true;
                if(sccOption!=caps.params.end()) for(const auto& condition:sccOption->enabledWhen)
                    if(condition.parameter=="rate-control" && std::find(condition.acceptedValues.begin(),condition.acceptedValues.end(),mode)==condition.acceptedValues.end()) enabled=false;
                if(!enabled) continue;
                std::vector<EncoderParam> params{
                    {.name="scc",.value=true}, {.name="rate-control",.value=mode},
                    {.name="qpi",.value=int64_t{20}}, {.name="bitrate-kbps",.value=int64_t{1000}}
                };
                const bool capture=format==PixelFormat::YUV420P8 || format==PixelFormat::YUV420P10LE;
                if (capture) params.push_back({.name="capture-reconstruction",.value=true});
                std::cout<<"testing SCC rate control format="<<int(format)<<" mode="<<mode<<std::endl;
                auto encoded=hevc->encode(baseInput,params);
                const auto reconstruction=encoded.previewImage;
                encoded.previewImage.reset();
                const auto decoded=hevc->decodePreview(encoded);
                CODEC_VIS_CHECK(decoded.image);
                CODEC_VIS_CHECK(decoded.image->format==baseInput.format);
                if (capture) {
                    CODEC_VIS_CHECK(reconstruction);
                    for(int p=0;p<3;++p) CODEC_VIS_CHECK(reconstruction->planes[p].bytes==decoded.image->planes[p].bytes);
                }
                ++sccRateCases;
            }
        }
        if(format==PixelFormat::YUV420P10LE || format==PixelFormat::YUV444P10LE) {
            for(const auto transfer : {TransferCharacteristics::PQ,TransferCharacteristics::HLG}) {
                auto hdr=baseInput;
                hdr.color.primaries=ColorPrimaries::BT2020;
                hdr.color.transfer=transfer;
                hdr.color.matrix=MatrixCoefficients::BT2020NonConstant;
                hdr.color.range=transfer==TransferCharacteristics::PQ?ColorRange::Full:ColorRange::Limited;
                const std::vector<EncoderParam> params{{.name="scc",.value=true}};
                auto encoded=hevc->encode(hdr,params);
                encoded.previewImage.reset();
                const auto decoded=hevc->decodePreview(encoded);
                CODEC_VIS_CHECK(decoded.image);
                CODEC_VIS_CHECK(decoded.image->color.primaries==hdr.color.primaries);
                CODEC_VIS_CHECK(decoded.image->color.transfer==hdr.color.transfer);
                CODEC_VIS_CHECK(decoded.image->color.matrix==hdr.color.matrix);
                CODEC_VIS_CHECK(decoded.image->color.range==hdr.color.range);
                std::cout<<"SCC HDR metadata format="<<int(format)<<" transfer="<<int(transfer)<<" PASS\n";
            }
        }
    }
    std::cout<<"SCC tool/tile cases="<<sccCases<<" SCC rate-control cases="<<sccRateCases<<" ordinary HEVC cases="<<tileCases<<'\n';
}
