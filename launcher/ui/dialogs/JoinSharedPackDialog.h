// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>
#include <optional>

#include "sharedpack/SharedPackInvite.h"
#include "sharedpack/SharedPackManifest.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

/// "Join Shared Pack" (pickle-sharedpack): the member pastes an invite, "Check" fetches the pack's newest revision and shows what it is (name,
/// Minecraft, loader, size, changelog), "Join" creates a normal instance from it. Nothing is created before "Join".
class JoinSharedPackDialog final : public QDialog {
    Q_OBJECT

   public:
    explicit JoinSharedPackDialog(QWidget* parent);

    /// The instance that was created, empty when the dialog was cancelled.
    QString createdInstanceId() const { return m_instanceId; }

   private:
    void inviteChanged();
    void check();
    void join();
    void showManifest(const SharedPack::Manifest& manifest);

    QLineEdit* m_invite = nullptr;
    QPushButton* m_check = nullptr;
    QPushButton* m_join = nullptr;
    QLabel* m_summary = nullptr;
    QPlainTextEdit* m_changelog = nullptr;
    QCheckBox* m_autoUpdate = nullptr;

    std::optional<SharedPack::Invite> m_parsed;
    std::optional<SharedPack::Manifest> m_manifest;
    QString m_instanceId;
};
