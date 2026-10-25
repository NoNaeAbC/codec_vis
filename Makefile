CXX ?= g++
.DEFAULT_GOAL := all
CC ?= gcc
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=
DEPFLAGS = -MMD -MP -MF $(@:.o=.d)

PKG_CONFIG ?= pkg-config
PKG_CONFIG_PATH := /usr/local/lib/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH

# Libraries discovered in /usr/local must also be discoverable when the built
# binaries are run. Keep the path in RUNPATH so loader environment overrides
# remain available.
LOCAL_LIBDIR ?= /usr/local/lib
LOCAL_RUNPATH := -Wl,-rpath,$(LOCAL_LIBDIR)

CODEC_PKGS := SvtJpegxs x265 libjpeg libraw libheif libvvenc libvvdec uvg266 SvtAv1Enc libva libva-drm avm libde265 dav1d dav2d charls libpng16 libjxl libjxl_cms libjxl_threads libopenjp2 x264 libavcodec libavutil kvazaar libwebp vpx SvtVp9Enc rav1e gstreamer-codecparsers-1.0 libdrm
GUI_PKGS := wayland-client vulkan harfbuzz freetype2 fontconfig sdbus-c++ xkbcommon
XDG_SHELL_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
FRACTIONAL_SCALE_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/staging/fractional-scale/fractional-scale-v1.xml
VIEWPORTER_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/viewporter/viewporter.xml
COLOR_MANAGEMENT_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/staging/color-management/color-management-v1.xml
WAYLAND_PROTOCOL_OBJS := build/color-management-v1-protocol.o build/xdg-shell-protocol.o build/fractional-scale-v1-protocol.o build/viewporter-protocol.o

JXRLIB_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libjxr 2>/dev/null || echo -I/usr/include/jxrlib)
JXRLIB_LIBS ?= $(shell $(PKG_CONFIG) --libs libjxr 2>/dev/null || echo -ljxrglue -ljpegxr)

pkg_cflags = $(foreach pkg,$(1),$(shell $(PKG_CONFIG) --cflags $(pkg) 2>/dev/null))
pkg_libs = $(foreach pkg,$(1),$(shell $(PKG_CONFIG) --libs $(pkg) 2>/dev/null))

# Optional real AVM ML runtime, generated from the matching upstream build.
-include build/avm-runtime.mk

CODEC_CFLAGS := $(call pkg_cflags,$(CODEC_PKGS)) $(JXRLIB_CFLAGS)
CODEC_LIBS := $(call pkg_libs,$(CODEC_PKGS)) $(call pkg_libs,vulkan) $(JXRLIB_LIBS) $(AVM_TFLITE_LIBS) -ldl -lpthread $(LOCAL_RUNPATH)
GUI_CFLAGS := $(call pkg_cflags,$(GUI_PKGS))
GUI_LIBS := $(call pkg_libs,$(GUI_PKGS))

MODEL_SRCS := \
	gui/app_update.cpp \
	gui/viewer_model.cpp \
	gui/layout.cpp
METRIC_NATIVE_OBJS := build/metric_butteraugli.o build/metric_cvvdp.o build/metric_cvvdp_c.o build/gui_metric_perceptual.o
METRIC_OBJS := build/gui_metrics.o build/gui_raw_image_conversion.o $(METRIC_NATIVE_OBJS)

build/libcodec_vis_metrics.a: $(METRIC_OBJS)
	$(AR) rcs $@ $^

build/metric_butteraugli.o: third_party/butteraugli/butteraugli.cc third_party/butteraugli/butteraugli.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-fast-math $(DEPFLAGS) -c -o $@ $<
build/metric_cvvdp.o: third_party/fcvvdp/cvvdp.c third_party/fcvvdp/cvvdp.h third_party/fcvvdp/internal.h third_party/fcvvdp/lut.h | build
	$(CC) -O3 -std=c11 -fno-fast-math $(DEPFLAGS) -c -o $@ $<
