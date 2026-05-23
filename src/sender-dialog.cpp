/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sender-dialog.hpp"

extern "C" {
#include <obs-module.h>
#include <obs.h>
#include "log.h"
#include "sender.h"
}

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace {

class SenderDialog : public QDialog {
public:
	SenderDialog();
	~SenderDialog() override;

private:
	void populateSourceList();
	void onGenerateToken();
	void onRevokeToken();
	void onAcceptPeer();
	void onRejectPeer();
	void onDisconnectPeer();
	void refreshStatusLabel();

	// Static callback trampolines — these may fire on any thread; they
	// marshal to the GUI thread via QMetaObject::invokeMethod with a
	// queued connection.
	static void onTokenStatic(void *user, const char *token);
	static void onPendingStatic(void *user, const char *peer_id, const char *display_name, const char *fingerprint);
	static void onPeerStateStatic(void *user, const char *peer_id, tether_sender_state_t state);
	static void onPeerGoneStatic(void *user, const char *peer_id);

	void postTokenIssued(const QString &token);
	void postPendingPeer(const QString &peer_id, const QString &name, const QString &fp);
	void postPeerState(const QString &peer_id, tether_sender_state_t state);
	void postPeerGone(const QString &peer_id);

	QComboBox *sourceCombo_ = nullptr;
	QListWidget *audioList_ = nullptr;
	QComboBox *modeCombo_ = nullptr;
	QLineEdit *serverField_ = nullptr;
	QPushButton *generateBtn_ = nullptr;
	QPushButton *revokeBtn_ = nullptr;
	QLineEdit *tokenField_ = nullptr;
	QPushButton *copyBtn_ = nullptr;
	QListWidget *peersList_ = nullptr;
	QPushButton *acceptBtn_ = nullptr;
	QPushButton *rejectBtn_ = nullptr;
	QPushButton *disconnectBtn_ = nullptr;
	QLabel *statusLabel_ = nullptr;

	tether_sender_t *sender_ = nullptr;
	std::atomic<bool> shutting_down_{false};
};

static QPointer<SenderDialog> g_dialog;

static QString fromTr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

SenderDialog::SenderDialog() : QDialog(nullptr)
{
	setWindowTitle(fromTr("Source.Sender.Name"));
	setMinimumWidth(520);

	auto *root = new QVBoxLayout(this);

	// --- Top: token + Generate/Revoke/Copy (the headline action) ---
	auto *tokenLabel = new QLabel(fromTr("Token.Generated"), this);
	QFont tokenLabelFont = tokenLabel->font();
	tokenLabelFont.setBold(true);
	tokenLabel->setFont(tokenLabelFont);
	root->addWidget(tokenLabel);

	auto *tokenRow = new QHBoxLayout();
	tokenField_ = new QLineEdit(this);
	tokenField_->setReadOnly(true);
	tokenField_->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 14pt; letter-spacing: 0.1em;"));
	tokenField_->setPlaceholderText(QStringLiteral("TTHR-XXXX-XXXX-XXXX"));
	copyBtn_ = new QPushButton(fromTr("Token.Copy"), this);
	copyBtn_->setEnabled(false);
	tokenRow->addWidget(tokenField_, 1);
	tokenRow->addWidget(copyBtn_);
	root->addLayout(tokenRow);

	auto *btnRow = new QHBoxLayout();
	generateBtn_ = new QPushButton(fromTr("Token.Generate"), this);
	revokeBtn_ = new QPushButton(fromTr("Token.Revoke"), this);
	revokeBtn_->setEnabled(false);
	btnRow->addWidget(generateBtn_);
	btnRow->addWidget(revokeBtn_);
	root->addLayout(btnRow);

	statusLabel_ = new QLabel(fromTr("Admission.Status.Idle"), this);
	root->addWidget(statusLabel_);

	// --- Middle: what to share (settings) ---
	auto *settingsHeader = new QLabel(fromTr("Settings.VideoSource"), this);
	root->addSpacing(8);
	root->addWidget(settingsHeader);
	sourceCombo_ = new QComboBox(this);
	root->addWidget(sourceCombo_);

	auto *audioLabel = new QLabel(fromTr("Settings.AudioTracks"), this);
	root->addWidget(audioLabel);
	audioList_ = new QListWidget(this);
	audioList_->setSelectionMode(QAbstractItemView::NoSelection);
	audioList_->setMaximumHeight(110);
	root->addWidget(audioList_);

	auto *modeLabel = new QLabel(fromTr("Settings.Mode"), this);
	root->addWidget(modeLabel);
	modeCombo_ = new QComboBox(this);
	modeCombo_->addItem(fromTr("Settings.Mode.Standard"), 0);
	modeCombo_->addItem(fromTr("Settings.Mode.TwitchStreamTogether"), 1);
	root->addWidget(modeCombo_);

	auto *serverLabel = new QLabel(fromTr("Settings.Server.Url"), this);
	root->addWidget(serverLabel);
	serverField_ = new QLineEdit(this);
	serverField_->setPlaceholderText(QStringLiteral("wss://… (leave blank for managed default)"));
	root->addWidget(serverField_);

	// --- Bottom: peers list + actions ---
	root->addSpacing(8);
	peersList_ = new QListWidget(this);
	root->addWidget(peersList_, 1);

	auto *peerRow = new QHBoxLayout();
	acceptBtn_ = new QPushButton(fromTr("Admission.Request.Accept"), this);
	rejectBtn_ = new QPushButton(fromTr("Admission.Request.Reject"), this);
	disconnectBtn_ = new QPushButton(fromTr("Admission.Receiver.Disconnect"), this);
	acceptBtn_->setEnabled(false);
	rejectBtn_->setEnabled(false);
	disconnectBtn_->setEnabled(false);
	peerRow->addWidget(acceptBtn_);
	peerRow->addWidget(rejectBtn_);
	peerRow->addWidget(disconnectBtn_);
	root->addLayout(peerRow);

	connect(generateBtn_, &QPushButton::clicked, this, &SenderDialog::onGenerateToken);
	connect(revokeBtn_, &QPushButton::clicked, this, &SenderDialog::onRevokeToken);
	connect(acceptBtn_, &QPushButton::clicked, this, &SenderDialog::onAcceptPeer);
	connect(rejectBtn_, &QPushButton::clicked, this, &SenderDialog::onRejectPeer);
	connect(disconnectBtn_, &QPushButton::clicked, this, &SenderDialog::onDisconnectPeer);
	connect(copyBtn_, &QPushButton::clicked, this, [this] {
		QGuiApplication::clipboard()->setText(tokenField_->text());
		copyBtn_->setText(fromTr("Token.Copied"));
		QTimer::singleShot(1500, this, [this] { copyBtn_->setText(fromTr("Token.Copy")); });
	});
	connect(peersList_, &QListWidget::currentRowChanged, this, [this](int row) {
		QListWidgetItem *it = (row >= 0) ? peersList_->item(row) : nullptr;
		const bool isConnected = it && it->data(Qt::UserRole + 1).toInt() == TETHER_SENDER_STATE_PEER_CONNECTED;
		const bool isPending = it && !isConnected;
		acceptBtn_->setEnabled(isPending);
		rejectBtn_->setEnabled(isPending);
		disconnectBtn_->setEnabled(isConnected);
	});

	// All widgets exist now — safe to populate the source / audio lists.
	populateSourceList();
}

SenderDialog::~SenderDialog()
{
	shutting_down_ = true;
	if (sender_) {
		tether_sender_release(sender_);
		sender_ = nullptr;
	}
}

void SenderDialog::populateSourceList()
{
	auto add_source = [this](obs_source_t *src) {
		const uint32_t caps = obs_source_get_output_flags(src);
		const bool is_video = (caps & OBS_SOURCE_VIDEO) != 0;
		const bool is_audio = (caps & OBS_SOURCE_AUDIO) != 0;
		const char *name = obs_source_get_name(src);
		if (!name) {
			return;
		}
		if (is_video) {
			const QString qname = QString::fromUtf8(name);
			if (sourceCombo_->findText(qname) < 0) {
				sourceCombo_->addItem(qname);
			}
		}
		if (is_audio) {
			const QString qname = QString::fromUtf8(name);
			for (int i = 0; i < audioList_->count(); ++i) {
				if (audioList_->item(i)->text() == qname) {
					return; // already added
				}
			}
			auto *item = new QListWidgetItem(qname, audioList_);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Unchecked);
		}
	};

	auto enum_cb = [](void *param, obs_source_t *src) -> bool {
		auto &fn = *static_cast<decltype(add_source) *>(param);
		fn(src);
		return true;
	};
	obs_enum_sources(enum_cb, &add_source);

	// OBS keeps the global audio I/O on the well-known output channels 1..6
	// (channel 0 is the main video composition). obs_enum_sources skips
	// these, so the Desktop Audio / Microphone tracks visible in the user's
	// Audio Mixer never showed up in the dialog. Pull them in explicitly.
	for (uint32_t channel = 1; channel < MAX_CHANNELS; ++channel) {
		if (obs_source_t *src = obs_get_output_source(channel)) {
			add_source(src);
			obs_source_release(src);
		}
	}
}

