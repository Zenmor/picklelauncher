// SPDX-License-Identifier: GPL-3.0-only

#include "PublishSharedPackDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QTreeView>
#include <QVBoxLayout>
#include <cmath>

#include "FileIgnoreProxy.h"
#include "FolderSizeCache.h"
#include "StringUtils.h"
#include "Application.h"
#include "sharedpack/SharedPackBuild.h"
#include "sharedpack/SharedPackCredentials.h"
#include "sharedpack/SharedPackRedact.h"
#include "sharedpack/SharedPackSelection.h"
#include "ui/SharedPackUi.h"

using namespace SharedPack;

namespace {

struct RelayPublisherCode {
    QString worker;
    QString code;
};

std::optional<RelayPublisherCode> decodeRelayPublisherCode(QString text)
{
    text = text.trimmed();
    constexpr QLatin1String prefix("picklepub:1:");
    if (!text.startsWith(prefix))
        return std::nullopt;
    const auto decoded = QByteArray::fromBase64Encoding(
        text.mid(prefix.size()).toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(decoded.decoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    const QJsonValue workerValue = object.value(QStringLiteral("worker"));
    const QJsonValue codeValue = object.value(QStringLiteral("code"));
    const QJsonValue expiryValue = object.value(QStringLiteral("exp"));
    if (!workerValue.isString() || !codeValue.isString() || !expiryValue.isDouble() ||
        expiryValue.toDouble() < 1 || std::floor(expiryValue.toDouble()) != expiryValue.toDouble())
        return std::nullopt;

    QUrl worker(workerValue.toString(), QUrl::StrictMode);
    if (!worker.isValid() || worker.scheme() != QLatin1String("https") || worker.host().isEmpty() ||
        !worker.userInfo().isEmpty() || worker.hasQuery() || worker.hasFragment())
        return std::nullopt;
    QString path = worker.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    worker.setPath(path);

    const QString code = codeValue.toString();
    if (code.size() < 16 || code.size() > 512)
        return std::nullopt;
    return RelayPublisherCode{ worker.toString(QUrl::FullyEncoded), code };
}

}  // namespace

PublishSharedPackDialog::PublishSharedPackDialog(const QString& instanceId, QWidget* parent)
    : QDialog(parent), m_session(std::make_unique<OwnerSession>(instanceId))
{
    setWindowTitle(tr("Publish Shared Pack"));
    {
        // Never taller or wider than the screen it opens on: the file tree takes whatever room is left, the rest is compact.
        const QScreen* screen = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
        const QSize available = screen ? screen->availableGeometry().size() : QSize(1280, 800);
        resize(qMin(650, available.width() - 40), qMin(720, available.height() - 80));
    }
    setSizeGripEnabled(true);  // as ExportPackDialog

    m_isOwner = m_session->isOwner();
    auto* layout = new QVBoxLayout(this);  // the style's margins and spacing, as in ExportPackDialog
    m_form = new QFormLayout();
    m_form->setLabelAlignment(Qt::AlignLeading | Qt::AlignLeft | Qt::AlignVCenter);
    layout->addLayout(m_form);

    const auto field = [this](const QString& label, QLineEdit* edit) {
        m_form->addRow(label, edit);
        return edit;
    };
    m_kind = new QComboBox(this);
    m_kind->addItem(tr("Pickles United Shared Packs"));
    m_kind->addItem(tr("Other hosts (advanced)"));
    m_form->addRow(tr("Host"), m_kind);
    m_otherKind = new QComboBox(this);
    m_otherKind->addItem(tr("Folder on a web server"));
    m_otherKind->addItem(tr("S3 bucket (Cloudflare R2, ...)"));
    m_form->addRow(tr("Type"), m_otherKind);
    m_otherKind->setToolTip(tr("A folder your web server or sync service already publishes, or any S3-compatible bucket such as Cloudflare R2."));
    m_folder = field(tr("Folder"), new QLineEdit(this));
    m_publicUrl = field(tr("Public Address"), new QLineEdit(this));
    m_publicUrl->setPlaceholderText(tr("https://files.example.com/packs"));
    m_publicUrl->setToolTip(tr("The https address of that folder."));
    m_endpoint = field(tr("Endpoint"), new QLineEdit(this));
    m_endpoint->setPlaceholderText(tr("https://<account>.r2.cloudflarestorage.com"));
    m_bucket = field(tr("Bucket"), new QLineEdit(this));
    m_prefix = field(tr("Prefix"), new QLineEdit(this));
    m_prefix->setPlaceholderText(tr("Optional, for example packs"));
    m_region = field(tr("Region"), new QLineEdit(this));
    m_region->setPlaceholderText(QStringLiteral("auto"));
    m_keyId = field(tr("Access Key ID"), new QLineEdit(this));
    m_secret = field(tr("Secret Access Key"), new QLineEdit(this));
    m_secret->setEchoMode(QLineEdit::Password);
    m_keyId->setToolTip(tr("A key that can write to the bucket. It stays on this computer."));
    m_secret->setToolTip(m_keyId->toolTip());
    m_readKeyId = field(tr("Read-Only Key ID (goes into the invite)"), new QLineEdit(this));
    m_readSecret = field(tr("Read-Only Secret"), new QLineEdit(this));
    m_readSecret->setEchoMode(QLineEdit::Password);
    m_readKeyId->setToolTip(tr("This key is written into the invite, so everybody who has the invite can read the pack with it. "
                               "Use a read-only key, never the one that can write."));
    m_readSecret->setToolTip(m_readKeyId->toolTip());
    m_publisherCode = field(tr("Publish key"), new QLineEdit(this));
    m_publisherCode->setEchoMode(QLineEdit::Password);
    m_publisherCode->setPlaceholderText(tr("Paste the publish key from whoever runs your server"));
    m_publisherCode->setToolTip(tr("You only need the key once on this computer."));
    // Without a code row the access of an earlier code would be stuck if the server stops accepting it: this brings the row back.
    m_newCodeButton = new QPushButton(tr("I have a new publish key"), this);
    m_newCodeButton->setFlat(true);
    m_newCodeButton->setAutoDefault(false);  // Enter must keep running Check
    m_form->addRow(m_newCodeButton);
    m_tokenSaved = findSavedPublisherWorker(APPLICATION->dataRoot()).has_value();

    // Name and Changelog sit in a group box with a form, like the fields of the Modrinth export (its Summary: label above a short box).
    auto* packBox = new QGroupBox(tr("Pack"), this);
    auto* packLayout = new QVBoxLayout(packBox);
    auto* packForm = new QFormLayout();
    packForm->setLabelAlignment(Qt::AlignLeading | Qt::AlignLeft | Qt::AlignVCenter);
    packLayout->addLayout(packForm);
    m_name = new QLineEdit(packBox);
    packForm->addRow(tr("Name"), m_name);
    m_name->setMaxLength(SharedPack::Limits::MaxNameChars);
    m_name->setText(m_isOwner && m_session->invite() && !m_session->invite()->name.isEmpty() ? m_session->invite()->name
                                                                                             : m_session->instanceName());
    auto* changelogLabel = new QLabel(tr("Changelog"), packBox);
    m_changelog = new QPlainTextEdit(packBox);
    changelogLabel->setBuddy(m_changelog);
    m_changelog->setPlaceholderText(tr("What changed?"));
    m_changelog->setTabChangesFocus(true);  // as the Modrinth Summary: Tab leaves the box instead of typing a tab
    m_changelog->setFixedHeight(3 * m_changelog->fontMetrics().lineSpacing() + 16);
    packLayout->addWidget(changelogLabel);
    packLayout->addWidget(m_changelog);
    layout->addWidget(packBox);

    // What will be published, below the pack details. Created here so the tab order is host, name, changelog, files, buttons.
    buildTree(layout);

    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);  // file names and texts come from disk: never rendered as HTML
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summary);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);  // the validation sentences are shown here
    layout->addWidget(m_status);
    m_bar = new QProgressBar(this);
    m_bar->setVisible(false);
    layout->addWidget(m_bar);

    // Check, Rotate Link and Copy Invite on the left; Publish and Close in a button box on the right, which the style orders like
    // ExportPackDialog's. Every button keeps its own clicked() slot.
    auto* buttonRow = new QHBoxLayout();
    m_checkButton = new QPushButton(tr("Check"), this);
    m_rotateButton = new QPushButton(tr("Rotate Link"), this);
    m_copyButton = new QPushButton(tr("Copy Invite"), this);
    buttonRow->addWidget(m_checkButton);
    buttonRow->addWidget(m_rotateButton);
    buttonRow->addWidget(m_copyButton);
    buttonRow->addStretch();
    auto* buttons = new QDialogButtonBox(this);
    m_publishButton = buttons->addButton(tr("Publish"), QDialogButtonBox::AcceptRole);
    m_closeButton = buttons->addButton(tr("Close"), QDialogButtonBox::RejectRole);
    buttonRow->addWidget(buttons);
    layout->addLayout(buttonRow);

    if (m_isOwner) {
        // The host of a published pack is fixed: changing it would strand the members. Only the keys stay hidden.
        const HostConfig& host = m_session->record()->host;
        setKind(defaultHostKind(host.kind));
        m_folder->setText(host.folder);
        if (host.kind == HostConfig::Kind::S3)
            m_endpoint->setText(host.endpoint);
        else if (host.kind == HostConfig::Kind::Folder)
            m_publicUrl->setText(host.endpoint);
        m_bucket->setText(host.bucket);
        m_prefix->setText(host.prefix);
        m_region->setText(host.region);
        for (QLineEdit* edit : { m_keyId, m_secret, m_readKeyId, m_readSecret })
            edit->setPlaceholderText(tr("Saved"));
        for (QWidget* widget : std::initializer_list<QWidget*>{ m_kind, m_otherKind, m_folder, m_publicUrl, m_endpoint, m_bucket, m_prefix,
                                                                m_region, m_keyId, m_secret, m_readKeyId, m_readSecret, m_publisherCode })
            widget->setEnabled(false);
        if (const auto invite = m_session->invite())
            m_invite = inviteToString(*invite);
    } else {
        m_region->setText(QStringLiteral("auto"));
        setKind(defaultHostKind(std::nullopt));
    }
    updateRows();
    m_checkButton->setDefault(true);  // Enter runs Check until there is something to publish
    if (!m_session->isMember())
        m_status->setText(tr("Press Check to see what will be published."));
    // Connected only now: invalidate() touches the buttons, and the setText calls above must not trigger it.
    for (QLineEdit* edit : findChildren<QLineEdit*>())
        connect(edit, &QLineEdit::textChanged, this, &PublishSharedPackDialog::invalidate);
    connect(m_changelog, &QPlainTextEdit::textChanged, this, &PublishSharedPackDialog::invalidate);
    for (QComboBox* box : { m_kind, m_otherKind })
        connect(box, &QComboBox::currentIndexChanged, this, [this] {
            updateRows();
            invalidate();
        });
    if (m_proxy) {
        // A tick changes what Check would publish, so the last Check no longer counts. Only check-state changes: the folder sizes
        // arriving re-sort the model without going through dataChanged.
        connect(m_proxy, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            if (roles.contains(Qt::CheckStateRole))
                invalidate();
        });
        // The 100 MB mark only applies to the Shared Packs Server host.
        connect(m_kind, &QComboBox::currentIndexChanged, this, [this] { m_tree->viewport()->update(); });
    }
    connect(m_newCodeButton, &QPushButton::clicked, this, [this] {
        m_wantsNewCode = true;
        updateRows();
        m_publisherCode->setFocus();
    });

    connect(m_checkButton, &QPushButton::clicked, this, &PublishSharedPackDialog::check);
    connect(m_publishButton, &QPushButton::clicked, this, &PublishSharedPackDialog::publish);
    connect(m_rotateButton, &QPushButton::clicked, this, &PublishSharedPackDialog::rotate);
    connect(m_copyButton, &QPushButton::clicked, this, &PublishSharedPackDialog::copyInvite);
    connect(m_closeButton, &QPushButton::clicked, this, &PublishSharedPackDialog::reject);

    connect(m_session.get(), &OwnerSession::status, this, [this](const QString& text) { m_status->setText(text); });
    connect(m_session.get(), &OwnerSession::progress, this, [this](qint64 done, qint64 total) {
        m_bar->setVisible(total > 0);
        m_bar->setRange(0, 1000);
        m_bar->setValue(total > 0 ? static_cast<int>(done * 1000 / total) : 0);
    });
    connect(m_session.get(), &OwnerSession::prepared, this, [this](const Preview& preview) {
        setBusy(false);
        m_ready = !preview.manifest.files.isEmpty();  // an empty pack would give every member an empty pack
        showPreview(preview);
        m_publishButton->setEnabled(m_ready);
        (m_ready ? m_publishButton : m_checkButton)->setDefault(true);
    });
    connect(m_session.get(), &OwnerSession::published, this, [this](int revision, const QString& invite) {
        setBusy(false);
        m_invite = invite;
        m_isOwner = true;
        m_rotateButton->setVisible(true);
        m_ready = false;
        m_publishButton->setEnabled(false);
        m_bar->setVisible(false);
        m_status->setText(tr("Published revision %1. The invite is on the clipboard.").arg(revision));
        m_summary->setText(tr("Send the invite to your friends. They join with it and get every new revision when they start the launcher."));
        m_copyButton->setEnabled(true);
        QApplication::clipboard()->setText(invite);
    });
    connect(m_session.get(), &OwnerSession::rotated, this, [this](const QString& invite, const QString& warning) {
        setBusy(false);
        m_invite = invite;
        m_status->setText(tr("The link was rotated."));
        QApplication::clipboard()->setText(invite);
        SharedPackUi::showMessage(this, tr("Rotate Link"),
                                  tr("The new invite is on the clipboard. Send it to your friends: they stop receiving updates until they paste it.") +
                                      (warning.isEmpty() ? QString() : QStringLiteral("\n\n") + warning),
                                  warning.isEmpty() ? QMessageBox::Information : QMessageBox::Warning);
    });
    connect(m_session.get(), &OwnerSession::failed, this, [this](const QString& reason) {
        setBusy(false);
        m_bar->setVisible(false);
        const bool cancelled = m_cancelling;
        m_cancelling = false;
        m_status->setText(cancelled ? tr("Cancelled.") : QString());
        if (!cancelled)
            SharedPackUi::showMessage(this, tr("Error"), reason, QMessageBox::Critical);
    });

    m_rotateButton->setVisible(m_isOwner);
    m_publishButton->setEnabled(false);
    m_copyButton->setEnabled(!m_invite.isEmpty());
    if (m_session->isMember()) {
        m_summary->setText(tr("This instance follows a shared pack. Only its owner can publish it."));
        m_checkButton->setEnabled(false);
        m_form->setEnabled(false);
    }
}