build/metric_cvvdp_c.o: third_party/fcvvdp/cvvdp_c.c third_party/fcvvdp/cvvdp_c.h | build
	$(CC) -O3 -std=c11 -fno-fast-math $(DEPFLAGS) -c -o $@ $<
build/gui_metric_perceptual.o: gui/metric_perceptual.cpp gui/metric_perceptual.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-fast-math $(DEPFLAGS) -c -o $@ $<

IMAGE_IO_SRCS := codec_gui_image_io.cpp
CODEC_BACKEND_SRCS := \
	codec_gui_x265.cpp \
	codec_gui_vvenc.cpp \
	codec_gui_svt_av1.cpp \
	codec_gui_vaapi.cpp \
	codec_gui_uvg266.cpp \
	codec_gui_av2.cpp \
	codec_gui_still_formats.cpp \
	codec_gui_jpegxs.cpp \
	codec_gui_extra.cpp \
	codec_gui_svt_vp9.cpp \
	codec_avm_tflite_link_shims.cpp
ifneq ($(strip $(AVM_TFLITE_LIBS)),)
CODEC_BACKEND_SRCS := $(filter-out codec_avm_tflite_link_shims.cpp,$(CODEC_BACKEND_SRCS))
endif
ROUNDTRIP_CODEC_OBJS := $(patsubst %.cpp,build/roundtrip_%.o,$(CODEC_BACKEND_SRCS))

DAV2D_BRIDGE_OBJ := build/dav2d_bridge.o
SCC_DECODE_OBJS := build/native_h265_parser.o build/native_h265_vulkan.o build/gui_scc_decode.o
SCC_NATIVE_INCLUDES := -Ithird_party/native_video_decode/native/src -Ithird_party/native_video_decode/native/include

build/native_h265_parser.o: third_party/native_video_decode/native/src/h265/bitstream_parser.cpp | build
	$(CXX) $(CPPFLAGS) $(filter-out -std=%,$(CXXFLAGS)) -std=c++23 $(CODEC_CFLAGS) $(SCC_NATIVE_INCLUDES) $(DEPFLAGS) -c -o $@ $<
build/native_h265_vulkan.o: third_party/native_video_decode/native/src/h265/vulkan_video_decode.cpp | build
	$(CXX) $(CPPFLAGS) $(filter-out -std=%,$(CXXFLAGS)) -std=c++23 $(CODEC_CFLAGS) $(SCC_NATIVE_INCLUDES) $(DEPFLAGS) -c -o $@ $<
build/gui_scc_decode.o: gui/scc_decode.cpp gui/scc_decode.hpp | build
	$(CXX) $(CPPFLAGS) $(filter-out -std=%,$(CXXFLAGS)) -std=c++23 $(CODEC_CFLAGS) $(SCC_NATIVE_INCLUDES) $(DEPFLAGS) -c -o $@ $<

PREVIEW_DECODER_SRCS := gui/preview_decoders.cpp $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS)

.PHONY: all test clean portable-bundle test_cli_help test_incremental_dependencies test_gui_model test_draw_list test_ui_interaction test_ui_widgets test_wayland_window test_platform_portal test_text_shaper test_text_atlas test_encoder_backends test_vaapi_discovery test_app_commands test_encode_runner test_metrics test_signal_math test_image_ops test_raw_image_conversion test_settings test_storage

all: codec_vis_cli codec_vis_gui test_gui_model test_draw_list test_ui_interaction test_ui_widgets test_wayland_window test_platform_portal test_text_shaper test_text_atlas test_encoder_backends test_app_commands test_encode_runner test_metrics test_image_ops test_raw_image_conversion test_settings test_storage

test: test_workbench test_cli_help test_incremental_dependencies test_gui_model test_draw_list test_ui_interaction test_ui_widgets test_wayland_window test_platform_portal test_text_shaper test_text_atlas test_encoder_backends test_vaapi_discovery test_app_commands test_encode_runner test_metrics test_signal_math test_image_ops test_raw_image_conversion test_settings test_storage

