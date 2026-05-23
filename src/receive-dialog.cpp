/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "receive-dialog.hpp"

extern "C" {
#include "known-tokens.h"
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
	void onRegisterToken();

	QLineEdit *tokenField_ = nullptr;
	QPushButton *registerBtn_ = nullptr;
	QListWidget *tokenList_ = nullptr;
	QPushButton *forgetBtn_ = nullptr;
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

	// --- Top: register a token so Tether-Quelle sources can pick it up. ---
	auto *tokenLabel = new QLabel(fromTr("Receive.TokenLabel"), this);
	QFont tokenFont = tokenLabel->font();
	tokenFont.setBold(true);
	tokenLabel->setFont(tokenFont);
	root->addWidget(tokenLabel);

	auto *tokenRow = new QHBoxLayout();
	tokenField_ = new QLineEdit(this);
	tokenField_->setPlaceholderText(fromTr("Receive.TokenPlaceholder"));
	tokenField_->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 14pt; letter-spacing: 0.1em;"));
	registerBtn_ = new QPushButton(fromTr("Receive.Register"), this);
	tokenRow->addWidget(tokenField_, 1);
	tokenRow->addWidget(registerBtn_);
	root->addLayout(tokenRow);

	auto *knownLabel = new QLabel(fromTr("Receive.KnownTokens"), this);
	root->addWidget(knownLabel);
	tokenList_ = new QListWidget(this);
	tokenList_->setMaximumHeight(110);
	root->addWidget(tokenList_);
	forgetBtn_ = new QPushButton(fromTr("Receive.Forget"), this);
	forgetBtn_->setEnabled(false);
	root->addWidget(forgetBtn_);

	hintLabel_ = new QLabel(fromTr("Receive.Hint"), this);
	hintLabel_->setWordWrap(true);
	hintLabel_->setStyleSheet(QStringLiteral("color: gray;"));
	root->addWidget(hintLabel_);

	root->addSpacing(12);
	auto *sessionsLabel = new QLabel(fromTr("Receive.Sessions"), this);
	root->addWidget(sessionsLabel);
	sessionList_ = new QListWidget(this);
	root->addWidget(sessionList_, 1);

	disconnectBtn_ = new QPushButton(fromTr("Receive.Disconnect"), this);
	disconnectBtn_->setEnabled(false);
	root->addWidget(disconnectBtn_);

	connect(registerBtn_, &QPushButton::clicked, this, &ReceiveDialog::onRegisterToken);
	connect(tokenField_, &QLineEdit::returnPressed, this, &ReceiveDialog::onRegisterToken);
	connect(tokenList_, &QListWidget::currentRowChanged, this,
		[this](int row) { forgetBtn_->setEnabled(row >= 0); });
	connect(forgetBtn_, &QPushButton::clicked, this, [this] {
		QListWidgetItem *it = tokenList_->currentItem();
		if (!it) {
			return;
		}
		const QByteArray ba = it->text().toUtf8();
		tether_known_tokens_remove(ba.constData());
		refreshSessionList();
	});

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
	QString selectedSession;
	if (QListWidgetItem *cur = sessionList_->currentItem()) {
		selectedSession = cur->data(Qt::UserRole).toString();
	}
	sessionList_->clear();
	enumerate_tether_sources(sessionList_);
	for (int i = 0; i < sessionList_->count(); ++i) {
		if (sessionList_->item(i)->data(Qt::UserRole).toString() == selectedSession) {
			sessionList_->setCurrentRow(i);
			break;
		}
	}

	// Sync the known-tokens list (right-side pool) with the registry.
	QString selectedToken;
	if (QListWidgetItem *cur = tokenList_->currentItem()) {
		selectedToken = cur->text();
	}
	tokenList_->clear();
	size_t n = 0;
	char **tokens = tether_known_tokens_snapshot(&n);
	for (size_t i = 0; i < n; ++i) {
		auto *item = new QListWidgetItem(QString::fromUtf8(tokens[i]), tokenList_);
		if (QString::fromUtf8(tokens[i]) == selectedToken) {
			tokenList_->setCurrentItem(item);
		}
	}
	tether_known_tokens_free_snapshot(tokens, n);
}

void ReceiveDialog::onRegisterToken()
{
	const QString token = tokenField_->text().trimmed().toUpper();
	if (token.isEmpty()) {
		return;
	}
	const QByteArray ba = token.toUtf8();
	tether_known_tokens_add(ba.constData());
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