void PublishSharedPackDialog::buildTree(QVBoxLayout* layout)
{
    // A member follows somebody else's pack and cannot publish: nothing to choose, so no tree.
    const QString gameRoot = m_session->gameRoot();
    if (m_session->isMember() || gameRoot.isEmpty())
        return;

    // The tree sits in a group box like the Modrinth export's Options, which frames it.
    auto* box = new QGroupBox(tr("Files"), this);
    auto* boxLayout = new QVBoxLayout(box);

    auto* model = new QFileSystemModel(this);
    model->setIconProvider(&m_icons);

    // Rooted at the game folder, so the paths the tree remembers are exactly the paths of the manifest.
    m_proxy = new FileIgnoreProxy(gameRoot, this);
    m_proxy->ignoreFilesWithName().append({ ".index", ".DS_Store", "thumbs.db", "Thumbs.db" });  // never published, only noise here
    m_proxy->setSourceModel(model);

    m_topLevel = QDir(gameRoot).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    // An owner whose pack was published before there was a choice keeps everything ticked; a new pack starts from the defaults.
    m_proxy->setBlockedPaths(initialUnticked(readSelection(m_session->instanceRoot()), m_topLevel, m_isOwner));

    m_proxy->setMarker([this](const QString& path, bool isDir, qint64 size, bool ticked) -> FileIgnoreProxy::Mark {
        if (path.isEmpty() || path == QLatin1String("."))
            return {};
        // Judged like the scan does it: a folder by a file in it. Ticking such a path changes nothing, the policy still refuses it.
        if (!isPublishablePath(isDir ? path + QStringLiteral("/x") : path))
            return { QColor(Qt::gray), tr("The launcher never publishes this (saves, logs, caches, downloaded runtimes, mod metadata).") };
        // The 100 MB limit belongs to the Shared Packs Server, not to a folder or S3 host.
        if (!isDir && ticked && size > ServerFileLimit && m_kind->currentIndex() == 0)
            return { QColor(0xe0, 0x7a, 0x00),
                     tr("Over 100 MB: the Shared Packs Server cannot store it, so publishing stops unless Modrinth hosts this file. Untick it.") };
        return {};
    });

    m_tree = new QTreeView(box);
    m_tree->setModel(m_proxy);
    m_tree->setRootIndex(m_proxy->mapFromSource(model->index(gameRoot)));
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->setUniformRowHeights(true);
    m_tree->setMinimumHeight(140);
    boxLayout->addWidget(m_tree);
    layout->addWidget(box, 1);  // the only part of the dialog that grows

    model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs | QDir::Hidden);
    model->setRootPath(gameRoot);

    QHeaderView* header = m_tree->header();
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(0, QHeaderView::Stretch);

    // Folder sizes are counted in the background, only read by the tree, and stopped when the dialog closes (reject()).
    m_sizes = new FolderSizeCache(this);
    m_proxy->setFolderSizes(m_sizes);
    m_sizes->start(gameRoot);
}

