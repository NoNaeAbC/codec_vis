#include "energy_access.hpp"
#include "energy_access_protocol.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/random.h>
#include <sys/wait.h>
extern char** environ;
namespace codec_gui::gui {
namespace {
struct Fd {
	int value = -1;
	explicit Fd(int fd) : value(fd) {}
	~Fd() { if (value >= 0) ::close(value); }
};
struct Child {
	pid_t pid = -1;
	~Child() {
		if (pid <= 0) return;
		// Do not hold up the GUI on an outstanding authentication dialog.
		::kill(pid, SIGTERM);
		std::thread([child = pid] { int status; while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {} }).detach();
	}
};
EnergyAccessResult authorize(std::stop_token stop, std::filesystem::path helper) {
	try {
		if (helper.empty()) helper = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "codec_vis_energy_helper";
		if (!std::filesystem::is_regular_file(helper))
			return {{}, "Energy helper is missing. Rebuild or reinstall codec_vis."};
		std::array<unsigned char, 16> random{};
		if (::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size()))
			throw std::runtime_error("Cannot create an energy authorization socket");
		std::ostringstream name;
		name << "codec-vis-energy-" << ::getpid() << '-';
		for (auto v : random) name << std::hex << std::setw(2) << std::setfill('0') << unsigned(v);
		sockaddr_un address{};
		const auto addressLength = energy_socket_address(name.str(), address);
		Fd listener(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
		if (listener.value < 0 || ::bind(listener.value, reinterpret_cast<sockaddr*>(&address), addressLength) < 0 ||
			::listen(listener.value, 4) < 0)
			throw std::runtime_error("Cannot listen for energy authorization");
		std::string helperPath = helper.string(), socketName = name.str();
		char program[] = "/usr/bin/pkexec", noAgent[] = "--disable-internal-agent";
		char* args[] = {program, noAgent, helperPath.data(), socketName.data(), nullptr};
		Child child;
		if (::posix_spawn(&child.pid, program, nullptr, nullptr, args, environ) != 0) {
			child.pid = -1;
			return {{}, "Cannot start authorization. Install polkit and a graphical authentication agent."};
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
			pollfd pending{listener.value, POLLIN, 0};
			if (::poll(&pending, 1, 100) > 0 && (pending.revents & POLLIN)) {
				Fd socket(::accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
				ucred peer{}; socklen_t size = sizeof(peer);
				if (socket.value < 0 || ::getsockopt(socket.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 ||
					peer.uid != 0 || peer.pid != child.pid) continue;
				pollfd ready{socket.value, POLLIN, 0};
				if (::poll(&ready, 1, 1000) <= 0) continue;
				EnergyAccessPacket packet;
				iovec io{&packet, sizeof(packet)};
				alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)]{};
				msghdr message{}; message.msg_iov = &io; message.msg_iovlen = 1;
				message.msg_control = control; message.msg_controllen = sizeof(control);
				const auto bytes = ::recvmsg(socket.value, &message, MSG_CMSG_CLOEXEC);
				auto counter = std::make_shared<EnergyCounter>();
				int count = 0;
				for (auto* h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
					if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS || h->cmsg_len < CMSG_LEN(0)) continue;
					for (size_t offset = 0; offset + sizeof(int) <= h->cmsg_len - CMSG_LEN(0); offset += sizeof(int)) {
						int received; std::memcpy(&received, CMSG_DATA(h) + offset, sizeof(int));
						if (++count == 1) counter->fd = received; else ::close(received);
					}
				}
				if (bytes != sizeof(packet) || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) || packet.version != 1 ||
					!std::memchr(packet.domain, 0, sizeof(packet.domain)) || !std::memchr(packet.error, 0, sizeof(packet.error)))
					throw std::runtime_error("Invalid energy authorization response");
				if (*packet.error) return {{}, packet.error};
				const auto flags = ::fcntl(counter->fd, F_GETFL);
				auto reading = read_energy_counter(counter->fd);
				if (count != 1 || flags < 0 || (flags & O_ACCMODE) != O_RDONLY || !packet.range || !reading || *reading >= packet.range)
					throw std::runtime_error("Authorized energy counter cannot be read");
				counter->range = packet.range; counter->domain = packet.domain;
				return {std::move(counter), {}};
			}
			int status = 0;
			if (::waitpid(child.pid, &status, WNOHANG) == child.pid) {
				child.pid = -1;
				return {{}, "Energy authorization was canceled or failed. Timing remains available. Check that a polkit authentication agent is running."};
			}
		}
		return {{}, stop.stop_requested() ? "Energy authorization canceled." : "Energy authorization timed out. You can try again."};
	} catch (const std::exception& e) { return {{}, e.what()}; }
}
} // namespace
void EnergyAccess::request(const std::filesystem::path& helper) {
	cancel();
	worker_ = std::jthread([this, helper](std::stop_token stop) {
		auto result = authorize(stop, helper);
		std::lock_guard lock(mutex_);
		if (!stop.stop_requested()) result_ = std::move(result);
	});
}
void EnergyAccess::cancel() {
	if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
	std::lock_guard lock(mutex_); result_.reset();
}
std::optional<EnergyAccessResult> EnergyAccess::take_result() {
	std::lock_guard lock(mutex_);
	auto result = std::move(result_); result_.reset(); return result;
}
} // namespace codec_gui::gui