.PHONY: test_repository test_heif_import test_decoder_workflow test_energy_access
test: test_repository test_heif_import test_decoder_workflow test_energy_access test_ui_framework test_live_preview test_image_size_target

test_repository:
	python3 scripts/check_repository.py

test_heif_import: build/test_heif_import
	./build/test_heif_import

test_decoder_workflow: build/test_decoder_workflow
	./build/test_decoder_workflow

test_energy_access: build/test_energy_access
	./build/test_energy_access

test_cli_help: codec_vis_cli
	./codec_vis_cli --help >/dev/null

test_incremental_dependencies: codec_vis_gui
	$(MAKE) -W gui/app_state.hpp -n codec_vis_gui | rg -c 'build/gui_wayland_window.o' | rg -q '^[1-9]'

test_gui_model: build/test_gui_model
	./build/test_gui_model

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | build
	wayland-scanner client-header $< $@

build/xdg-shell-protocol.c: $(XDG_SHELL_XML) | build
	wayland-scanner private-code $< $@

build/xdg-shell-protocol.o: build/xdg-shell-protocol.c build/xdg-shell-client-protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ build/xdg-shell-protocol.c

build/fractional-scale-v1-client-protocol.h: $(FRACTIONAL_SCALE_XML) | build
	wayland-scanner client-header $< $@

build/fractional-scale-v1-protocol.c: $(FRACTIONAL_SCALE_XML) | build
	wayland-scanner private-code $< $@

build/fractional-scale-v1-protocol.o: build/fractional-scale-v1-protocol.c build/fractional-scale-v1-client-protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ build/fractional-scale-v1-protocol.c

build/viewporter-client-protocol.h: $(VIEWPORTER_XML) | build
	wayland-scanner client-header $< $@

build/viewporter-protocol.c: $(VIEWPORTER_XML) | build
	wayland-scanner private-code $< $@

build/viewporter-protocol.o: build/viewporter-protocol.c build/viewporter-client-protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ build/viewporter-protocol.c

build/color-management-v1-client-protocol.h: $(COLOR_MANAGEMENT_XML) | build
	wayland-scanner client-header $< $@
build/color-management-v1-protocol.c: $(COLOR_MANAGEMENT_XML) | build
	wayland-scanner private-code $< $@
build/color-management-v1-protocol.o: build/color-management-v1-protocol.c build/color-management-v1-client-protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ $<
# Raw fonts and SPIR-V are objcopy objects in read-only ELF sections.
OBJCOPY ?= objcopy
UI_FONT ?= $(shell fc-match -f '%{file}' 'DejaVu Sans')
MONO_FONT ?= $(shell fc-match -f '%{file}' 'DejaVu Sans Mono')
EMBEDDED_FONTS := build/embedded_sans.o build/embedded_mono.o
EMBEDDED_SHADERS := build/embedded_compose.o build/embedded_linearize.o
EMBEDDED_ASSETS := $(EMBEDDED_FONTS) $(EMBEDDED_SHADERS)
build/embedded_fonts.hpp $(EMBEDDED_FONTS) &: scripts/embed_assets.py $(UI_FONT) $(MONO_FONT) | build
	python3 scripts/embed_assets.py --objcopy $(OBJCOPY) --target-machine '$(shell $(CXX) -dumpmachine)' --output build/embedded_fonts.hpp sans=$(UI_FONT) mono=$(MONO_FONT)
build/embedded_shaders.hpp $(EMBEDDED_SHADERS) &: scripts/embed_assets.py build/compose.comp.spv build/linearize.comp.spv | build
	python3 scripts/embed_assets.py --objcopy $(OBJCOPY) --target-machine '$(shell $(CXX) -dumpmachine)' --word-size 4 --output build/embedded_shaders.hpp compose=build/compose.comp.spv linearize=build/linearize.comp.spv
build/gui_render_vulkan.o build/gui_main.o: build/embedded_shaders.hpp
build/gui_text_shaper.o: build/embedded_fonts.hpp

build/compose.comp.spv: gui/shaders/compose.comp | build
	glslc -fshader-stage=comp $< -o $@