QStringList PublishSharedPackDialog::untickedPaths() const
{
    return m_proxy ? m_proxy->blockedPaths().toStringList() : QStringList();
}

void PublishSharedPackDialog::saveSelection() const
{
    if (!m_proxy)
        return;
    writeSelection(m_session->instanceRoot(), StoredSelection{ untickedPaths(), m_topLevel });
}

HostConfig::Kind PublishSharedPackDialog::currentKind() const
{
    if (m_kind->currentIndex() == 0)
        return HostConfig::Kind::Relay;
    return m_otherKind->currentIndex() == 1 ? HostConfig::Kind::S3 : HostConfig::Kind::Folder;
}

void PublishSharedPackDialog::setKind(HostConfig::Kind kind)
{
    m_kind->setCurrentIndex(kind == HostConfig::Kind::Relay ? 0 : 1);
    if (kind != HostConfig::Kind::Relay)
        m_otherKind->setCurrentIndex(kind == HostConfig::Kind::S3 ? 1 : 0);
}

void PublishSharedPackDialog::updateRows()
{
    const HostConfig::Kind kind = currentKind();
    const HostRows rows = hostRows(kind, m_isOwner, m_tokenSaved, m_wantsNewCode);
    m_form->setRowVisible(m_otherKind, rows.otherHostPicker);
    m_form->setRowVisible(m_prefix, rows.folder || rows.s3);  // the server form has no folder below the host to name
    for (QWidget* widget : std::initializer_list<QWidget*>{ m_folder, m_publicUrl })
        m_form->setRowVisible(widget, rows.folder);
    for (QWidget* widget : std::initializer_list<QWidget*>{ m_endpoint, m_bucket, m_region, m_keyId, m_secret, m_readKeyId, m_readSecret })
        m_form->setRowVisible(widget, rows.s3);
    m_form->setRowVisible(m_publisherCode, rows.publisherCode);
    m_form->setRowVisible(m_newCodeButton, rows.newCodeButton);
}

