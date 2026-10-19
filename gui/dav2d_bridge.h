#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CodecVisDav2dPicture { // NOLINT(modernize-use-using): public C ABI
	int width;
	int height;
	int layout;
	int bit_depth;
	int color_primaries;
	int transfer;
	int matrix;
	int full_range;
	uint8_t* planes[3];
	int strides[3];
} CodecVisDav2dPicture;

int codec_vis_dav2d_decode(
	const uint8_t* data,
	size_t size,
	CodecVisDav2dPicture* output,
	char* error,
	size_t error_size
);
void codec_vis_dav2d_picture_free(CodecVisDav2dPicture* picture);

#ifdef __cplusplus
}
#endif