build/linearize.comp.spv: gui/shaders/linearize.comp | build
	glslc -fshader-stage=comp $< -o $@

build/test_gui_model: gui/test_gui_model.cpp $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_gui_model.cpp $(MODEL_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_draw_list: build/test_draw_list
	./build/test_draw_list

build/test_draw_list: build/embedded_fonts.hpp $(EMBEDDED_FONTS)

build/test_draw_list: gui/test_draw_list.cpp gui/draw_list.cpp gui/ui_widgets.cpp gui/text_atlas.cpp gui/text_shaper.cpp $(METRIC_OBJS) $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -o $@ gui/test_draw_list.cpp gui/draw_list.cpp gui/ui_widgets.cpp gui/text_atlas.cpp gui/text_shaper.cpp $(METRIC_OBJS) $(MODEL_SRCS) $(CODEC_LIBS) $(EMBEDDED_FONTS) $(GUI_LIBS) $(LDFLAGS)

test_ui_interaction: build/test_ui_interaction
	./build/test_ui_interaction

build/test_ui_interaction: gui/test_ui_interaction.cpp gui/ui_interaction.cpp $(METRIC_OBJS) $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_ui_interaction.cpp gui/ui_interaction.cpp $(METRIC_OBJS) $(MODEL_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_ui_widgets: build/test_ui_widgets
	./build/test_ui_widgets

build/test_ui_widgets: gui/test_ui_widgets.cpp gui/ui_widgets.cpp gui/image_list_model.hpp $(METRIC_OBJS) $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_ui_widgets.cpp gui/ui_widgets.cpp $(METRIC_OBJS) $(MODEL_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_wayland_window: build/test_wayland_window
	./build/test_wayland_window

build/test_wayland_window: gui/test_wayland_window.cpp gui/wayland_window.cpp gui/wayland_window.hpp $(WAYLAND_PROTOCOL_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -Ibuild -o $@ gui/test_wayland_window.cpp gui/wayland_window.cpp $(WAYLAND_PROTOCOL_OBJS) $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

test_platform_portal: build/test_platform_portal
	./build/test_platform_portal

build/test_platform_portal: gui/test_platform_portal.cpp gui/platform_portal.cpp gui/platform_portal.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -o $@ gui/test_platform_portal.cpp gui/platform_portal.cpp $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

test_text_shaper: build/test_text_shaper
	./build/test_text_shaper

build/test_text_shaper: build/embedded_fonts.hpp $(EMBEDDED_FONTS)

build/test_text_shaper: gui/test_text_shaper.cpp gui/text_shaper.cpp gui/text_shaper.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) -o $@ gui/test_text_shaper.cpp gui/text_shaper.cpp $(EMBEDDED_FONTS) $(GUI_LIBS) $(LDFLAGS)

test_text_atlas: build/test_text_atlas
	./build/test_text_atlas

build/test_text_atlas: build/embedded_fonts.hpp $(EMBEDDED_FONTS)

build/test_text_atlas: gui/test_text_atlas.cpp gui/text_atlas.cpp gui/text_shaper.cpp gui/text_atlas.hpp gui/text_shaper.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_text_atlas.cpp gui/text_atlas.cpp gui/text_shaper.cpp $(EMBEDDED_FONTS) $(GUI_LIBS) $(CODEC_LIBS) $(LDFLAGS)

test_encoder_backends: build/test_encoder_backends
	./build/test_encoder_backends

.PHONY: test_codec_integrity
test: test_codec_integrity
test_codec_integrity: build/test_codec_integrity
	./build/test_codec_integrity

build/test_codec_integrity: gui/test_codec_integrity.cpp build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)

test_vaapi_discovery: build/test_vaapi_discovery
	./build/test_vaapi_discovery

build/test_vaapi_discovery: gui/test_vaapi_discovery.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_vaapi_discovery.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(LDFLAGS)

build/test_encoder_backends: gui/test_encoder_backends.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_encoder_backends.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_app_commands: build/test_app_commands
	./build/test_app_commands

build/test_app_commands: gui/test_app_commands.cpp gui/app_commands.cpp gui/platform_portal.cpp $(METRIC_OBJS) gui/image_ops.cpp gui/encoder_backends.cpp $(PREVIEW_DECODER_SRCS) codec_gui_image_io.cpp $(MODEL_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -o $@ gui/test_app_commands.cpp gui/app_commands.cpp gui/platform_portal.cpp $(METRIC_OBJS) gui/image_ops.cpp gui/encoder_backends.cpp $(PREVIEW_DECODER_SRCS) codec_gui_image_io.cpp $(MODEL_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

test_encode_runner: build/test_encode_runner
	./build/test_encode_runner

build/test_encode_runner: gui/test_encode_runner.cpp build/gui_encode_runner.o build/gui_encoder_backends.o build/gui_preview_decoders.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(METRIC_OBJS) build/gui_app_state.o build/gui_viewer_model.o build/gui_layout.o $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.mk,$^) $(CODEC_LIBS) $(LDFLAGS)

test_metrics: build/test_metrics
	./build/test_metrics



build/test_metrics: gui/test_metrics.cpp $(METRIC_OBJS) gui/metrics.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_metrics.cpp $(METRIC_OBJS) $(CODEC_LIBS) $(LDFLAGS)

test_image_ops: build/test_image_ops
	./build/test_image_ops

build/test_image_ops: gui/test_image_ops.cpp gui/image_ops.cpp gui/raw_image_conversion.cpp gui/image_ops.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_image_ops.cpp gui/image_ops.cpp gui/raw_image_conversion.cpp $(CODEC_LIBS) $(LDFLAGS)

test_signal_math: build/test_signal_math
	./build/test_signal_math

build/test_signal_math: gui/test_signal_math.cpp gui/signal_math.hpp gui/raw_image_conversion.hpp gui/raw_image_utils.hpp build/gui_raw_image_conversion.o | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-fast-math -o $@ gui/test_signal_math.cpp build/gui_raw_image_conversion.o $(LDFLAGS)

test_raw_image_conversion: build/test_raw_image_conversion
	./build/test_raw_image_conversion

build/test_raw_image_conversion: gui/test_raw_image_conversion.cpp gui/raw_image_conversion.cpp gui/raw_image_conversion.hpp gui/raw_image_utils.hpp gui/signal_math.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_raw_image_conversion.cpp gui/raw_image_conversion.cpp $(CODEC_LIBS) $(LDFLAGS)

test_settings: build/test_settings
	./build/test_settings

build/test_settings: gui/test_settings.cpp gui/settings.cpp gui/settings.hpp gui/app_state.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_settings.cpp gui/settings.cpp $(CODEC_LIBS) $(LDFLAGS)

test_storage: build/test_storage
	./build/test_storage

build/test_storage: gui/test_storage.cpp gui/storage.hpp gui/app_state.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_storage.cpp $(CODEC_LIBS) $(LDFLAGS)

build/gui_app_state.o: gui/app_update.cpp gui/app_state.hpp gui/viewer_model.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/app_update.cpp

build/gui_app_commands.o: gui/app_commands.cpp gui/app_commands.hpp gui/app_state.hpp gui/encoder_backends.hpp gui/metrics.hpp gui/platform_portal.hpp codec_gui_image_io.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) $(DEPFLAGS) -c -o $@ gui/app_commands.cpp

build/gui_platform_portal.o: gui/platform_portal.cpp gui/platform_portal.hpp gui/app_state.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/platform_portal.cpp

build/gui_encode_runner.o: gui/encode_runner.cpp gui/encode_runner.hpp gui/app_state.hpp gui/encoder_backends.hpp gui/metrics.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/encode_runner.cpp

build/gui_metrics.o: gui/metrics.cpp gui/metrics.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -fno-fast-math $(DEPFLAGS) -c -o $@ gui/metrics.cpp

build/gui_image_ops.o: gui/image_ops.cpp gui/image_ops.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/image_ops.cpp

build/gui_viewer_model.o: gui/viewer_model.cpp gui/viewer_model.hpp gui/app_state.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/viewer_model.cpp

build/gui_layout.o: gui/layout.cpp gui/layout.hpp gui/app_state.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/layout.cpp

build/gui_draw_list.o: gui/draw_list.cpp gui/draw_list.hpp gui/app_state.hpp gui/layout.hpp gui/viewer_model.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/draw_list.cpp

build/gui_ui_interaction.o: gui/ui_interaction.cpp gui/ui_interaction.hpp gui/app_state.hpp gui/layout.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/ui_interaction.cpp

build/gui_ui_widgets.o: gui/ui_widgets.cpp gui/ui_widgets.hpp gui/app_state.hpp gui/layout.hpp gui/viewer_model.hpp gui/image_list_model.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/ui_widgets.cpp

build/gui_wayland_window.o: gui/wayland_window.cpp gui/wayland_window.hpp build/xdg-shell-client-protocol.h build/fractional-scale-v1-client-protocol.h build/viewporter-client-protocol.h build/color-management-v1-client-protocol.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ gui/wayland_window.cpp

build/gui_render_vulkan.o: gui/render_vulkan.cpp gui/render_vulkan.hpp gui/raw_image_conversion.hpp gui/wayland_window.hpp gui/draw_list.hpp gui/text_atlas.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ gui/render_vulkan.cpp

build/gui_raw_image_conversion.o: gui/raw_image_conversion.cpp gui/raw_image_conversion.hpp gui/raw_image_utils.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -fno-fast-math $(DEPFLAGS) -c -o $@ gui/raw_image_conversion.cpp

build/gui_text_shaper.o: gui/text_shaper.cpp gui/text_shaper.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) $(DEPFLAGS) -c -o $@ gui/text_shaper.cpp

build/gui_text_atlas.o: gui/text_atlas.cpp gui/text_atlas.hpp gui/text_shaper.hpp gui/draw_list.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/text_atlas.cpp

build/codec_gui_image_io.o: codec_gui_image_io.cpp codec_gui_image_io.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ codec_gui_image_io.cpp

build/gui_encoder_backends.o: gui/encoder_backends.cpp gui/encoder_backends.hpp gui/preview_decoders.hpp gui/app_state.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/encoder_backends.cpp

build/gui_preview_decoders.o: gui/preview_decoders.cpp gui/preview_decoders.hpp gui/encoder_backends.hpp gui/app_state.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/preview_decoders.cpp

$(DAV2D_BRIDGE_OBJ): gui/dav2d_bridge.c gui/dav2d_bridge.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CODEC_CFLAGS) -c -o $@ gui/dav2d_bridge.c

