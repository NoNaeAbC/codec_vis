// Opens only fixed sysfs RAPL attributes and passes one read-only FD, then exits.
// No chmod, shell, GUI, arbitrary paths, or long-lived privileged process.
#include "energy_access_protocol.hpp"
#include "energy_counter.hpp"
#include <cstdlib>
#include <iostream>
using namespace codec_gui::gui;
int codec_vis_energy_helper_main(int argc, char** argv) {
	if (argc != 2 || ::geteuid() != 0) return 2;
	const char* owner = std::getenv("PKEXEC_UID");
	uint32_t uid = 0;
	if (!owner) return 2;
	auto [end, error] = std::from_chars(owner, owner + std::strlen(owner), uid);
	if (error != std::errc{} || *end) return 2;
	try {
		sockaddr_un address{};
		const auto length = energy_socket_address(argv[1], address);
		int socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (socket < 0 || ::connect(socket, reinterpret_cast<sockaddr*>(&address), length) < 0) return 3;
		ucred peer{}; socklen_t size = sizeof(peer);
		if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || peer.uid != uid) return 3;
		EnergyAccessPacket packet;
		auto counter = open_energy_counter();
		if (counter) {
			packet.range = counter->range;
			std::snprintf(packet.domain, sizeof(packet.domain), "%s", counter->domain.c_str());
		} else std::snprintf(packet.error, sizeof(packet.error), "No usable RAPL counter is exposed by this system, even with authorization.");
		iovec io{&packet, sizeof(packet)};
		msghdr message{}; message.msg_iov = &io; message.msg_iovlen = 1;
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
		if (counter) {
			message.msg_control = control; message.msg_controllen = sizeof(control);
			auto* header = CMSG_FIRSTHDR(&message);
			header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
			std::memcpy(CMSG_DATA(header), &counter->fd, sizeof(int));
		}
		bool sent = ::sendmsg(socket, &message, MSG_NOSIGNAL) == sizeof(packet);
		::close(socket);
		return sent ? 0 : 4;
	} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 5; }
}

#ifndef CODEC_VIS_EMBED_ENERGY_HELPER
int main(int argc, char** argv) { return codec_vis_energy_helper_main(argc, argv); }
#endif
