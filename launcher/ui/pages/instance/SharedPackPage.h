// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>

#include "ManagedPackPage.h"
#include "sharedpack/SharedPackLink.h"
#include "sharedpack/SharedPackManifest.h"

class QCheckBox;
class QPushButton;

/// The instance page of a shared pack (ManagedPackType "pickleshared"). Reuses the layout of the managed pack page: the changelog box shows
/// what the next revision brings, "Update Pack" installs it. Below it: the member's auto-update choice, copying the invite and leaving
/// the pack (the instance and its files stay, it just stops following).
class SharedPackPage final : public ManagedPackPage {
    Q_OBJECT

   public:
    SharedPackPage(BaseInstance* inst, QWidget* parent = nullptr);
    ~SharedPackPage() override = default;

    QString displayName() const override;
    QIcon icon() const override;
    void openedImpl() override;

   public slots:
    void update() override;

   private:
    void startCheck(const SharedPack::Link& link);
    void copyInvite();
    void leave();
    void setPackControlsEnabled(bool enabled);

    QCheckBox* m_autoUpdate = nullptr;
    QPushButton* m_copyInvite = nullptr;
    QPushButton* m_leave = nullptr;
    QPushButton* m_publish = nullptr;
    Task::Ptr m_check;
    std::optional<SharedPack::Manifest> m_manifest;  ///< the newer revision the last check found
};