build/gui_main.o: gui/main_gui.cpp gui/app_state.hpp gui/app_commands.hpp gui/draw_list.hpp gui/layout.hpp gui/render_vulkan.hpp gui/text_atlas.hpp gui/text_shaper.hpp gui/ui_interaction.hpp gui/wayland_window.hpp gui/viewer_model.hpp codec_gui_image_io.hpp gui/encoder_backends.hpp gui/encode_runner.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ gui/main_gui.cpp

codec_vis_cli: main.cpp build/codec_gui_image_io.o build/gui_raw_image_conversion.o build/gui_preview_decoders.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.hpp %.mk,$^) $(CODEC_LIBS) $(LDFLAGS)

build/gui_embedded_energy_helper.o: gui/energy_access_helper.cpp gui/energy_access_protocol.hpp gui/energy_counter.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DCODEC_VIS_EMBED_ENERGY_HELPER $(DEPFLAGS) -c -o $@ $<

codec_vis_gui: $(EMBEDDED_ASSETS) build/gui_embedded_energy_helper.o build/gui_workbench.o build/gui_ui_framework.o build/gui_energy_access.o build/gui_decoder_registry.o build/gui_decoder_runner.o $(METRIC_NATIVE_OBJS) build/gui_main.o build/gui_app_state.o build/gui_app_commands.o build/gui_platform_portal.o build/gui_encode_runner.o build/gui_metrics.o build/gui_image_ops.o build/gui_raw_image_conversion.o build/gui_viewer_model.o build/gui_layout.o build/gui_draw_list.o build/gui_ui_interaction.o build/gui_ui_widgets.o build/gui_wayland_window.o build/gui_render_vulkan.o build/gui_text_shaper.o build/gui_text_atlas.o $(WAYLAND_PROTOCOL_OBJS) build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build/linearize.comp.spv build/compose.comp.spv
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.mk,$^) $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

