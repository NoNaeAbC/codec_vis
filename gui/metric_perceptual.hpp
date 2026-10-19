#pragma once
#include "raw_image_conversion.hpp"
namespace codec_gui::gui {
double metric_butteraugli(const LinearBt2020Image&, const LinearBt2020Image&);
double metric_cvvdp(const LinearBt2020Image&, const LinearBt2020Image&);
}
