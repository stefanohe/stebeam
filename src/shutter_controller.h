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

// Shutter control: drives the shutter's GCI-73M-USB driver box through the DEBUG372.exe CLI via QProcess
// (send command + read back the MCU reply; exit code 0 = confirmed by the MCU).
#pragma once
#include <QObject>
#include <QProcess>

class ShutterController : public QObject
{
	Q_OBJECT
public:
	explicit ShutterController(const QString& exePath, QObject* parent = nullptr);

	void command(bool open);            // true=OPEN(ea) false=CLOSE(0a)
	void probe();                       // presence check: "DEBUG372 probe" enumerates devices, exit0=present -> connected(true)
	bool exeExists() const;

signals:
	void result(const QString& text, bool confirmed);
	void connected(bool ok);            // probe result (reported at startup/retry; the app stays fully usable without a shutter)

private:
	QProcess m_proc;
	QProcess m_probe;                   // separate process channel for probe, so it never collides with command's m_busy
	QString m_exe;
	bool m_busy = false;
};
