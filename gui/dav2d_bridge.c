#include "dav2d_bridge.h"

#include <dav2d/dav2d.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char* output, size_t size, const char* operation, int code) {
	if (output == NULL || size == 0) return;
	if (code < 0) snprintf(output, size, "%s: %s", operation, strerror(-code));
	else snprintf(output, size, "%s", operation);
}

void codec_vis_dav2d_picture_free(CodecVisDav2dPicture* picture) {
	if (picture == NULL) return;
	for (int plane = 0; plane < 3; ++plane) {
		free(picture->planes[plane]);
		picture->planes[plane] = NULL;
		picture->strides[plane] = 0;
	}
}

static int copy_picture(const Dav2dPicture* source, CodecVisDav2dPicture* output, char* error, size_t error_size) {
	output->width = source->p.w;
	output->height = source->p.h;
	output->layout = source->p.layout;
	output->bit_depth = source->p.bpc;
	const int sample_bytes = source->p.bpc > 8 ? 2 : 1;
	const int plane_count = source->p.layout == DAV2D_PIXEL_LAYOUT_I400 ? 1 : 3;
	for (int plane = 0; plane < plane_count; ++plane) {
		const int width = plane == 0 || source->p.layout == DAV2D_PIXEL_LAYOUT_I444 ? source->p.w : (source->p.w + 1) / 2;
		const int height = plane == 0 || source->p.layout == DAV2D_PIXEL_LAYOUT_I444 || source->p.layout == DAV2D_PIXEL_LAYOUT_I422 ? source->p.h : (source->p.h + 1) / 2;
		const int row_bytes = width * sample_bytes;
		const ptrdiff_t source_stride = plane == 0 ? source->stride[0] : source->stride[1];
		if (source->data[plane] == NULL || labs(source_stride) < row_bytes) {
			set_error(error, error_size, "dav2d returned an incomplete plane", 0);
			codec_vis_dav2d_picture_free(output);
			return -1;
		}
		output->planes[plane] = malloc((size_t)row_bytes * (size_t)height);
		if (output->planes[plane] == NULL) {
			set_error(error, error_size, "allocating decoded AV2 plane failed", -ENOMEM);
			codec_vis_dav2d_picture_free(output);
			return -1;
		}
		output->strides[plane] = row_bytes;
		for (int y = 0; y < height; ++y) {
			memcpy(output->planes[plane] + (size_t)y * (size_t)row_bytes,
			       (const uint8_t*)source->data[plane] + (ptrdiff_t)y * source_stride,
			       (size_t)row_bytes);
		}
	}
	return 0;
}

int codec_vis_dav2d_decode(const uint8_t* bytes, size_t size, CodecVisDav2dPicture* output, char* error, size_t error_size) {
	if (bytes == NULL || size == 0 || output == NULL) {
		set_error(error, error_size, "invalid dav2d input", -EINVAL);
		return -1;
	}
	memset(output, 0, sizeof(*output));
	Dav2dSettings settings;
	dav2d_default_settings(&settings);
	settings.n_threads = 1;
	settings.max_frame_delay = 1;
	settings.apply_grain = 0;
	settings.strict_std_compliance = 1;
	Dav2dContext* context = NULL;
	int result = dav2d_open(&context, &settings);
	if (result < 0) {
		set_error(error, error_size, "dav2d_open failed", result);
		return -1;
	}
	Dav2dData data = {0};
	uint8_t* destination = dav2d_data_create(&data, size);
	if (destination == NULL) {
		set_error(error, error_size, "dav2d input allocation failed", -ENOMEM);
		dav2d_close(&context);
		return -1;
	}
	memcpy(destination, bytes, size);
	Dav2dPicture picture = {0};
	while (data.sz > 0) {
		result = dav2d_send_data(context, &data);
		if (result == DAV2D_ERR(EAGAIN)) {
			result = dav2d_get_picture(context, &picture);
			if (result == 0) goto have_picture;
			if (result != DAV2D_ERR(EAGAIN)) goto fail_picture;
			continue;
		}
		if (result < 0) {
			set_error(error, error_size, "dav2d_send_data failed", result);
			goto fail;
		}
	}
	(void)dav2d_send_data(context, NULL);
	for (;;) {
		result = dav2d_get_picture(context, &picture);
		if (result == 0) break;
		if (result == DAV2D_ERR(EAGAIN) || result == DAV2D_EOF) {
			set_error(error, error_size, "dav2d produced no picture", 0);
			goto fail;
		}
		goto fail_picture;
	}
have_picture:
	result = copy_picture(&picture, output, error, error_size);
	dav2d_picture_unref(&picture);
	dav2d_data_unref(&data);
	dav2d_close(&context);
	return result;
fail_picture:
	set_error(error, error_size, "dav2d_get_picture failed", result);
fail:
	dav2d_data_unref(&data);
	dav2d_close(&context);
	return -1;
}