HostConfig PublishSharedPackDialog::hostFromFields() const
{
    HostConfig host;
    const HostConfig::Kind kind = currentKind();
    if (kind == HostConfig::Kind::S3) {
        host.kind = HostConfig::Kind::S3;
        host.endpoint = m_endpoint->text().trimmed();
        host.bucket = m_bucket->text().trimmed();
        host.region = m_region->text().trimmed();
        host.writeKeyId = m_keyId->text().trimmed();
        host.writeSecret = m_secret->text().trimmed();
        host.readKeyId = m_readKeyId->text().trimmed();
        host.readSecret = m_readSecret->text().trimmed();
    } else if (kind == HostConfig::Kind::Relay) {
        host.kind = HostConfig::Kind::Relay;
        // The server comes from the code the person pastes (it carries the address), else from the server remembered after the last
        // redeem. A freshly pasted code always wins over a saved token: it is a new access that is redeemed on Check.
        std::optional<RelayPublisherCode> code;
        if (!m_isOwner)
            code = decodeRelayPublisherCode(m_publisherCode->text());
        if (m_isOwner && m_session->record()) {
            host.endpoint = m_session->record()->host.endpoint;
        } else if (code) {
            host.endpoint = code->worker;
        } else if (const auto saved = findSavedPublisherWorker(APPLICATION->dataRoot())) {
            host.endpoint = *saved;
        }
        if (!code) {
            if (const auto token = findPublisherToken(APPLICATION->dataRoot(), host.endpoint))
                host.publisherToken = *token;
        }
    } else {
        host.kind = HostConfig::Kind::Folder;
        host.folder = QDir::cleanPath(m_folder->text().trimmed());
        host.endpoint = m_publicUrl->text().trimmed();
    }
    host.prefix = m_prefix->text().trimmed();
    while (host.prefix.startsWith(QLatin1Char('/')))
        host.prefix.remove(0, 1);
    while (host.prefix.endsWith(QLatin1Char('/')))
        host.prefix.chop(1);
    return host;
}

