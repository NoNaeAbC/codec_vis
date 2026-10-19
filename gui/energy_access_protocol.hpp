#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
namespace codec_gui::gui {
struct EnergyAccessPacket {
	uint64_t version = 1, range = 0;
	char domain[128]{};
	char error[256]{};
};
inline socklen_t energy_socket_address(const std::string& name, sockaddr_un& address) {
	if (!name.starts_with("codec-vis-energy-") || name.size() >= sizeof(address.sun_path) - 1)
		throw std::runtime_error("Invalid energy authorization socket");
	address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path + 1, name.data(), name.size());
	return offsetof(sockaddr_un, sun_path) + 1 + name.size();
}
} // namespace codec_gui::gui
