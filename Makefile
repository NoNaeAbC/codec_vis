CXX ?= g++
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

CODEC_PKGS := x265 libjpeg libraw libvvenc libvvdec uvg266 SvtAv1Enc libva libva-drm avm libde265 dav1d dav2d charls libpng16 libjxl libjxl_threads libopenjp2 x264 openh264 libvmaf
GUI_PKGS := wayland-client vulkan harfbuzz freetype2 fontconfig sdbus-c++ xkbcommon
XDG_SHELL_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
FRACTIONAL_SCALE_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/staging/fractional-scale/fractional-scale-v1.xml
VIEWPORTER_XML := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/viewporter/viewporter.xml
WAYLAND_PROTOCOL_OBJS := build/xdg-shell-protocol.o build/fractional-scale-v1-protocol.o build/viewporter-protocol.o

JXRLIB_CFLAGS ?= -I/usr/include/jxrlib
JXRLIB_LIBS ?= -ljxrglue -ljpegxr

pkg_cflags = $(foreach pkg,$(1),$(shell $(PKG_CONFIG) --cflags $(pkg) 2>/dev/null))
pkg_libs = $(foreach pkg,$(1),$(shell $(PKG_CONFIG) --libs $(pkg) 2>/dev/null))

CODEC_CFLAGS := $(call pkg_cflags,$(CODEC_PKGS)) $(JXRLIB_CFLAGS)
CODEC_LIBS := $(call pkg_libs,$(CODEC_PKGS)) $(JXRLIB_LIBS) -ldl -lpthread $(LOCAL_RUNPATH)
GUI_CFLAGS := $(call pkg_cflags,$(GUI_PKGS))
GUI_LIBS := $(call pkg_libs,$(GUI_PKGS))

MODEL_SRCS := \
	gui/app_update.cpp \
	gui/viewer_model.cpp \
	gui/layout.cpp
METRIC_SRCS := gui/metrics.cpp gui/raw_image_conversion.cpp

IMAGE_IO_SRCS := codec_gui_image_io.cpp
CODEC_BACKEND_SRCS := \
	codec_gui_x265.cpp \
	codec_gui_vvenc.cpp \
	codec_gui_svt_av1.cpp \
	codec_gui_vaapi.cpp \
	codec_gui_uvg266.cpp \
	codec_gui_av2.cpp \
	codec_gui_still_formats.cpp \
	codec_avm_tflite_link_shims.cpp
DAV2D_BRIDGE_OBJ := build/dav2d_bridge.o
PREVIEW_DECODER_SRCS := gui/preview_decoders.cpp $(DAV2D_BRIDGE_OBJ)

.PHONY: all test clean test_cli_help test_incremental_dependencies test_gui_model test_draw_list test_ui_interaction test_ui_widgets test_wayland_window test_platform_portal test_text_shaper test_text_atlas test_encoder_backends test_vaapi_discovery test_app_commands test_encode_runner test_metrics test_image_ops test_raw_image_conversion test_settings test_storage

all: codec_vis_cli codec_vis_gui test_gui_model test_draw_list test_ui_interaction test_ui_widgets test_wayland_window test_platform_portal test_text_shaper test_text_atlas test_encoder_backends test_app_commands test_encode_runner test_metrics test_image_ops test_raw_image_conversion test_settings test_storage

test: test_cli_help test_incremental_dependencies test_gui_model test_draw_list test_ui_interaction test_ui_widgets test_wayland_window test_platform_portal test_text_shaper test_text_atlas test_encoder_backends test_vaapi_discovery test_app_commands test_encode_runner test_metrics test_image_ops test_raw_image_conversion test_settings test_storage

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

build/rect.vert.spv: gui/shaders/rect.vert | build
	glslc -fshader-stage=vert $< -o $@

build/rect.frag.spv: gui/shaders/rect.frag | build
	glslc -fshader-stage=frag $< -o $@

build/text.vert.spv: gui/shaders/text.vert | build
	glslc -fshader-stage=vert $< -o $@

build/text.frag.spv: gui/shaders/text.frag | build
	glslc -fshader-stage=frag $< -o $@

build/image.frag.spv: gui/shaders/image.frag | build
	glslc -fshader-stage=frag $< -o $@

