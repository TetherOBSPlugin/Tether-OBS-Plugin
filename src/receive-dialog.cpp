/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "receive-dialog.hpp"

extern "C" {
#include "known-tokens.h"
#include "receive-session.h"
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

#include <unordered_map>

namespace {

static const char *state_label_key(tether_receive_state_t s)
{
	switch (s) {
	case TETHER_RX_STATE_CONNECTING:
		return "Receive.State.Connecting";
	case TETHER_RX_STATE_AWAITING_ACCEPT:
		return "Receive.State.AwaitingAccept";
	case TETHER_RX_STATE_ACCEPTED:
		return "Receive.State.Accepted";
	case TETHER_RX_STATE_NEGOTIATING:
		return "Receive.State.Negotiating";
	case TETHER_RX_STATE_CONNECTED:
		return "Receive.State.Connected";
	case TETHER_RX_STATE_FAILED:
		return "Receive.State.Failed";
	case TETHER_RX_STATE_CLOSED:
		return "Receive.State.Closed";
	}
	return "Receive.State.Failed";
}

class ReceiveDialog : public QDialog {
public:
	ReceiveDialog();
	~ReceiveDialog() override;

private:
	void onRegisterToken();
	void onForgetToken();
	void refreshTokenList();
	void releaseAllSessions();

	QLineEdit *tokenField_ = nullptr;
	QPushButton *registerBtn_ = nullptr;
	QListWidget *tokenList_ = nullptr;
	QPushButton *forgetBtn_ = nullptr;
	QLabel *hintLabel_ = nullptr;
	QTimer *refreshTimer_ = nullptr;

	// One session ref per registered token. Dialog owns refs; releasing here
	// drops the dialog's hold (other subscribers — e.g. Tether-Quelle sources
	// — may keep the session alive).
	std::unordered_map<std::string, tether_receive_session_t *> sessions_;
};

static QPointer<ReceiveDialog> g_dialog;

static QString fromTr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

ReceiveDialog::ReceiveDialog() : QDialog(nullptr)
{
	setWindowTitle(fromTr("Receive.Title"));
	setMinimumWidth(520);

	auto *root = new QVBoxLayout(this);

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

	root->addSpacing(8);
	auto *sessionsLabel = new QLabel(fromTr("Receive.Sessions"), this);
	root->addWidget(sessionsLabel);
	tokenList_ = new QListWidget(this);
	root->addWidget(tokenList_, 1);
	forgetBtn_ = new QPushButton(fromTr("Receive.Forget"), this);
	forgetBtn_->setEnabled(false);
	root->addWidget(forgetBtn_);

	hintLabel_ = new QLabel(fromTr("Receive.Hint"), this);
	hintLabel_->setWordWrap(true);
	hintLabel_->setStyleSheet(QStringLiteral("color: gray;"));
	root->addWidget(hintLabel_);

	connect(registerBtn_, &QPushButton::clicked, this, &ReceiveDialog::onRegisterToken);
	connect(tokenField_, &QLineEdit::returnPressed, this, &ReceiveDialog::onRegisterToken);
	connect(tokenList_, &QListWidget::currentRowChanged, this,
		[this](int row) { forgetBtn_->setEnabled(row >= 0); });
	connect(forgetBtn_, &QPushButton::clicked, this, &ReceiveDialog::onForgetToken);

	// Adopt every persisted known token as a live session immediately, so
	// reopening the dialog after restart picks up where we left off.
	size_t n = 0;
	char **persisted = tether_known_tokens_snapshot(&n);
	for (size_t i = 0; i < n; ++i) {
		std::string tok = persisted[i];
		if (sessions_.find(tok) == sessions_.end()) {
			tether_receive_session_t *s = tether_receive_session_get(tok.c_str());
			if (s) {
				sessions_[tok] = s;
			}
		}
	}
	tether_known_tokens_free_snapshot(persisted, n);

	refreshTokenList();

	refreshTimer_ = new QTimer(this);
	refreshTimer_->setInterval(750);
	connect(refreshTimer_, &QTimer::timeout, this, &ReceiveDialog::refreshTokenList);
	refreshTimer_->start();
}

ReceiveDialog::~ReceiveDialog()
{
	releaseAllSessions();
}

void ReceiveDialog::releaseAllSessions()
{
	for (auto &kv : sessions_) {
		tether_receive_session_release(kv.second);
	}
	sessions_.clear();
}

void ReceiveDialog::onRegisterToken()
{
	const QString token = tokenField_->text().trimmed().toUpper();
	if (token.isEmpty()) {
		return;
	}
	const QByteArray ba = token.toUtf8();
	tether_known_tokens_add(ba.constData());

	const std::string key = ba.constData();
	if (sessions_.find(key) == sessions_.end()) {
		tether_receive_session_t *s = tether_receive_session_get(ba.constData());
		if (s) {
			sessions_[key] = s;
		}
	}
	tokenField_->clear();
	refreshTokenList();
}

void ReceiveDialog::onForgetToken()
{
	QListWidgetItem *it = tokenList_->currentItem();
	if (!it) {
		return;
	}
	QString token = it->data(Qt::UserRole).toString();
	if (token.isEmpty()) {
		return;
	}
	const QByteArray ba = token.toUtf8();
	tether_known_tokens_remove(ba.constData());

	auto found = sessions_.find(ba.constData());
	if (found != sessions_.end()) {
		tether_receive_session_release(found->second);
		sessions_.erase(found);
	}
	refreshTokenList();
}

void ReceiveDialog::refreshTokenList()
{
	// Preserve current selection across re-population.
	QString selected;
	if (QListWidgetItem *cur = tokenList_->currentItem()) {
		selected = cur->data(Qt::UserRole).toString();
	}
	tokenList_->clear();

	// Pull the canonical set of tokens from the persisted registry — keeps
	// what we show in lock-step with what survives a restart.
	size_t n = 0;
	char **persisted = tether_known_tokens_snapshot(&n);
	for (size_t i = 0; i < n; ++i) {
		std::string tok = persisted[i];
		// Make sure we hold a session ref for every persisted token.
		if (sessions_.find(tok) == sessions_.end()) {
			tether_receive_session_t *s = tether_receive_session_get(tok.c_str());
			if (s) {
				sessions_[tok] = s;
			}
		}
		tether_receive_session_t *s = sessions_.count(tok) ? sessions_[tok] : nullptr;
		tether_receive_state_t st = tether_receive_session_state(s);
		QString state = fromTr(state_label_key(st));
		QString label = QStringLiteral("%1   —   %2").arg(QString::fromUtf8(tok.c_str()), state);
		auto *item = new QListWidgetItem(label, tokenList_);
		item->setData(Qt::UserRole, QString::fromUtf8(tok.c_str()));
		if (QString::fromUtf8(tok.c_str()) == selected) {
			tokenList_->setCurrentItem(item);
		}
	}
	tether_known_tokens_free_snapshot(persisted, n);

	// Drop session refs for tokens that disappeared from the registry.
	for (auto it = sessions_.begin(); it != sessions_.end();) {
		bool found = false;
		for (int i = 0; i < tokenList_->count(); ++i) {
			if (tokenList_->item(i)->data(Qt::UserRole).toString() ==
			    QString::fromUtf8(it->first.c_str())) {
				found = true;
				break;
			}
		}
		if (!found) {
			tether_receive_session_release(it->second);
			it = sessions_.erase(it);
		} else {
			++it;
		}
	}
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
