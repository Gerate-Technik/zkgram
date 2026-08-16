// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QString>
#include "codegen/common/cpp_file.h"

namespace codegen {
namespace lang {

bool WriteSubsets(
	const QString &genPath,
	const QString &sourcesPath,
	const common::ProjectInfo &project);

} // namespace lang
} // namespace codegen