void PublishSharedPackDialog::invalidate()
{
    // The manifest was built from what was in the fields at Check: after any change it must be built again.
    m_ready = false;
    m_publishButton->setEnabled(false);
    m_checkButton->setDefault(true);
    if (!m_busy && !m_session->isMember())
        m_status->setText(tr("Press Check to see what will be published."));
}

void PublishSharedPackDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (busy)
        m_cancelling = false;  // a new step: a Cancel that did not stop the last one must not turn this step's error into "Cancelled."
    m_checkButton->setEnabled(!busy && !m_session->isMember());
    m_publishButton->setEnabled(!busy && m_ready);
    m_rotateButton->setEnabled(!busy);
    m_copyButton->setEnabled(!busy && !m_invite.isEmpty());
    m_name->setEnabled(!busy);
    m_changelog->setEnabled(!busy);
    if (m_tree)
        m_tree->setEnabled(!busy);  // the ticks are read when Check starts: they must not change under a running step
    // Not m_form->setEnabled(): that only stops the layout from reflowing, and updateRows() runs while a redeem is still busy. An owner's
    // host widgets stay disabled for good (see the constructor).
    if (!m_isOwner)
        for (QWidget* widget : std::initializer_list<QWidget*>{ m_kind, m_otherKind, m_publisherCode, m_newCodeButton })
            widget->setEnabled(!busy);
    m_closeButton->setText(busy ? tr("Cancel") : tr("Close"));
}

