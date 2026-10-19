#include "decode_energy.hpp"
#include "decoder_ui.hpp"
#include "energy_access.hpp"
#include "test_check.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace codec_gui::gui;
#define CHECK CODEC_VIS_CHECK
static EnergyAccessResult await_result(EnergyAccess& access) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(125);
	while (std::chrono::steady_clock::now() < deadline) {
		if (auto result = access.take_result()) return std::move(*result);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error("Authorization worker timed out");
}
int main(int argc, char** argv) {
	AppState state;
	ImageObject image; image.id={1};image.decoder.queried=true;
	state.images.push_back(image);state.selection.selectedImage=image.id;state.decoderInspector=true;
	auto control = [](const AppState& s) {
		for (const auto& c : decoder_controls(s,{2520,40,360,1728})) if(c.id=="energy:access")return c;
		throw std::runtime_error("Missing energy control");
	};
	CHECK(control(state).label=="Enable energy...");
	auto result=update(state,control(state).action);
	CHECK(result.commands.size()==1 && result.commands[0].kind==CommandKind::AuthorizeEnergyAccess);
	state=std::move(result.state);CHECK(state.energyAccessPending);
	Action request;request.kind=ActionKind::RequestEnergyAccess;
	CHECK(update(state,request).commands.empty());
	CHECK(control(state).label=="Cancel energy request");
	result=update(state,control(state).action);
	CHECK(result.commands.size()==1 && result.commands[0].kind==CommandKind::CloseEnergyAccess);
	state=std::move(result.state);CHECK(!state.energyAccessPending && !state.energyAccessEnabled);
	Action ready;ready.kind=ActionKind::EnergyAccessReady;ready.value=1;ready.text="psys";
	CHECK(!update(state,ready).state.energyAccessEnabled); // Completion after cancellation is ignored.
	state=update(state,request).state;ready.value=0;ready.text="Authorization canceled";
	state=update(state,ready).state;CHECK(!state.energyAccessEnabled && !state.energyAccessPending);
	CHECK(state.energyAccessError==ready.text);
	state=update(state,request).state;ready.value=1;ready.text="psys";
	state=update(state,ready).state;CHECK(state.energyAccessEnabled && state.energyAccessError.empty());
	CHECK(control(state).label=="Disable energy");
	state=update(state,control(state).action).state;CHECK(!state.energyAccessEnabled);
	EnergyAccess access;
	access.request("/nonexistent-codec-vis-energy-helper");
	auto missing=await_result(access);CHECK(!missing.counter && !missing.error.empty());
	CHECK(!access.take_result());
	std::cout<<"Energy access state, cancellation, retry, stale completion and missing-helper handling passed\n";
	if (argc>1 && std::string(argv[1])=="--authorize") {
		CHECK(::geteuid()!=0);
		access.request(std::filesystem::absolute("codec_vis_energy_helper"));
		auto authorized=await_result(access);
		if (!authorized.counter)throw std::runtime_error(authorized.error);
		std::cout<<"Authorized read-only counter: "<<authorized.counter->domain<<'\n';
		CHECK((::fcntl(authorized.counter->fd,F_GETFL)&O_ACCMODE)==O_RDONLY);
		CHECK(::fcntl(authorized.counter->fd,F_GETFD)&FD_CLOEXEC);
		set_session_energy_counter(std::move(authorized.counter));
		access.cancel(); // The helper/worker is no longer needed to read the counter.
		DecodeEnergy ongoing;
		const auto before=ongoing.read();CHECK(before);
		set_session_energy_counter({}); // Releasing access cannot corrupt a running sample.
		CHECK(!DecodeEnergy{}.read());
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto after=ongoing.read();CHECK(after);
		auto joules=ongoing.joules(*before,*after);CHECK(joules && *joules>0);
		std::cout<<"In-process read after authorization helper exit and release: "<<*joules<<" J\n";
	}
}