portable-bundle: codec_vis_cli codec_vis_gui
	./scripts/make_portable_bundle.sh

clean:
	rm -rf build codec_vis_gui codec_vis_cli codec_vis_energy_helper

-include $(wildcard build/*.d)

# Isolated round-trip probes share codec objects so decoder fixes do not rebuild
# every encoder implementation for every test invocation.
build/roundtrip_%.o: %.cpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ $<

build/test_codec_roundtrips: gui/test_codec_roundtrips.cpp gui/encoder_param_state.hpp gui/raw_image_utils.hpp build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $< build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) $(CODEC_LIBS) $(LDFLAGS)

.PHONY: test_codec_roundtrips
.PHONY: test_hevc_large_nal
test_hevc_large_nal: build/test_codec_roundtrips
	python3 scripts/test_hevc_large_nal.py

.PHONY: test_additional_encoders
.PHONY: test_image_import
test: test_additional_encoders test_image_import
test_image_import: build/test_image_import
	./build/test_image_import

build/test_image_import: gui/test_image_import.cpp build/codec_gui_image_io.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)

build/test_heif_import: gui/test_heif_import.cpp build/gui_decoder_registry.o build/codec_gui_image_io.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)


test_additional_encoders: build/test_additional_encoders
	./build/test_additional_encoders

build/test_additional_encoders: gui/test_additional_encoders.cpp codec_gui_extra.hpp build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.hpp,$^) $(CODEC_LIBS) $(LDFLAGS)

test_codec_roundtrips: build/test_codec_roundtrips
	python3 scripts/test_codec_roundtrips.py

# Explicit GPU integration test; intentionally excluded from headless `test`.
build/test_vaapi_workflow: $(METRIC_NATIVE_OBJS) gui/test_vaapi_workflow.cpp gui/encode_runner.hpp build/codec_gui_image_io.o build/gui_encode_runner.o build/gui_metrics.o build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) $(MODEL_SRCS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.hpp %.mk,$^) $(CODEC_LIBS) $(LDFLAGS)

# Relink when a local real-runtime configuration is created or changed.
codec_vis_cli codec_vis_gui build/test_encoder_backends build/test_vaapi_discovery build/test_app_commands build/test_encode_runner build/test_codec_roundtrips: $(wildcard build/avm-runtime.mk)

.PHONY: test_jpegxs
test: test_jpegxs
test_jpegxs: build/test_jpegxs
	./build/test_jpegxs

build/test_jpegxs: gui/test_jpegxs.cpp codec_gui_jpegxs.cpp codec_gui_jpegxs.hpp codec_gui_x265.hpp gui/raw_image_utils.hpp gui/test_check.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(call pkg_cflags,SvtJpegxs) -o $@ gui/test_jpegxs.cpp codec_gui_jpegxs.cpp $(call pkg_libs,SvtJpegxs) $(LOCAL_RUNPATH) -lpthread $(LDFLAGS)


build/gui_decoder_registry.o: gui/decoder_registry.cpp gui/decoder_registry.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) $(DEPFLAGS) -c -o $@ $<

build/gui_decoder_runner.o: gui/decoder_runner.cpp gui/decoder_runner.hpp gui/decode_energy.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) $(DEPFLAGS) -c -o $@ $<

build/test_decoder_workflow: gui/test_decoder_workflow.cpp build/gui_decoder_registry.o build/gui_decoder_runner.o build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) $(MODEL_SRCS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)

# Authentication is separate from the unprivileged Wayland application.
codec_vis_energy_helper: gui/energy_access_helper.cpp gui/energy_access_protocol.hpp gui/energy_counter.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

build/gui_energy_access.o: gui/energy_access.cpp gui/energy_access.hpp gui/energy_access_protocol.hpp gui/energy_counter.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

build/test_energy_access: gui/test_energy_access.cpp gui/decode_energy.hpp gui/decoder_ui.hpp gui/energy_counter.hpp build/gui_energy_access.o build/gui_app_state.o build/gui_viewer_model.o build/gui_layout.o | codec_vis_energy_helper
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.hpp,$^) $(CODEC_LIBS) $(LDFLAGS)


build/gui_ui_framework.o: gui/ui_framework.cpp gui/ui_framework.hpp gui/ui_geometry.hpp gui/draw_list.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

build/test_ui_framework: gui/test_ui_framework.cpp gui/ui_framework.hpp gui/ui_render_frame.hpp gui/paint_scene.hpp build/gui_ui_framework.o | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter-out %.hpp,$^) $(LDFLAGS)

build/gui_framework_lab: $(EMBEDDED_ASSETS) gui/framework_lab.cpp build/gui_ui_framework.o build/gui_render_vulkan.o build/gui_wayland_window.o build/gui_text_shaper.o build/gui_text_atlas.o build/gui_raw_image_conversion.o $(WAYLAND_PROTOCOL_OBJS) | build/linearize.comp.spv build/compose.comp.spv
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)


build/test_live_preview: gui/test_live_preview.cpp build/gui_encode_runner.o build/gui_encoder_backends.o build/gui_preview_decoders.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) build/codec_gui_image_io.o build/gui_app_state.o build/gui_viewer_model.o build/gui_layout.o $(METRIC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)

.PHONY: test_live_preview
test_live_preview: build/test_live_preview
	./build/test_live_preview

.PHONY: test_ui_framework gui_framework_lab
test_ui_framework: build/test_ui_framework
	./build/test_ui_framework

gui_framework_lab: build/gui_framework_lab


build/gui_workbench.o: gui/workbench.cpp gui/workbench.hpp gui/ui_framework.hpp gui/encoder_param_state.hpp gui/metrics.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) $(DEPFLAGS) -c -o $@ gui/workbench.cpp

build/test_workbench: gui/test_workbench.cpp build/gui_workbench.o build/gui_ui_framework.o build/gui_app_state.o build/gui_layout.o build/gui_viewer_model.o $(METRIC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.mk,$^) $(CODEC_LIBS) $(LDFLAGS)

.PHONY: test_workbench
test_workbench: build/test_workbench build/test_workbench_navigation
	./build/test_workbench
	./build/test_workbench_navigation

build/test_workbench_navigation: gui/test_workbench_navigation.cpp build/gui_workbench.o build/gui_ui_framework.o build/gui_app_state.o build/gui_layout.o build/gui_viewer_model.o $(METRIC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $(filter-out %.mk,$^) $(CODEC_LIBS) $(LDFLAGS)


build/test_image_size_target: gui/test_image_size_target.cpp gui/image_size_target.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<
.PHONY: test_image_size_target
test_image_size_target: build/test_image_size_target
	./build/test_image_size_target

# Explicit hardware test; all SCC parsing and decoding remain in process.
build/test_scc_workflow: gui/test_scc_workflow.cpp build/gui_decoder_registry.o build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)
.PHONY: test_scc_workflow
test_scc_workflow: build/test_scc_workflow
	./build/test_scc_workflow

.PHONY: test_anamorphic
test: test_anamorphic
test_anamorphic: build/test_anamorphic
	./build/test_anamorphic

build/test_anamorphic: gui/test_anamorphic.cpp build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o build/gui_raw_image_conversion.o $(DAV2D_BRIDGE_OBJ) $(SCC_DECODE_OBJS) $(ROUNDTRIP_CODEC_OBJS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(LDFLAGS)