void PublishSharedPackDialog::check()
{
    // Mistakes in the fields are said here, next to the fields: a dialog on top of a dialog is for failures of a long step.
    m_summary->clear();
    saveSelection();  // what is ticked is remembered per pack, also when the dialog is closed without publishing
    if (m_name->text().trimmed().isEmpty()) {
        m_status->setText(tr("The pack needs a name."));
        m_name->setFocus();
        return;
    }
    HostConfig host = hostFromFields();
    if (!m_isOwner) {
        const auto publisherCode = host.kind == HostConfig::Kind::Relay
                                       ? decodeRelayPublisherCode(m_publisherCode->text())
                                       : std::optional<RelayPublisherCode>();
        if (host.kind == HostConfig::Kind::Relay && !m_publisherCode->text().trimmed().isEmpty() && !publisherCode) {
            m_status->setText(tr("The publish key is invalid."));
            return;
        }
        if (host.kind == HostConfig::Kind::Relay && !publisherCode && host.publisherToken.isEmpty()) {
            // No code and no saved access: the server-side messages of validateHostConfig talk about fields this form does not show.
            m_status->setText(tr("Paste the publish key you were given."));
            m_publisherCode->setFocus();
            return;
        }
        HostConfig validationHost = host;
        if (publisherCode && validationHost.publisherToken.isEmpty())
            validationHost.publisherToken = QStringLiteral("publisher-code-pending");
        const QString problem = validateHostConfig(validationHost);
        if (!problem.isEmpty()) {
            m_status->setText(problem);
            return;
        }
        if (publisherCode && host.publisherToken.isEmpty()) {
            QString worker = publisherCode->worker;
            while (worker.endsWith(QLatin1Char('/')))
                worker.chop(1);
            QNetworkRequest request(QUrl(worker + QStringLiteral("/publisher/redeem"), QUrl::StrictMode));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            const QString fullCode = m_publisherCode->text().trimmed();
            registerLogSecret(fullCode);
            registerLogSecret(publisherCode->code);
            const QByteArray payload = QJsonDocument(QJsonObject{ { QStringLiteral("code"), fullCode } }).toJson(QJsonDocument::Compact);
            m_status->setText(tr("Connecting to the Shared Packs Server..."));
            setBusy(true);
            m_redeemReply = APPLICATION->network()->post(request, payload);
            QNetworkReply* reply = m_redeemReply;
            connect(reply, &QNetworkReply::finished, this, [this, reply, host, endpoint = publisherCode->worker] {
                m_redeemReply = nullptr;
                const bool cancelled = m_cancelling;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray response = reply->read(64 * 1024);
                reply->deleteLater();
                if (cancelled) {
                    setBusy(false);
                    m_cancelling = false;
                    m_status->setText(tr("Cancelled."));
                    return;
                }
                const QJsonDocument document = QJsonDocument::fromJson(response);
                const QString token = document.isObject() ? document.object().value(QStringLiteral("publisherToken")).toString() : QString();
                if (status < 200 || status > 299 || token.isEmpty()) {
                    setBusy(false);
                    m_status->setText(status > 0 ? tr("The publish key could not be used (HTTP %1).").arg(status)
                                                 : tr("Could not reach the Shared Packs Server."));
                    return;
                }
                registerLogSecret(token);
                if (!writePublisherToken(APPLICATION->dataRoot(), endpoint, token)) {
                    setBusy(false);
                    m_status->setText(tr("The publisher access could not be saved on this computer."));
                    return;
                }
                // The server is remembered with the token (writePublisherToken): from now on this computer needs no code. The field is
                // emptied without textChanged: invalidate() would reset the step that is about to start.
                m_tokenSaved = true;
                m_wantsNewCode = false;
                {
                    const QSignalBlocker blocker(m_publisherCode);
                    m_publisherCode->clear();
                }
                updateRows();
                HostConfig redeemedHost = host;
                redeemedHost.publisherToken = token;
                m_session->prepare(redeemedHost, m_name->text(), m_changelog->toPlainText(), untickedPaths());
            });
            return;
        }
    }
    m_summary->clear();
    m_ready = false;
    setBusy(true);
    m_session->prepare(host, m_name->text(), m_changelog->toPlainText(), untickedPaths());
}