void SenderDialog::onGenerateToken()
{
	if (sender_) {
		tether_sender_release(sender_);
		sender_ = nullptr;
	}
	const QString sourceName = sourceCombo_->currentText();
	if (sourceName.isEmpty()) {
		statusLabel_->setText(fromTr("Settings.VideoSource.Description"));
		return;
	}
	std::vector<QByteArray> audioBytes;
	for (int i = 0; i < audioList_->count(); ++i) {
		QListWidgetItem *it = audioList_->item(i);
		if (it->checkState() == Qt::Checked) {
			audioBytes.push_back(it->text().toUtf8());
		}
	}
	std::vector<const char *> audioPtrs;
	for (const auto &ba : audioBytes) {
		audioPtrs.push_back(ba.constData());
	}
	audioPtrs.push_back(nullptr);

	const QByteArray sourceBytes = sourceName.toUtf8();
	const QByteArray serverBytes = serverField_->text().toUtf8();

	tether_sender_config_t cfg{};
	cfg.server_url = serverBytes.isEmpty() ? nullptr : serverBytes.constData();
	cfg.source_name = sourceBytes.constData();
	cfg.audio_source_names = audioPtrs.empty() ? nullptr : audioPtrs.data();
	cfg.video_bitrate_kbps = 6000;
	cfg.max_receivers = 4;
	cfg.token_ttl_minutes = 30;
	cfg.reusable_token = false;
	cfg.twitch_st_mode = (modeCombo_->currentData().toInt() == 1);

	tether_sender_callbacks_t cbs{};
	cbs.user = this;
	cbs.on_token = &SenderDialog::onTokenStatic;
	cbs.on_pending = &SenderDialog::onPendingStatic;
	cbs.on_peer_state = &SenderDialog::onPeerStateStatic;
	cbs.on_peer_gone = &SenderDialog::onPeerGoneStatic;

	sender_ = tether_sender_create(&cfg, &cbs);
	if (!sender_) {
		statusLabel_->setText(QStringLiteral("Failed to start sender (see log)"));
		return;
	}
	generateBtn_->setEnabled(false);
	revokeBtn_->setEnabled(true);
	statusLabel_->setText(fromTr("Status.Connecting"));
}

