/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hub-dialog.hpp"
#include "sender-dialog.hpp"
#include "receive-dialog.hpp"

extern "C" {
#include <obs-module.h>
}

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

namespace {

class HubDialog : public QDialog {
public:
	HubDialog();

private:
	QPushButton *shareBtn_ = nullptr;
	QPushButton *receiveBtn_ = nullptr;
};

static QPointer<HubDialog> g_hub;

static QString fromTr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

HubDialog::HubDialog() : QDialog(nullptr)
{
	setWindowTitle(fromTr("Hub.Title"));
	setMinimumWidth(440);

	auto *root = new QVBoxLayout(this);
	auto *prompt = new QLabel(fromTr("Hub.Prompt"), this);
	QFont promptFont = prompt->font();
	promptFont.setPointSizeF(promptFont.pointSizeF() * 1.2);
	promptFont.setBold(true);
	prompt->setFont(promptFont);
	root->addWidget(prompt);
	root->addSpacing(8);

	shareBtn_ = new QPushButton(fromTr("Hub.Share"), this);
	auto *shareDesc = new QLabel(fromTr("Hub.Share.Description"), this);
	shareDesc->setWordWrap(true);
	shareDesc->setStyleSheet(QStringLiteral("color: gray;"));
	root->addWidget(shareBtn_);
	root->addWidget(shareDesc);
	root->addSpacing(12);

	receiveBtn_ = new QPushButton(fromTr("Hub.Receive"), this);
	auto *recvDesc = new QLabel(fromTr("Hub.Receive.Description"), this);
	recvDesc->setWordWrap(true);
	recvDesc->setStyleSheet(QStringLiteral("color: gray;"));
	root->addWidget(receiveBtn_);
	root->addWidget(recvDesc);

	connect(shareBtn_, &QPushButton::clicked, this, [this] {
		tether_open_sender_dialog();
		close();
	});
	connect(receiveBtn_, &QPushButton::clicked, this, [this] {
		tether_open_receive_dialog();
		close();
	});
}

} // namespace

extern "C" void tether_open_hub_dialog(void)
{
	auto open = [] {
		if (!g_hub) {
			g_hub = new HubDialog();
			g_hub->setAttribute(Qt::WA_DeleteOnClose);
		}
		g_hub->show();
		g_hub->raise();
		g_hub->activateWindow();
	};
	if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
		open();
	} else {
		QMetaObject::invokeMethod(QCoreApplication::instance(), open, Qt::QueuedConnection);
	}
}

extern "C" void tether_close_all_dialogs(void)
{
	if (g_hub) {
		g_hub->close();
		g_hub = nullptr;
	}
	tether_close_sender_dialog();
	tether_close_receive_dialog();
}