build/test_gui_model: gui/test_gui_model.cpp $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_gui_model.cpp $(MODEL_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_draw_list: build/test_draw_list
	./build/test_draw_list

build/test_draw_list: gui/test_draw_list.cpp gui/draw_list.cpp gui/ui_widgets.cpp gui/text_atlas.cpp gui/text_shaper.cpp $(METRIC_SRCS) $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -o $@ gui/test_draw_list.cpp gui/draw_list.cpp gui/ui_widgets.cpp gui/text_atlas.cpp gui/text_shaper.cpp $(METRIC_SRCS) $(MODEL_SRCS) $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

test_ui_interaction: build/test_ui_interaction
	./build/test_ui_interaction

build/test_ui_interaction: gui/test_ui_interaction.cpp gui/ui_interaction.cpp $(METRIC_SRCS) $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_ui_interaction.cpp gui/ui_interaction.cpp $(METRIC_SRCS) $(MODEL_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_ui_widgets: build/test_ui_widgets
	./build/test_ui_widgets

build/test_ui_widgets: gui/test_ui_widgets.cpp gui/ui_widgets.cpp gui/image_list_model.hpp $(METRIC_SRCS) $(MODEL_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_ui_widgets.cpp gui/ui_widgets.cpp $(METRIC_SRCS) $(MODEL_SRCS) $(CODEC_LIBS) $(LDFLAGS)

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

build/test_text_shaper: gui/test_text_shaper.cpp gui/text_shaper.cpp gui/text_shaper.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) -o $@ gui/test_text_shaper.cpp gui/text_shaper.cpp $(GUI_LIBS) $(LDFLAGS)

test_text_atlas: build/test_text_atlas
	./build/test_text_atlas

build/test_text_atlas: gui/test_text_atlas.cpp gui/text_atlas.cpp gui/text_shaper.cpp gui/text_atlas.hpp gui/text_shaper.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_text_atlas.cpp gui/text_atlas.cpp gui/text_shaper.cpp $(GUI_LIBS) $(CODEC_LIBS) $(LDFLAGS)

test_encoder_backends: build/test_encoder_backends
	./build/test_encoder_backends

test_vaapi_discovery: build/test_vaapi_discovery
	./build/test_vaapi_discovery

build/test_vaapi_discovery: gui/test_vaapi_discovery.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_vaapi_discovery.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(LDFLAGS)

build/test_encoder_backends: gui/test_encoder_backends.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_encoder_backends.cpp gui/encoder_backends.cpp gui/raw_image_conversion.cpp $(PREVIEW_DECODER_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_app_commands: build/test_app_commands
	./build/test_app_commands

build/test_app_commands: gui/test_app_commands.cpp gui/app_commands.cpp gui/platform_portal.cpp $(METRIC_SRCS) gui/image_ops.cpp gui/encoder_backends.cpp $(PREVIEW_DECODER_SRCS) codec_gui_image_io.cpp $(MODEL_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(GUI_CFLAGS) -o $@ gui/test_app_commands.cpp gui/app_commands.cpp gui/platform_portal.cpp $(METRIC_SRCS) gui/image_ops.cpp gui/encoder_backends.cpp $(PREVIEW_DECODER_SRCS) codec_gui_image_io.cpp $(MODEL_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

test_encode_runner: build/test_encode_runner
	./build/test_encode_runner

build/test_encode_runner: gui/test_encode_runner.cpp gui/encode_runner.cpp gui/encoder_backends.cpp $(PREVIEW_DECODER_SRCS) $(METRIC_SRCS) $(MODEL_SRCS) $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_encode_runner.cpp gui/encode_runner.cpp gui/encoder_backends.cpp $(PREVIEW_DECODER_SRCS) $(METRIC_SRCS) $(MODEL_SRCS) $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_metrics: build/test_metrics
	./build/test_metrics

build/test_metrics: gui/test_metrics.cpp $(METRIC_SRCS) gui/metrics.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_metrics.cpp $(METRIC_SRCS) $(CODEC_LIBS) $(LDFLAGS)

test_image_ops: build/test_image_ops
	./build/test_image_ops

build/test_image_ops: gui/test_image_ops.cpp gui/image_ops.cpp gui/image_ops.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ gui/test_image_ops.cpp gui/image_ops.cpp $(CODEC_LIBS) $(LDFLAGS)

test_raw_image_conversion: build/test_raw_image_conversion
	./build/test_raw_image_conversion

build/test_raw_image_conversion: gui/test_raw_image_conversion.cpp gui/raw_image_conversion.cpp gui/raw_image_conversion.hpp gui/raw_image_utils.hpp codec_gui_x265.hpp | build
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
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/metrics.cpp

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

build/gui_wayland_window.o: gui/wayland_window.cpp gui/wayland_window.hpp build/xdg-shell-client-protocol.h build/fractional-scale-v1-client-protocol.h build/viewporter-client-protocol.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ gui/wayland_window.cpp

build/gui_render_vulkan.o: gui/render_vulkan.cpp gui/render_vulkan.hpp gui/raw_image_conversion.hpp gui/wayland_window.hpp gui/draw_list.hpp gui/text_atlas.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GUI_CFLAGS) -Ibuild $(DEPFLAGS) -c -o $@ gui/render_vulkan.cpp

build/gui_raw_image_conversion.o: gui/raw_image_conversion.cpp gui/raw_image_conversion.hpp gui/raw_image_utils.hpp codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) $(DEPFLAGS) -c -o $@ gui/raw_image_conversion.cpp

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

codec_vis_cli: main.cpp $(CODEC_BACKEND_SRCS) codec_gui_x265.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ main.cpp $(CODEC_BACKEND_SRCS) $(CODEC_LIBS) $(LDFLAGS)

codec_vis_gui: build/gui_main.o build/gui_app_state.o build/gui_app_commands.o build/gui_platform_portal.o build/gui_encode_runner.o build/gui_metrics.o build/gui_image_ops.o build/gui_raw_image_conversion.o build/gui_viewer_model.o build/gui_layout.o build/gui_draw_list.o build/gui_ui_interaction.o build/gui_ui_widgets.o build/gui_wayland_window.o build/gui_render_vulkan.o build/gui_text_shaper.o build/gui_text_atlas.o $(WAYLAND_PROTOCOL_OBJS) build/codec_gui_image_io.o build/gui_encoder_backends.o build/gui_preview_decoders.o $(DAV2D_BRIDGE_OBJ) $(CODEC_BACKEND_SRCS) | build/rect.vert.spv build/rect.frag.spv build/text.vert.spv build/text.frag.spv build/image.frag.spv
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CODEC_CFLAGS) -o $@ $^ $(CODEC_LIBS) $(GUI_LIBS) $(LDFLAGS)

clean:
	rm -rf build codec_vis_gui codec_vis_cli

-include $(wildcard build/*.d)
