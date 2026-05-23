/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "receive-dialog.hpp"

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
}

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {

class ReceiveDialog : public QDialog {
public:
	ReceiveDialog();

private:
	void refreshSessionList();
	void onConnect();

	QLineEdit *tokenField_ = nullptr;
	QPushButton *connectBtn_ = nullptr;
	QListWidget *sessionList_ = nullptr;
	QPushButton *disconnectBtn_ = nullptr;
	QLabel *hintLabel_ = nullptr;
	QTimer *refreshTimer_ = nullptr;
};

static QPointer<ReceiveDialog> g_dialog;

static QString fromTr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Enumerate Tether-Quelle sources in every scene of the current collection.
// We tag each by its OBS source UUID so the user can disconnect a specific one.
static void enumerate_tether_sources(QListWidget *list)
{
	struct ctx {
		QListWidget *list;
	} c{list};
	auto cb = [](void *param, obs_source_t *src) -> bool {
		auto *ctx = static_cast<struct ctx *>(param);
		const char *id = obs_source_get_id(src);
		if (!id || strcmp(id, "tether_source") != 0) {
			return true;
		}
		const char *name = obs_source_get_name(src);
		obs_data_t *settings = obs_source_get_settings(src);
		const char *token = obs_data_get_string(settings, "token");
		QString label =
			QStringLiteral("%1 — %2").arg(QString::fromUtf8(name ? name : "(unnamed)"),
						      QString::fromUtf8(token && *token ? token : "(no token)"));
		auto *item = new QListWidgetItem(label, ctx->list);
		item->setData(Qt::UserRole, QString::fromUtf8(obs_source_get_uuid(src)));
		obs_data_release(settings);
		return true;
	};
	obs_enum_sources(cb, &c);
}

ReceiveDialog::ReceiveDialog() : QDialog(nullptr)
{
	setWindowTitle(fromTr("Receive.Title"));
	setMinimumWidth(480);

	auto *root = new QVBoxLayout(this);

	auto *tokenLabel = new QLabel(fromTr("Receive.TokenLabel"), this);
	root->addWidget(tokenLabel);

	auto *tokenRow = new QHBoxLayout();
	tokenField_ = new QLineEdit(this);
	tokenField_->setPlaceholderText(fromTr("Receive.TokenPlaceholder"));
	tokenField_->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 14pt; letter-spacing: 0.1em;"));
	connectBtn_ = new QPushButton(fromTr("Receive.Connect"), this);
	tokenRow->addWidget(tokenField_, 1);
	tokenRow->addWidget(connectBtn_);
	root->addLayout(tokenRow);

	root->addSpacing(8);
	auto *sessionsLabel = new QLabel(fromTr("Receive.Sessions"), this);
	root->addWidget(sessionsLabel);
	sessionList_ = new QListWidget(this);
	root->addWidget(sessionList_, 1);

	disconnectBtn_ = new QPushButton(fromTr("Receive.Disconnect"), this);
	disconnectBtn_->setEnabled(false);
	root->addWidget(disconnectBtn_);

	hintLabel_ = new QLabel(fromTr("Receive.Hint"), this);
	hintLabel_->setWordWrap(true);
	hintLabel_->setStyleSheet(QStringLiteral("color: gray;"));
	root->addWidget(hintLabel_);

	connect(connectBtn_, &QPushButton::clicked, this, &ReceiveDialog::onConnect);
	connect(sessionList_, &QListWidget::currentRowChanged, this,
		[this](int row) { disconnectBtn_->setEnabled(row >= 0); });
	connect(disconnectBtn_, &QPushButton::clicked, this, [this] {
		QListWidgetItem *it = sessionList_->currentItem();
		if (!it) {
			return;
		}
		QString uuid = it->data(Qt::UserRole).toString();
		obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (src) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "token", "");
			obs_source_update(src, settings);
			obs_data_release(settings);
			obs_source_release(src);
		}
		refreshSessionList();
	});

	refreshTimer_ = new QTimer(this);
	refreshTimer_->setInterval(2000);
	connect(refreshTimer_, &QTimer::timeout, this, &ReceiveDialog::refreshSessionList);
	refreshTimer_->start();

	refreshSessionList();
}

void ReceiveDialog::refreshSessionList()
{
	QString selectedUuid;
	if (QListWidgetItem *cur = sessionList_->currentItem()) {
		selectedUuid = cur->data(Qt::UserRole).toString();
	}
	sessionList_->clear();
	enumerate_tether_sources(sessionList_);
	for (int i = 0; i < sessionList_->count(); ++i) {
		if (sessionList_->item(i)->data(Qt::UserRole).toString() == selectedUuid) {
			sessionList_->setCurrentRow(i);
			break;
		}
	}
}

// Create a new Tether-Quelle source on the currently active scene with the
// pasted token. Lets the user receive purely from the hub dialog, without
// having to use the OBS Add-Source menu.
void ReceiveDialog::onConnect()
{
	const QString token = tokenField_->text().trimmed();
	if (token.isEmpty()) {
		return;
	}

	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) {
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "token", token.toUtf8().constData());

	// Name the source by token so it's easy to find in the source list.
	QString name = QStringLiteral("Tether %1").arg(token);
	obs_source_t *src = obs_source_create("tether_source", name.toUtf8().constData(), settings, nullptr);
	if (src) {
		obs_scene_t *s = obs_scene_from_source(scene);
		if (s) {
			obs_scene_add(s, src);
		}
		obs_source_release(src);
	}
	obs_data_release(settings);
	obs_source_release(scene);

	tokenField_->clear();
	refreshSessionList();
}

} // namespace

extern "C" void tether_open_receive_dialog(void)
{
	auto open = [] {
		if (!g_dialog) {
			g_dialog = new ReceiveDialog();
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

extern "C" void tether_close_receive_dialog(void)
{
	if (g_dialog) {
		g_dialog->close();
		g_dialog = nullptr;
	}
}
