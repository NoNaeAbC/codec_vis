// codec_avm_tflite_link_shims.cpp
// AVM was built with optional TFLite pruning code. The still-image AV2 path here
// does not enable those tools, but the static archive still has references to
// TFLite/XNNPACK entry points. These no-op definitions satisfy the unused
// references without loading TensorFlow into the process.
#include <cstddef>
#include <memory>

struct TfLiteDelegate;
struct TfLiteRegistration;
struct TfLiteXNNPackDelegateOptions {
	int num_threads;
};

extern "C" TfLiteXNNPackDelegateOptions TfLiteXNNPackDelegateOptionsDefault() {
	return {};
}

extern "C" TfLiteDelegate* TfLiteXNNPackDelegateCreate(
	const TfLiteXNNPackDelegateOptions*
) {
	return nullptr;
}

extern "C" void TfLiteXNNPackDelegateDelete(TfLiteDelegate*) {}

extern "C" const unsigned char dip_pruning_tflite_qp85[] = {0};
extern "C" const unsigned char dip_pruning_tflite_qp110[] = {0};
extern "C" const unsigned char dip_pruning_tflite_qp135[] = {0};
extern "C" const unsigned char dip_pruning_tflite_qp160[] = {0};
extern "C" const unsigned char dip_pruning_tflite_qp185[] = {0};

namespace tflite {
enum BuiltinOperator : int {
	kBuiltinOperatorDummy = 0,
};

enum LogSeverity : int {
	kTfLiteInfo = 0,
};

class Allocation;
class ErrorReporter;
class InterpreterOptions;
class Model;

class OpResolver {
public:
	virtual ~OpResolver() = default;
};

class MutableOpResolver : public OpResolver {
public:
	~MutableOpResolver() override;

	virtual const TfLiteRegistration* FindOp(BuiltinOperator, int) const;
	virtual const TfLiteRegistration* FindOp(const char*, int) const;
	virtual bool MayContainUserDefinedOps() const;

	void AddBuiltin(BuiltinOperator, const TfLiteRegistration*, int);
};

MutableOpResolver::~MutableOpResolver() = default;

const TfLiteRegistration* MutableOpResolver::FindOp(BuiltinOperator, int) const {
	return nullptr;
}

const TfLiteRegistration* MutableOpResolver::FindOp(const char*, int) const {
	return nullptr;
}

bool MutableOpResolver::MayContainUserDefinedOps() const {
	return false;
}

void MutableOpResolver::AddBuiltin(BuiltinOperator, const TfLiteRegistration*, int) {}

class ErrorReporter {
public:
	int Report(const char*, ...);
};

int ErrorReporter::Report(const char*, ...) {
	return 0;
}

ErrorReporter* DefaultErrorReporter() {
	static ErrorReporter reporter;
	return &reporter;
}

class LoggerOptions {
public:
	static void SetMinimumLogSeverity(LogSeverity);
};

void LoggerOptions::SetMinimumLogSeverity(LogSeverity) {}

namespace impl {
class FlatBufferModel {
public:
	~FlatBufferModel();

	static std::unique_ptr<FlatBufferModel> BuildFromBuffer(
		const char*,
		std::size_t,
		ErrorReporter*
	);
};

FlatBufferModel::~FlatBufferModel() = default;

std::unique_ptr<FlatBufferModel> FlatBufferModel::BuildFromBuffer(
	const char*,
	std::size_t,
	ErrorReporter*
) {
	return nullptr;
}

class Interpreter {
public:
	~Interpreter();
	int AllocateTensors();
	int Invoke();
	int ModifyGraphWithDelegate(TfLiteDelegate*);
};

Interpreter::~Interpreter() = default;

int Interpreter::AllocateTensors() {
	return -1;
}

int Interpreter::Invoke() {
	return -1;
}

int Interpreter::ModifyGraphWithDelegate(TfLiteDelegate*) {
	return -1;
}

class InterpreterBuilder {
public:
	InterpreterBuilder(
		const Model*,
		const OpResolver&,
		ErrorReporter*,
		const InterpreterOptions*,
		const Allocation*
	);

	InterpreterBuilder(
		const FlatBufferModel&,
		const OpResolver&,
		const InterpreterOptions*
	);

	~InterpreterBuilder();

	int operator()(std::unique_ptr<Interpreter>*);
};

InterpreterBuilder::InterpreterBuilder(
	const Model*,
	const OpResolver&,
	ErrorReporter*,
	const InterpreterOptions*,
	const Allocation*
) {}

InterpreterBuilder::InterpreterBuilder(
	const FlatBufferModel&,
	const OpResolver&,
	const InterpreterOptions*
) {}

InterpreterBuilder::~InterpreterBuilder() = default;

int InterpreterBuilder::operator()(std::unique_ptr<Interpreter>* out) {
	if (out) {
		out->reset();
	}
	return -1;
}
} // namespace impl

namespace ops::builtin {
class BuiltinOpResolver : public MutableOpResolver {
public:
	BuiltinOpResolver();
	~BuiltinOpResolver() override = default;
};

BuiltinOpResolver::BuiltinOpResolver() = default;

const TfLiteRegistration* Register_CONCATENATION() {
	return nullptr;
}

const TfLiteRegistration* Register_FULLY_CONNECTED() {
	return nullptr;
}

const TfLiteRegistration* Register_LOGISTIC() {
	return nullptr;
}
} // namespace ops::builtin
} // namespace tflite