void PublishSharedPackDialog::showPreview(const Preview& preview)
{
    const auto& plan = preview.plan;
    QStringList lines;
    lines << (preview.firstPublish ? tr("First publish: revision %1").arg(preview.manifest.revision)
                                   : tr("New revision: %1 (the host has %2)").arg(preview.manifest.revision).arg(preview.previousRevision));
    lines << tr("%1 files, %2 in total").arg(preview.manifest.files.size()).arg(StringUtils::humanReadableFileSize(preview.manifest.totalSize()));
    lines << tr("%1 files are fetched from Modrinth by your friends").arg(preview.publicUrlFiles);
    lines << tr("%1 files to upload, %2").arg(plan.blobCount).arg(StringUtils::humanReadableFileSize(plan.blobBytes));
    if (!preview.skipped.isEmpty())
        lines << tr("%1 items are never published (saves, screenshots, logs, caches, links)").arg(preview.skipped.size());
    if (preview.releasedCount > 0)
        lines << tr("%1 unticked items are still in your game folder: friends who have them keep their copy").arg(preview.releasedCount);
    lines << preview.warnings;
    m_summary->setText(lines.join(QLatin1Char('\n')));
    m_status->setText(preview.manifest.files.isEmpty() ? tr("There are no files to publish.") : tr("Ready to publish."));
    m_bar->setVisible(false);
}

