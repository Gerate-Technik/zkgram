// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "codegen/lang/processor.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include "codegen/common/cpp_file.h"
#include "codegen/lang/parsed_file.h"
#include "codegen/lang/generator.h"
#include "codegen/lang/subsets.h"

namespace codegen {
namespace lang {
namespace {

constexpr int kErrorCantWritePath = 821;

[[nodiscard]] common::ProjectInfo Project(const QString &inputPath) {
	return {
		"codegen_style",
		QFileInfo(inputPath).fileName(),
		false,
	};
}

} // namespace

Processor::Processor(const Options &options)
: parser_(std::make_unique<ParsedFile>(options))
, options_(options) {
}

int Processor::launch() {
	if (options_.subsetsOnly) {
		return WriteSubsets(
			options_.outputPath,
			options_.sourcesPath,
			Project(options_.inputPath)) ? 0 : -1;
	}

	if (!parser_->read()) {
		return -1;
	}

	if (!write(parser_->getResult())) {
		return -1;
	}

	return 0;
}

bool Processor::write(const LangPack &langpack) const {
	QDir dir(options_.outputPath);
	if (!dir.mkpath(".")) {
		common::logError(kErrorCantWritePath, "Command Line") << "can not open path for writing: " << dir.absolutePath().toStdString();
		return false;
	}

	QString dstFilePath = dir.absolutePath() + "/lang_auto";

	const auto project = Project(options_.inputPath);

	Generator generator(langpack, dstFilePath, project);
	if (!generator.writeHeader()
		|| !generator.writeCounts()
		|| !generator.writeAllKeys()
		|| !generator.writeSource()
		|| !common::TouchTimestamp(dstFilePath)) {
		return false;
	}
	return true;
}

Processor::~Processor() = default;

} // namespace lang
} // namespace codegen