void SenderDialog::onRevokeToken()
{
	if (sender_) {
		tether_sender_revoke_token(sender_);
		tether_sender_release(sender_);
		sender_ = nullptr;
	}
	tokenField_->clear();
	copyBtn_->setEnabled(false);
	peersList_->clear();
	statusLabel_->setText(fromTr("Admission.Status.Idle"));
	generateBtn_->setEnabled(true);
	revokeBtn_->setEnabled(false);
}

void SenderDialog::onAcceptPeer()
{
	QListWidgetItem *it = peersList_->currentItem();
	if (!it || !sender_) {
		return;
	}
	tether_sender_accept(sender_, it->data(Qt::UserRole).toString().toUtf8().constData(), true);
}

void SenderDialog::onRejectPeer()
{
	QListWidgetItem *it = peersList_->currentItem();
	if (!it || !sender_) {
		return;
	}
	tether_sender_reject(sender_, it->data(Qt::UserRole).toString().toUtf8().constData());
}

void SenderDialog::onDisconnectPeer()
{
	QListWidgetItem *it = peersList_->currentItem();
	if (!it || !sender_) {
		return;
	}
	tether_sender_disconnect_peer(sender_, it->data(Qt::UserRole).toString().toUtf8().constData());
}

void SenderDialog::refreshStatusLabel()
{
	int pending = 0;
	int connected = 0;
	for (int i = 0; i < peersList_->count(); ++i) {
		QListWidgetItem *it = peersList_->item(i);
		const int state = it->data(Qt::UserRole + 1).toInt();
		if (state == TETHER_SENDER_STATE_PEER_CONNECTED) {
			++connected;
		} else {
			++pending;
		}
	}
	if (connected > 0 && pending > 0) {
		statusLabel_->setText(fromTr("Admission.Status.Mixed").arg(connected).arg(pending));
	} else if (connected > 0) {
		statusLabel_->setText(fromTr("Admission.Status.Connected").arg(connected));
	} else if (pending > 0) {
		statusLabel_->setText(fromTr("Admission.Status.Pending").arg(pending));
	} else if (sender_ && tether_sender_current_token(sender_)) {
		statusLabel_->setText(fromTr("Admission.Status.Idle"));
	}
}

// ---- static trampolines marshaling to the Qt thread ---