void PublishSharedPackDialog::publish()
{
    if (!m_ready)
        return;
    m_status->setText(tr("Uploading..."));
    setBusy(true);
    m_session->publish();
}

void PublishSharedPackDialog::rotate()
{
    // Said plainly: this is a decision with consequences for everybody who has the invite. For S3 the sentence about the keys comes
    // first: the new invite holds the same read-only key (only the link changes), so Rotate Link does nothing against a leaked key.
    const bool relay = currentKind() == HostConfig::Kind::Relay;
    const QString consequence = relay
                                   ? tr("The old invite will immediately stop reading the pack and receiving updates.")
                                   : tr("Everybody who has the old invite stops receiving updates until they paste the new one.");
    const bool s3 = currentKind() == HostConfig::Kind::S3;
    QString text = consequence;
    if (s3)
        text = tr("The new invite holds the same read-only key as the old one. Rotating the link does not revoke or replace it, so it "
                  "does not help if that key leaked.") +
               QStringLiteral("\n\n") + consequence;
    QMessageBox box(s3 ? QMessageBox::Warning : QMessageBox::Question, tr("Rotate Link"), text,
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Yes)
        return;
    m_summary->clear();
    setBusy(true);
    m_session->rotate();
}

void PublishSharedPackDialog::copyInvite()
{
    if (m_invite.isEmpty())
        return;
    QApplication::clipboard()->setText(m_invite);
    m_status->setText(tr("Invite copied."));
}

void PublishSharedPackDialog::reject()
{
    if (m_busy) {
        m_cancelling = true;
        m_status->setText(tr("Cancelling..."));
        if (m_redeemReply) {
            m_redeemReply->abort();
            return;
        }
        m_session->abort();
        return;  // the dialog closes when the user presses Close again after the step has stopped
    }
    saveSelection();
    if (m_sizes)
        m_sizes->cancel();  // stop counting folders: nothing is shown any more
    QDialog::reject();
}
