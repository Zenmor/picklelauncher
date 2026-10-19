// SPDX-License-Identifier: GPL-3.0-only
//
// picklelauncher - "Publish Shared Pack" (pickle-sharedpack). The owner's dialog: where to publish, what changed, what will be uploaded.
// Every rule is in launcher/sharedpack/ (tested); this only asks, shows and starts OwnerSession.

#pragma once

#include <QDialog>
#include <memory>

#include "FastFileIconProvider.h"
#include "sharedpack/SharedPackOwnerSession.h"

class FileIgnoreProxy;
class FolderSizeCache;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QNetworkReply;
class QTreeView;
class QVBoxLayout;

class PublishSharedPackDialog final : public QDialog {
    Q_OBJECT
   public:
    PublishSharedPackDialog(const QString& instanceId, QWidget* parent);

    void reject() override;

   private:
    SharedPack::HostConfig hostFromFields() const;
    SharedPack::HostConfig::Kind currentKind() const;
    void setKind(SharedPack::HostConfig::Kind kind);
    void updateRows();
    void invalidate();
    void setBusy(bool busy);
    void check();
    void publish();
    void rotate();
    void copyInvite();
    void showPreview(const SharedPack::Preview& preview);
    void buildTree(QVBoxLayout* layout);
    QStringList untickedPaths() const;
    void saveSelection() const;

    std::unique_ptr<SharedPack::OwnerSession> m_session;
    QTreeView* m_tree = nullptr;           ///< the files that will be published; absent for a member (nothing to publish)
    FileIgnoreProxy* m_proxy = nullptr;    ///< holds the unticked paths
    FolderSizeCache* m_sizes = nullptr;    ///< folder sizes, counted in the background and stopped when the dialog closes
    FastFileIconProvider m_icons;
    QStringList m_topLevel;                ///< names directly in the game folder when the dialog opened
    QComboBox* m_kind = nullptr;       ///< Shared Packs Server / Other hosts (advanced)
    QComboBox* m_otherKind = nullptr;  ///< below "Other hosts": Folder / S3 bucket
    QLineEdit* m_folder = nullptr;
    QLineEdit* m_publicUrl = nullptr;
    QLineEdit* m_endpoint = nullptr;
    QLineEdit* m_bucket = nullptr;
    QLineEdit* m_prefix = nullptr;
    QLineEdit* m_region = nullptr;
    QLineEdit* m_keyId = nullptr;
    QLineEdit* m_secret = nullptr;
    QLineEdit* m_readKeyId = nullptr;
    QLineEdit* m_readSecret = nullptr;
    QLineEdit* m_publisherCode = nullptr;
    QPushButton* m_newCodeButton = nullptr;
    QLineEdit* m_name = nullptr;
    QPlainTextEdit* m_changelog = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_checkButton = nullptr;
    QPushButton* m_publishButton = nullptr;
    QPushButton* m_rotateButton = nullptr;
    QPushButton* m_copyButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QFormLayout* m_form = nullptr;
    QString m_invite;
    bool m_ready = false;
    bool m_busy = false;
    bool m_isOwner = false;
    bool m_cancelling = false;
    bool m_tokenSaved = false;     ///< this computer already holds a publisher token for the saved server: no code needed
    bool m_wantsNewCode = false;   ///< the person pressed "I have a new Publisher Code"
    QNetworkReply* m_redeemReply = nullptr;
};
