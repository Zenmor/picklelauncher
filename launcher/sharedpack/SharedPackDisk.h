// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

#include "SharedPackPlan.h"

namespace SharedPack {

/// Lower-case hex SHA-512 of a file, empty when it cannot be read.
QString hashFile(const QString& absolutePath);

/// Whether \a absolutePath is itself a symlink, junction or other reparse point (not what it points to). False when it does not exist.
bool isLinkOrReparsePoint(const QString& absolutePath);

/// True when every folder between \a gameDir and the file \a relativePath is a real directory or missing. A symlinked "mods" folder would
/// make a write land somewhere else, so such a path is never touched.
bool ancestorsAreSafe(const QString& gameDir, const QString& relativePath);

/// Looks at \a relativePaths (manifest and state paths) in \a gameDir without following links and without changing anything. Missing files
/// are left out of the result; a directory, symlink, reparse point, file below a link or unreadable file becomes DiskKind::Unsafe.
DiskState scanDisk(const QString& gameDir, const QStringList& relativePaths);

}  // namespace SharedPack
