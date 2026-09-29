// SteBeam - laser beam profiler
// Copyright (C) 2026 Stefano's AI Lab
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library.  If not, see <https://www.gnu.org/licenses/>.

#include "shutter_controller.h"
#include "i18n.h"
#include <QFileInfo>

ShutterController::ShutterController(const QString& exePath, QObject* parent)
	: QObject(parent), m_exe(exePath)
{
	connect(&m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
	        this, [this](int code, QProcess::ExitStatus) {
		m_busy = false;
		bool ok = (code == 0);
		// Device status messages follow the UI language switch;
		// status wording is kept short (a few words) -- troubleshooting details go to DEBUG372.log.
		emit result(ok ? L("快门已应答", "Shutter OK")
		               : L("快门无应答", "No response"), ok);
	});
}

bool ShutterController::exeExists() const
{
	return QFileInfo::exists(m_exe);
}

void ShutterController::command(bool open)
{
	if (m_busy || !exeExists()) {
		emit result(m_busy ? L("命令忙", "Busy")
		                   : L("无驱动", "No driver"), false);
		return;
	}
	m_busy = true;
	m_proc.start(m_exe, { "shot", open ? "ea" : "0a" });
}

// Presence check (not every user owns the shutter): "DEBUG372 probe" enumerates devices by VID/PID.
// Exit code 0=present, 2=no device, 3=CreateFile failed (driver/occupied) -- only 0 counts as
// present; a missing exe reports "not present" directly. The protocol offers no query path for the
// physical open/closed state (replies are command echoes only), so we test presence, not state.
void ShutterController::probe()
{
	if (!exeExists()) { emit connected(false); return; }
	if (m_probe.state() != QProcess::NotRunning) return;   // previous probe still running: ignore, the in-flight one reports
	connect(&m_probe, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
		emit connected(code == 0);
	}, Qt::SingleShotConnection);
	m_probe.start(m_exe, { "probe" });
}
