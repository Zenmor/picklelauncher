// SPDX-License-Identifier: GPL-3.0-only
//
// picklelauncher - the widgets of Shared Packs that more than one place needs (pickle-sharedpack).
// Everything with logic is in launcher/sharedpack/ and tested there; this file only asks, shows and starts tasks.

#pragma once

#include <QMessageBox>
#include <QString>

#include "sharedpack/SharedPackApply.h"
#include "sharedpack/SharedPackManifest.h"

class QWidget;

namespace SharedPackUi {

/// A message box that shows \a text as plain text. Text from a pack (changelog, names, file paths) is written by the owner: Qt's
/// auto-detection would render HTML from it, with clickable links. The box deletes itself when closed.
void showMessage(QWidget* parent, const QString& title, const QString& text, QMessageBox::Icon icon);

/// Tells the member what an update left for them (.packnew files, paths that were not written). Nothing when there is nothing to tell.
void reportResult(QWidget* parent, const QString& instanceName, const SharedPack::ApplyResult& result);

/// Updates instance \a instanceId to \a manifest with a progress dialog and reports the outcome. False when nothing was installed.
bool runUpdate(QWidget* parent, const QString& instanceId, const SharedPack::Manifest& manifest);

/// "Publish Shared Pack": the owner's dialog for instance \a instanceId (first publish, new revision, Rotate Link).
void publish(QWidget* parent, const QString& instanceId);

/// "Join Shared Pack": asks for an invite, shows what it holds, creates the instance and selects it.
void join(QWidget* parent);

/// The start-up round: checks every pack the launcher follows, installs what is set to auto-update and asks about the rest.
/// Call once, after the instance list is loaded; \a window owns everything that is created.
void start(QWidget* window);

}  // namespace SharedPackUi
