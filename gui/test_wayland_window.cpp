#include "test_check.hpp"
#include "wayland_window.hpp"

#include <cassert>
#include <filesystem>
#include <string>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	const std::string plain = (root / "plain.png").string();
	CODEC_VIS_CHECK(decode_wayland_file_uri("file://" + (root / "frog.png").string()) == (root / "frog.png").string());
	CODEC_VIS_CHECK(decode_wayland_file_uri("file://" + (root / "frog%20image%23a.png").string()) == (root / "frog image#a.png").string());
	CODEC_VIS_CHECK(decode_wayland_file_uri(plain) == plain);

	const std::string list = "# comment\r\n"
	                         "\r\n"
	                         "file://" + (root / "first%20image.png").string() + "\r\n"
	                         "file://" + (root / "second.png").string() + "\r\n";
	CODEC_VIS_CHECK(first_uri_from_uri_list(list) == "file://" + (root / "first%20image.png").string());
	CODEC_VIS_CHECK(first_uri_from_uri_list("# only comments\n").empty());
	return 0;
}