void SenderDialog::onTokenStatic(void *user, const char *token)
{
	auto *self = static_cast<SenderDialog *>(user);
	if (!self || self->shutting_down_) {
		return;
	}
	const QString tok = QString::fromUtf8(token ? token : "");
	QMetaObject::invokeMethod(self, [self, tok] { self->postTokenIssued(tok); }, Qt::QueuedConnection);
}

void SenderDialog::onPendingStatic(void *user, const char *peer_id, const char *display_name, const char *fingerprint)
{
	auto *self = static_cast<SenderDialog *>(user);
	if (!self || self->shutting_down_) {
		return;
	}
	const QString id = QString::fromUtf8(peer_id ? peer_id : "");
	const QString nm = QString::fromUtf8(display_name ? display_name : "");
	const QString fp = QString::fromUtf8(fingerprint ? fingerprint : "");
	QMetaObject::invokeMethod(
		self, [self, id, nm, fp] { self->postPendingPeer(id, nm, fp); }, Qt::QueuedConnection);
}

void SenderDialog::onPeerStateStatic(void *user, const char *peer_id, tether_sender_state_t state)
{
	auto *self = static_cast<SenderDialog *>(user);
	if (!self || self->shutting_down_) {
		return;
	}
	const QString id = QString::fromUtf8(peer_id ? peer_id : "");
	QMetaObject::invokeMethod(self, [self, id, state] { self->postPeerState(id, state); }, Qt::QueuedConnection);
}

void SenderDialog::onPeerGoneStatic(void *user, const char *peer_id)
{
	auto *self = static_cast<SenderDialog *>(user);
	if (!self || self->shutting_down_) {
		return;
	}
	const QString id = QString::fromUtf8(peer_id ? peer_id : "");
	QMetaObject::invokeMethod(self, [self, id] { self->postPeerGone(id); }, Qt::QueuedConnection);
}

void SenderDialog::postTokenIssued(const QString &token)
{
	tokenField_->setText(token);
	copyBtn_->setEnabled(true);
	statusLabel_->setText(fromTr("Admission.Status.Idle"));
}

void SenderDialog::postPendingPeer(const QString &peer_id, const QString &name, const QString &fp)
{
	auto *item =
		new QListWidgetItem(QStringLiteral("%1  (%2)").arg(name.isEmpty() ? peer_id : name, fp), peersList_);
	item->setData(Qt::UserRole, peer_id);
	item->setData(Qt::UserRole + 1, static_cast<int>(TETHER_SENDER_STATE_WAITING));
	refreshStatusLabel();
}

void SenderDialog::postPeerState(const QString &peer_id, tether_sender_state_t state)
{
	for (int i = 0; i < peersList_->count(); ++i) {
		QListWidgetItem *it = peersList_->item(i);
		if (it->data(Qt::UserRole).toString() == peer_id) {
			QString suffix;
			if (state == TETHER_SENDER_STATE_PEER_CONNECTED) {
				suffix = QStringLiteral(" — connected");
			} else if (state == TETHER_SENDER_STATE_FAILED) {
				suffix = QStringLiteral(" — failed");
			}
			it->setText(QStringLiteral("%1%2").arg(it->text().split(" — ").first(), suffix));
			it->setData(Qt::UserRole + 1, static_cast<int>(state));
			break;
		}
	}
	refreshStatusLabel();
	// Update button enablement for the current selection.
	if (QListWidgetItem *cur = peersList_->currentItem()) {
		const bool isConnected = cur->data(Qt::UserRole + 1).toInt() == TETHER_SENDER_STATE_PEER_CONNECTED;
		acceptBtn_->setEnabled(!isConnected);
		rejectBtn_->setEnabled(!isConnected);
		disconnectBtn_->setEnabled(isConnected);
	}
}

void SenderDialog::postPeerGone(const QString &peer_id)
{
	for (int i = 0; i < peersList_->count(); ++i) {
		QListWidgetItem *it = peersList_->item(i);
		if (it->data(Qt::UserRole).toString() == peer_id) {
			delete peersList_->takeItem(i);
			break;
		}
	}
	refreshStatusLabel();
}

} // namespace

extern "C" void tether_open_sender_dialog(void)
{
	auto open = [] {
		if (!g_dialog) {
			g_dialog = new SenderDialog();
			g_dialog->setAttribute(Qt::WA_DeleteOnClose);
		}
		g_dialog->show();
		g_dialog->raise();
		g_dialog->activateWindow();
	};
	if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
		open();
	} else {
		QMetaObject::invokeMethod(QCoreApplication::instance(), open, Qt::QueuedConnection);
	}
}

extern "C" void tether_close_sender_dialog(void)
{
	if (g_dialog) {
		g_dialog->close();
		g_dialog = nullptr;
	}
}
