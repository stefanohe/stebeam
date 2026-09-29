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

#include "mainwindow.h"
#include "chart_view.h"
#include "i18n.h"
#include "mvs_source.h"
#include "simulated_source.h"
#include "spot_view.h"
#include "track_view.h"
#include "view3d.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>   // realtime frame-rate column width measured against the widest readout
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QStatusBar>
#include <QStyle>        // numeric-column alignment calibration via the SE_CheckBoxContents sub-control rect
#include <QStyleOptionButton>   // checkbox style option (take the text-area left edge after initFrom)
#include <QStyleOptionComboBox> // CenterCombo centered custom paint
#include <QStylePainter>        // CenterCombo centered custom paint
#include <QTabWidget>
#include <QTextStream>
#include <QToolBar>
#include <QVBoxLayout>

#include <QFile>
#include <QFileInfo>   // load-settings dialog default directory
#include <QFontInfo>
#include <QPixmap>
#include <QProcess>
#include <QScreen>   // reads available screen height to correct the view area's initial 1280:1024 ratio
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numeric>

// UI language g_cn/L: see i18n.h (shared so the custom-painted views spot_view/view3d/track_view pull words from the same source)

// A combo box whose current text is horizontally centered -- in non-editable state the style engine always draws
// combo text left-aligned and Qt offers no alignment property (the editable+readonly workaround changes
// click/wheel interaction, rejected); this class keeps the frame painted in the native style (currentText cleared,
// frame only) and paints the text itself centered in the edit-field rect = behavior 100% unchanged (click
// open/close, wheel, keyboard all identical to native), pure Qt custom paint with zero system dependency; the
// popup list is still painted left-aligned by the view.
class CenterCombo : public QComboBox {
public:
	using QComboBox::QComboBox;
protected:
	void paintEvent(QPaintEvent*) override
	{
		QStylePainter p(this);
		QStyleOptionComboBox opt;
		initStyleOption(&opt);
		opt.currentText.clear();   // frame painted natively (text painted by hand)
		p.drawComplexControl(QStyle::CC_ComboBox, opt);
		const QRect r = style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, this);
		p.drawItemText(r.adjusted(2, 0, -2, 0), Qt::AlignHCenter | Qt::AlignVCenter,
		               palette(), isEnabled(), currentText(), QPalette::Text);
	}
};

// Writable data directory: use the exe directory when writable (portable-install behavior), otherwise fall back
// to %APPDATA%\SteBeam. Once installed into Program Files a non-elevated process cannot write the install
// directory and every write of settings.ini/realtime_csv would silently fail, so the fallback is mandatory.
// Writability is decided by an actual write test -- QFileInfo::isWritable() on Windows only checks the
// FILE_ATTRIBUTE_READONLY bit, not ACL grants; Program Files install subdirectories (created by the installer,
// no read-only bit but ACL-denied writes) false-positive on the attribute bit. Open and delete a real probe
// file with QFile: writable only if the write actually succeeds.
static QString dataDir()
{
	static QString cached;
	if (!cached.isEmpty()) return cached;
	const QString appDir = QCoreApplication::applicationDirPath();
	const QString probe = appDir + QString("/.write_test_%1.tmp").arg(QCoreApplication::applicationPid());
	QFile t(probe);
	if (t.open(QIODevice::WriteOnly)) { t.write("x"); t.close(); QFile::remove(probe); cached = appDir; return cached; }
	const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(d);
	cached = d;
	return cached;
}

// Default directory for save-image/save-CSV dialogs = Documents\SteBeam (the installed app's cwd can be
// Program Files, which is unwritable, so a bare-file-name default would open the dialog in a read-only area)
static QString saveDir()
{
	const QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SteBeam";
	QDir().mkpath(d);
	return d;
}

// Power/intensity readouts: adaptive unit conversion. The tier floor extends down to nW/pW -- with the
// placeholder calibration constants (e-/ADU=1, QE=60%) at 1064nm the magnitude genuinely lands in pW~nW, so a
// higher floor would display "0.000"; after single-point calibration against a power meter (calibration
// coefficient) it naturally steps up to uW/mW/W. The ADU tier does no conversion.
// (Defined at the file head: buildPanel's background-sampling report line also picks its unit by background magnitude, so this must be visible before buildPanel)
static const double kPowScale[] = { 1.0, 1e-3, 1e-6, 1e-9, 1e-12 };
static const char* kPowUnit[] = { "W", "mW", "µW", "nW", "pW" };
// Tier-selection criterion: when the current tier's 3 decimals leave only the last digit significant
// (value < 0.01 x current tier), drop one tier, guaranteeing at least two significant digits even for small
// values (a plain "value < 1 drops a tier" rule stops dropping at e.g. 0.007 uW and the readout loses readability)
static int powPickIdx(double w)
{
	int i = 0;
	while (i < 4 && std::fabs(w) > 0 && std::fabs(w) < kPowScale[i] * 0.01) ++i;
	return i;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
	m_clock.start();   // size-chart time base
	{   // language is finalized before buildUi: settings.ini ui/lang=en selects English, missing/anything else = Chinese;
	    // path goes through dataDir() -- the installed build cannot write Program Files, so the ini lands in %APPDATA%\SteBeam
		QSettings ls(dataDir() + "/settings.ini", QSettings::IniFormat);
		g_cn = ls.value("ui/lang", "cn").toString() != "en";
	}
	qRegisterMetaType<Frame>("Frame");
	// The shutter driver tool DEBUG372.exe is not distributed here (users with the hardware install it themselves)
	// -- it is looked up in the exe directory (drop it in after installation and integration works); when no
	// shutter is present the software degrades automatically.
	m_shutter = new ShutterController(
		QCoreApplication::applicationDirPath() + "/DEBUG372.exe", this);
	connect(m_shutter, &ShutterController::result, this,
	        [this](const QString& t, bool ok) {
		m_shutterStatus->setText(t);
		m_shutterStatus->setStyleSheet(ok ? "color:#0a0;" : "color:#c00;");
		// a command confirmed by an MCU reply = the device is certainly present (second presence evidence beyond the probe, covering hot-plug)
		if (ok && !m_shutterConnected) { m_shutterConnected = true; updateBkgState(); }
	});
	// Shutter presence detection: when absent, no error, just an honest notice; background subtraction
	// automatically degrades to manual-occlusion mode -- the software must work fully without a shutter.
	connect(m_shutter, &ShutterController::connected, this, [this](bool ok) {
		m_shutterConnected = ok;
		m_shutterStatus->setText(ok ? L("快门就绪", "Ready")
		                            : L("快门未连接", "No shutter"));   // short phrases (5 chars or fewer)
		m_shutterStatus->setStyleSheet(ok ? "color:#0a0;" : "color:#888;");
		updateBkgState();
	});

	m_csv.setDir(dataDir() + "/realtime_csv");   // realtime CSV default folder (created if missing; via dataDir() the installed build lands in %APPDATA%\SteBeam;
	                                             // must precede loadSettingsIni -- a custom folder from the ini overrides it)
	buildUi();
	loadSettingsIni(iniPath(), true);   // auto-load settings.ini at startup (silent; manual load uses the file-picker dialog entry)
	m_sim = new SimulatedSource(this);
	attachSource(m_sim);
	setAcquiring(true);   // acquire from launch (start/stop button state synced)
	m_shutter->probe();   // presence detection at startup (async, result reported via the connected signal)
	// compliance probe: STEBEAM_ABOUT=<png path> auto-opens the About box at launch (screenshot and exit inside showAbout; verified once per language via the ui/lang pre-read)
	if (qEnvironmentVariableIsSet("STEBEAM_ABOUT"))
		QTimer::singleShot(1200, this, &MainWindow::showAbout);
}

void MainWindow::buildUi()
{
	setWindowTitle(L("SteBeam — AI 原生专业光斑测试分析系统    V0.1",
	                 "SteBeam — AI-Native Professional Beam Test & Analysis System    V0.1"));   // version number appended at the end (no "Version:" prefix, wide gap before it; the open-source build carries no trial suffix)

	addToolBar(buildRibbon());

	auto central = new QWidget(this);
	auto root = new QHBoxLayout(central);
	root->addWidget(buildViews(), 1);
	root->addWidget(buildPanel(), 0);
	setCentralWidget(central);
	// 1520 -> 1565: the English-mode ribbon's measured sizeHint (1508) exceeded the window width and pushed the
	// language button into the » extension menu; widening clears it (1920 available screen width leaves room);
	// the view ratio correction below auto-adds window height, and the view grows with the window width.
	// Also tracks the panel widening 445 -> 490 (+45 here) so the image area stays the same size.
	resize(1565, 860);
	// Initial view-area ratio = 1280:1024 (= the max sensor; a full frame fills it with no black bars): after
	// show, when the layout settles, add window height from the stack's real height (total width doesn't shrink;
	// width freed from the panel naturally goes to the view area = view grows); if the added height would exceed
	// the available screen height, instead shrink by real width.
	// Since the 2D view gained axis bands taking width and height, computing image height from the full stack
	// width always exceeds the drawable height -> the vertically centered image leaves a large gap = bottom
	// black band; now compute image height from the plot-area width (stack width - 2 x dynamic left/right axis
	// bands) then add the axis bands top/bottom = the image fills the plot area vertically, the bottom band is
	// only the axis annotation strip.
	QTimer::singleShot(0, this, [this] {
		const int sw = m_stack->width(), sh = m_stack->height();
		if (sw <= 0 || sh <= 0) return;
		const int ml = m_view->axisMLFor(kSensorW * sizeScale(), m_sizeUnit->currentIndex() == 0 ? 1 : 3);
		const int zoneV = SpotView::kAxisMT + SpotView::kAxisMB;
		const int wantH = (int)std::lround((sw - 2 * ml) * (kSensorH / (double)kSensorW)) + zoneV;
		if (height() + wantH - sh <= screen()->availableGeometry().height())
			resize(width(), height() + wantH - sh);
		else
			resize(width() - (sw - ((int)std::lround((sh - zoneV) * (kSensorW / (double)kSensorH)) + 2 * ml)), height());
	});

	onViewPage(0);   // initial page = 2D intensity
}

// Top tool ribbon: Qt has no native ribbon, so a toolbar with group captions approximates a ribbon layout (Capture/File/Config groups)
QToolBar* MainWindow::buildRibbon()
{
	auto* tb = new QToolBar("Ribbon", this);
	tb->setMovable(false);
	// Ribbon entries compacted one notch (tighter spacing / button padding / captions and group gaps) so the
	// language button fits fully within the default startup window width (no longer hidden behind » or floating over content)
	tb->setStyleSheet("QToolBar{spacing:2px;} QToolBar QPushButton{padding:1px 5px;}");
	auto caption = [tb](const QString& text) {
		auto* l = new QLabel(text, tb);
		l->setStyleSheet("color:#0b57a4;font-weight:bold;padding:0 3px;");
		tb->addWidget(l);
	};
	auto gap = [tb]() { auto* s = new QWidget(tb); s->setFixedWidth(10); tb->addWidget(s); };

	caption(L("采集", "Capture"));
	m_radioSim = new QRadioButton(L("模拟源", "Sim"), tb);
	m_radioCam = new QRadioButton("CMOS", tb);   // hardware brand hidden from the UI; CMOS is a term abbreviation, identical in both languages
	m_radioSim->setChecked(true);
	auto* srcGrp = new QButtonGroup(this);
	srcGrp->addButton(m_radioSim);
	srcGrp->addButton(m_radioCam);
	tb->addWidget(m_radioSim);
	tb->addWidget(m_radioCam);
	// green text badge after CMOS showing camera connection state (driven by the effective connectionChanged signal);
	// the separate "camera status" message group was removed -- the badge already states the status, a message would be redundant
	m_camConn = new QLabel(L("未连接", "Disconnected"), tb);
	m_camConn->setStyleSheet("color:#888;");
	tb->addWidget(m_camConn);
	// Acquisition start/stop (Start/Stop button after CMOS): checked = acquiring, calls the active source's
	// start/stop; stopping the camera source = stream down, and connectionChanged naturally flips the badge to
	// "Disconnected"; a failed start (camera absent / occupied) springs the button back.
	m_acqBtn = new QPushButton(L("停止采集", "Stop"), tb);
	m_acqBtn->setCheckable(true);
	m_acqBtn->setChecked(true);
	tb->addWidget(m_acqBtn);
	connect(m_acqBtn, &QPushButton::clicked, this, [this] { setAcquiring(m_acqBtn->isChecked()); });
	connect(m_radioSim, &QRadioButton::toggled, this, &MainWindow::onSourceToggled);
	connect(m_radioCam, &QRadioButton::toggled, this, &MainWindow::onSourceToggled);
	// English mode uses short words: full-spelled English made the ribbon exceed the default window width and pushed the language button into the >> overflow (proven by English smoke screenshots) -- the shutter/save/config button meanings are self-evident under their group captions
	auto bOpen = new QPushButton(L("开快门", "Open"), tb);
	auto bClose = new QPushButton(L("关快门", "Close"), tb);
	m_shutterStatus = new QLabel(L("检测中", "…"), tb);   // rewritten via the connected signal after the startup probe
	tb->addWidget(bOpen);
	tb->addWidget(bClose);
	tb->addWidget(m_shutterStatus);
	connect(bOpen, &QPushButton::clicked, this, &MainWindow::shutterOpen);
	connect(bClose, &QPushButton::clicked, this, &MainWindow::shutterClose);
	gap();

	caption(L("文件", "File"));
	auto bPng = new QPushButton(L("存 PNG", "PNG"), tb);
	auto bCsv = new QPushButton(L("存 CSV", "CSV"), tb);
	auto bRaw = new QPushButton(L("存 RAW", "RAW"), tb);
	tb->addWidget(bPng);
	tb->addWidget(bCsv);
	tb->addWidget(bRaw);
	connect(bPng, &QPushButton::clicked, this, &MainWindow::savePng);
	connect(bCsv, &QPushButton::clicked, this, &MainWindow::saveCsv);
	connect(bRaw, &QPushButton::clicked, this, &MainWindow::saveRaw);
	// Realtime CSV saving: once enabled, every Nth frame the 2D intensity matrix is stored to the target folder
	// named by the frame's capture time (CsvRecorder writes asynchronously with drop-on-busy backpressure; a
	// full-frame CSV is ~6MB, a synchronous GUI-thread write would surely stutter); off by default and the button
	// state is not persisted -- prevents auto-recording after restart; default folder exe\realtime_csv,
	// customizable via the Path button (persisted in ini setup/csvDir); the button text uses action semantics
	// "Start/Stop" wording (On/Off is ambiguous -- "Off" can read as not recording while it is); the
	// "Saved N" count always shows the true number, never replaced by a "recording" label.
	auto* csvPreLbl = new QLabel(L("实时存储", "Realtime Save"), tb);
	csvPreLbl->setStyleSheet("color:#0b57a4;font-weight:bold;");
	tb->addWidget(csvPreLbl);
	m_csvBtn = new QPushButton(L("开始", "Start"), tb);
	m_csvBtn->setCheckable(true);
	auto* csvEveryLbl = new QLabel(L("每", "Every"), tb);
	m_csvEvery = new QSpinBox(tb);
	m_csvEvery->setRange(1, 1000);
	m_csvEvery->setValue(10);
	m_csvEvery->setSuffix(L(" 帧", " frames"));
	m_csvLbl = new QLabel(L("已存 0", "Saved 0"), tb);
	m_csvDirBtn = new QPushButton(L("存储路径", "Path"), tb);   // custom storage folder (persisted in ini setup/csvDir); a named button rather than "..."
	m_csvDirBtn->setToolTip(L("自定义存储文件夹（当前：", "Custom folder (current: ") + m_csv.dir() + "）");
	tb->addWidget(m_csvBtn);
	tb->addWidget(csvEveryLbl);
	tb->addWidget(m_csvEvery);
	tb->addWidget(m_csvLbl);
	tb->addWidget(m_csvDirBtn);
	connect(m_csvBtn, &QPushButton::toggled, this, [this](bool on) {
		m_csvBtn->setText(on ? L("停止", "Stop") : L("开始", "Start"));
	});
	connect(m_csvDirBtn, &QPushButton::clicked, this, [this] {
		if (m_csvBtn->isChecked()) m_csvBtn->setChecked(false);   // stop recording before changing folders (prevents in-flight frames writing to the old directory)
		const QString d = QFileDialog::getExistingDirectory(this, L("选择实时存 CSV 文件夹", "Choose realtime CSV folder"), m_csv.dir());
		if (d.isEmpty()) return;
		m_csv.setDir(d);
		m_csvDirBtn->setToolTip(L("自定义存储文件夹（当前：", "Custom folder (current: ") + m_csv.dir() + "）");
		statusBar()->showMessage(L("实时存 CSV 文件夹已设为 ", "Realtime CSV folder set to ") + m_csv.dir(), 4000);
	});
	gap();

	caption(L("配置", "Config"));
	auto bCfgSave = new QPushButton(L("保存设置", "Save"), tb);
	auto bCfgLoad = new QPushButton(L("加载设置", "Load"), tb);
	auto bCfgReset = new QPushButton(L("恢复默认", "Reset"), tb);
	tb->addWidget(bCfgSave);
	tb->addWidget(bCfgLoad);
	tb->addWidget(bCfgReset);
	// Save/load settings open dialogs -- pick the storage location + custom file name (default remains the settings.ini next to the exe)
	connect(bCfgSave, &QPushButton::clicked, this, [this] {
		const QString fn = QFileDialog::getSaveFileName(this, L("保存配置文件", "Save config file"),
		                                                iniPath(), L("配置文件 (*.ini)", "Config (*.ini)"));
		if (fn.isEmpty()) return;
		saveSettingsIni(fn);
	});
	connect(bCfgLoad, &QPushButton::clicked, this, [this] {
		const QString fn = QFileDialog::getOpenFileName(this, L("加载配置文件", "Load config file"),
		                                                QFileInfo(iniPath()).absolutePath(), L("配置文件 (*.ini)", "Config (*.ini)"));
		if (fn.isEmpty()) return;
		loadSettingsIni(fn, false);
	});
	connect(bCfgReset, &QPushButton::clicked, this, &MainWindow::resetSettingsIni);
	// open-source compliance: the About button (the GPL-3.0 terms incorporated by LGPL-3.0 require presenting Appropriate Legal Notices in an interactive interface)
	auto bAbout = new QPushButton(L("关于", "About"), tb);
	tb->addWidget(bAbout);
	connect(bAbout, &QPushButton::clicked, this, &MainWindow::showAbout);

	// Language switch button (top-right Chinese/En toggle): a spacer pushes it to the far right; clicking writes
	// ui/lang, relaunches a new process, then quits this one -- all strings are finalized once during buildUi,
	// so nothing can be missed at runtime.
	auto* sp = new QWidget(tb);
	sp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	tb->addWidget(sp);
	// Small button inside the ribbon layout (can go behind », never floats over content); rounded rectangle so it matches the button row's height naturally.
	// Its label names the target language -- Chinese UI shows "En" (blue), English UI shows "中" (red); the color matches the language the button advertises.
	m_langBtn = new QPushButton(g_cn ? "En" : "中", tb);
	m_langBtn->setStyleSheet(QString("QPushButton{border-radius:5px;background:%1;color:white;font-weight:bold;border:none;}"
	                                 "QPushButton:hover{opacity:0.9;}").arg(g_cn ? "#0b57a4" : "#c0392b"));
	m_langBtn->setToolTip(L("切换为英文（重启生效）", "切换为中文（重启生效）"));
	tb->addWidget(m_langBtn);
	connect(m_langBtn, &QPushButton::clicked, this, [this] {
		QSettings s(iniPath(), QSettings::IniFormat);
		s.setValue("ui/lang", g_cn ? "en" : "cn");
		s.sync();
		QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList());
		qApp->quit();
	});
	return tb;
}

// Left five-view stack + bottom switch-button row; the stack naturally renders only the visible page, feeding schedule in onFrame/onViewPage
QWidget* MainWindow::buildViews()
{
	auto* w = new QWidget;
	auto* lay = new QVBoxLayout(w);
	lay->setContentsMargins(4, 4, 4, 4);
	m_stack = new QStackedWidget;

	// page0 2D intensity = SpotView directly (the replay control row lives below the stack rather than inside it,
	// shared by the 2D intensity / 3D surface / beam profile pages -- the views share the 2D replay function area)
	m_view = new SpotView;
	m_stack->addWidget(m_view);
	m_replayRow = new QWidget;
	auto* rbLay = new QHBoxLayout(m_replayRow);
	rbLay->setContentsMargins(4, 1, 4, 1);
	// Replay/live toggle button (button-driven switching replaces drag-to-autoswitch: the slider is disabled and
	// undraggable in live mode; switching to replay auto-stops acquisition so it can be dragged freely; switching
	// back to live auto-resumes acquisition and returns to the live frame)
	m_rbBtn = new QPushButton(L("切换回看", "Replay"), m_replayRow);
	m_rbBtn->setCheckable(true);
	m_rbMax = new QSpinBox(m_replayRow);
	m_rbMax->setRange(0, 100);   // hard cap 100 (out-of-range input clamps back on focus-out)
	m_rbMax->setValue(100);
	m_rbMax->setSuffix(L(" 帧", " frames"));
	m_rbMax->setToolTip(L("回看帧数上限（0=取消回看功能）", "Max replay frames (0 = disable replay)"));
	m_rbSl = new QSlider(Qt::Horizontal, m_replayRow);
	m_rbSl->setRange(0, 0);   // max = buffered frame count grows with recording; rightmost = newest frame
	m_rbSl->setEnabled(false);   // disabled in live mode (draggable only in replay mode)
	m_rbLbl = new QLabel(L("实时", "Live"), m_replayRow);
	m_rbSpin = new QSpinBox(m_replayRow);
	m_rbSpin->setRange(0, 0);
	m_rbSpin->setPrefix(L("回看 ", "Back "));
	m_rbSpin->setSuffix(L(" 帧", " frames"));
	m_rbSpin->setToolTip(L("定位到回看第几帧（正序：1=最旧帧，越大越新；0=实时）", "Jump to replay frame (1 = oldest, larger = newer; 0 = live)"));   // frame numbers in forward order
	m_rbSpin->setEnabled(false);   // disabled in live mode
	m_tsLbl = new QLabel(m_replayRow);
	m_tsLbl->setStyleSheet("color:#0a0;font-family:Consolas,monospace;");
	m_tsLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	rbLay->addWidget(m_rbBtn);
	rbLay->addWidget(m_rbMax);
	rbLay->addWidget(m_rbSl, 1);
	rbLay->addWidget(m_rbLbl);
	rbLay->addWidget(m_rbSpin);
	rbLay->addStretch(1);
	rbLay->addWidget(m_tsLbl);
	connect(m_rbBtn, &QPushButton::toggled, this, [this](bool replayMode) {
		if (replayMode) {
			if (m_acqBtn->isChecked()) setAcquiring(false);   // auto-stop acquisition, freezing the buffer for scrubbing
			m_rbSl->setEnabled(true);
			m_rbSpin->setEnabled(true);
			m_rbBtn->setText(L("切换实时", "Go live"));
			// entering replay = position at the newest frame; the slider range 0..sz-1 covers only replay frames,
			// dragging to the far right no longer switches back to live (only the Go live button switches modes)
			onReplaySel((int)m_rb.size() - 1);
		} else {
			m_rbSl->setEnabled(false);
			m_rbSpin->setEnabled(false);
			m_rbBtn->setText(L("切换回看", "Replay"));
			onReplaySel(-1);                                  // back to the live frame
			if (!m_acqBtn->isChecked()) setAcquiring(true);   // auto-resume acquisition
		}
	});
	connect(m_rbSl, &QSlider::valueChanged, this, [this](int v) {
		const int sz = (int)m_rb.size();
		onReplaySel(v >= sz ? -1 : v);   // the replay-mode range stops at sz-1; this branch is only reachable from live-mode program paths
	});
	connect(m_rbSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int k) {
		const int sz = (int)m_rb.size();
		onReplaySel(k <= 0 ? -1 : std::min(k - 1, sz - 1));   // replay frame k (forward order: 1 = oldest) -> index from oldest
	});
	connect(m_rbMax, qOverload<int>(&QSpinBox::valueChanged), this, [this](int mx) {
		while ((int)m_rb.size() > mx) m_rb.pop_front();
		onReplaySel(m_rbSel);   // out-of-range clamping is handled by onReplaySel per mode (replay = newest frame, live = -1)
	});

	// page1 3D surface: same jet colormap + dark-gray background as the 2D view (colormap.h); fed frames only while this page is visible (exclusive feeding saves compute)
	m_3d = new View3D;
	m_stack->addWidget(m_3d);

	// page2 beam profile (QtCharts replaced by the custom-painted LineChartView -- Qt6Charts is GPL-3.0-only per
	// the local SBOM check; with it removed the whole program ships under LGPL-3.0)
	m_profileChart = new LineChartView;
	m_stack->addWidget(m_profileChart);

	// page3 realtime beam-size chart (84% enclosed-energy widths X/Y over a sliding frame window, same custom painter)
	m_sizeChart = new LineChartView;
	m_stack->addWidget(m_sizeChart);

	// page4 centroid tracking: custom-painted centroid trajectory scatter view (y up = sensor bottom-left origin,
	// the whole app's unified convention; QtCharts value axes cannot invert and silently skip axis-attached
	// series -- the root cause of "tracking shows only the frame, no spots"; see the track_view.h header note)
	m_track = new TrackView;
	m_track->setSource(&m_trackHist);
	// container = TrackView + bottom clear row (the clear-window button shows only on this page)
	auto* page4 = new QWidget;
	auto* p4lay = new QVBoxLayout(page4);
	p4lay->setContentsMargins(0, 0, 0, 0);
	p4lay->setSpacing(2);
	p4lay->addWidget(m_track, 1);
	m_trackClearRow = new QWidget;
	auto* tkLay = new QHBoxLayout(m_trackClearRow);
	tkLay->setContentsMargins(4, 1, 4, 1);
	m_trackClearBtn = new QPushButton(L("清空轨迹", "Clear track"), m_trackClearRow);
	m_trackClearBtn->setToolTip(L("清空质心轨迹历史（新点继续记录）", "Clear centroid track history (new points keep recording)"));
	tkLay->addStretch(1);   // button centered (equal stretches on both sides)
	tkLay->addWidget(m_trackClearBtn);
	tkLay->addStretch(1);
	p4lay->addWidget(m_trackClearRow);
	m_stack->addWidget(page4);
	connect(m_trackClearBtn, &QPushButton::clicked, this, [this] {
		m_trackHist.clear();
		m_track->update();   // TrackView holds a pointer to m_trackHist and pulls the full set at paint time
	});

	lay->addWidget(m_stack, 1);
	lay->addWidget(m_replayRow);   // replay row hugs the stack's bottom edge, shared by the 2D/3D/profile pages (onViewPage controls visibility)

	// Bottom view-switch button row (view names unified as 2D Intensity / 3D Surface / Beam Profile / Beam Size / Centroid Track, matching the Setup axis-group titles)
	auto* btnRow = new QHBoxLayout;
	static const char* namesZh[5] = { "2D 强度", "3D 曲面", "光斑剖面", "光斑尺寸", "质心追踪" };
	static const char* namesEn[5] = { "2D Intensity", "3D Surface", "Beam Profile", "Beam Size", "Centroid Track" };
	for (int i = 0; i < 5; ++i) {
		m_viewBtn[i] = new QPushButton(L(namesZh[i], namesEn[i]));
		m_viewBtn[i]->setCheckable(true);
		m_viewBtn[i]->setAutoExclusive(true);
		btnRow->addWidget(m_viewBtn[i]);
		connect(m_viewBtn[i], &QPushButton::clicked, this, [this, i] { onViewPage(i); });
	}
	m_viewBtn[0]->setChecked(true);
	lay->addLayout(btnRow);
	return w;
}

// Built-in QE curve: digitized from the grayscale reference line (gray_line = intrinsic QE without a color
// filter, applicable to this monochrome camera) of the official datasheet's sensor response curve
// (camera_qe_curve_MV-CU013-A0UM.csv, accuracy +/-2~3 percentage points); clamped below 400nm, log-linear
// extrapolation across 940~1100nm (silicon band-edge cutoff ~1000nm; at 1064nm the response is a very weak
// parasitic ~5% -- absolute power still requires single-point calibration against a power meter); clamped above 1100nm.
static double builtinQePct(double nm)
{
	static const double wl[] = { 400, 420, 440, 460, 480, 500, 520, 540, 560, 580, 600,
	                             620, 640, 660, 680, 700, 750, 800, 850, 900, 940 };
	static const double qe[] = { 59, 66, 71, 74, 79, 70, 80, 82, 80, 84, 81,
	                             79, 76, 71, 64, 61, 53, 41, 33, 23, 16 };
	constexpr int N = (int)(sizeof(wl) / sizeof(wl[0]));
	if (nm <= wl[0]) return qe[0];
	if (nm >= wl[N - 1]) {
		if (nm > 1100.0) nm = 1100.0;   // valid-domain upper bound (400~1100 nm)
		double slope = (std::log(qe[N - 1]) - std::log(qe[N - 2])) / (wl[N - 1] - wl[N - 2]);
		return qe[N - 1] * std::exp(slope * (nm - wl[N - 1]));
	}
	for (int i = 0; i + 1 < N; ++i)
		if (nm <= wl[i + 1])
			return qe[i] + (qe[i + 1] - qe[i]) * (nm - wl[i]) / (wl[i + 1] - wl[i]);
	return qe[N - 1];
}

// Right panel: brand banner (top of the right panel) + Home/Setup/Acquisition tabs (blue-tab + three-column style)
QWidget* MainWindow::buildPanel()
{
	auto* w = new QWidget;
	// Fixed panel width, tuned so the horizontal scrollbar never appears in either language: the width is chosen
	// from real-platform measurements (STEBEAM_PANEL_DBG) of the widest content row per tab, per language, plus
	// margins and the vertical scrollbar, with headroom for wider font metrics on other Windows versions.
	// Two measurement traps to avoid: minimumSizeHint during buildPanel is bogus (layout not yet activated), and
	// the offscreen platform's tofu font metrics inflate widths ~1.8x.
	w->setFixedWidth(490);
	auto* lay = new QVBoxLayout(w);
	lay->setContentsMargins(6, 6, 6, 6);

	// Brand banner: circular transparent avatar (st-c.png, enlarged ~1.5x and kept close to the text) + script font over a gradient backing
	auto* bannerBox = new QWidget;
	bannerBox->setObjectName("bannerBox");
	bannerBox->setStyleSheet(
		"QWidget#bannerBox { border-radius: 6px;"
		" background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #0a2a52, stop:1 #14508c); }");
	auto* bannerLay = new QHBoxLayout(bannerBox);
	bannerLay->setContentsMargins(12, 8, 12, 8);
	QFont bf;   // Windows built-in script fonts, falling back in order
	bf.setFamilies({ "Segoe Script", "Brush Script MT", "KaiTi" });
	bf.setPointSize(18);
	bf.setItalic(true);
	const int avSide = (int)(QFontInfo(bf).pixelSize() * 1.6);   // font pixel height x1.6
	auto* avatar = new QLabel;
	QPixmap avPix(":/logo/st-c.png");   // circular transparent PNG; the blue gradient shows through the transparent areas
	if (!avPix.isNull())
		avatar->setPixmap(avPix.scaled(avSide, avSide, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	auto* banner = new QLabel("Stefano's AI Lab");
	banner->setFont(bf);
	banner->setStyleSheet("QLabel { color: white; background: transparent; }");
	// avatar + text centered as one unit (the old "avatar pinned left, text stretched" layout left too big a gap)
	bannerLay->addStretch();
	bannerLay->addWidget(avatar, 0, Qt::AlignVCenter);
	bannerLay->addSpacing(14);   // avatar/text spacing, opened up a touch once the panel widened
	bannerLay->addWidget(banner, 0, Qt::AlignVCenter);
	bannerLay->addStretch();
	lay->addWidget(bannerBox);

	m_tabs = new QTabWidget;
	lay->addWidget(m_tabs, 1);

	auto scroll = [](QWidget* content) {
		auto* sa = new QScrollArea;
		sa->setWidgetResizable(true);
		sa->setFrameShape(QFrame::NoFrame);
		sa->setWidget(content);
		return sa;
	};
	// right-panel entry names are uniformly left-aligned
	auto captionL = [](const QString& text) {
		auto* l = new QLabel(text);
		l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		return l;
	};

	// ── Home: Main Controls + Measures (Power/Diameter/Position, three columns of label+value+unit) ──
	auto* home = new QWidget;
	auto* homeLay = new QVBoxLayout(home);
	auto gMain = new QGroupBox(L("控制", "Controls"));
	auto* mainLay = new QFormLayout(gMain);
	m_diamDef = new CenterCombo;   // centered combo text (the 6 combos below likewise)
	m_diamDef->addItems({ "D4σ (ISO)", L("84% 能量", "84% Energy"), "FWHM" });
	m_diamDef->setToolTip(L("Measures 主直径口径",
	                        "Primary diameter definition shown in Measures"));
	m_sizeUnit = new CenterCombo;
	m_sizeUnit->addItems({ "px", "µm", "mm" });
	m_powUnit = new CenterCombo;
	m_powUnit->addItems({ "ADU", "µW", "mW", "W" });
	m_winCombo = new CenterCombo;
	m_winCombo->addItems({ "1min", "5min", "10min", "30min", "60min" });
	m_showCross = new QCheckBox(L("显示质心十字", "Show Centroid Crosshair"));
	m_showCross->setChecked(true);
	m_show84 = new QCheckBox(L("显示 84% 能量包围轮廓", "Show 84% Energy Enclosure"));
	m_show84->setChecked(true);
	m_shape84 = new CenterCombo;
	m_shape84->addItems({ L("椭圆轮廓", "Ellipse"), L("矩形轮廓", "Rectangle") });   // contour shape selectable
	m_rotSpot = new QCheckBox(L("实时光斑旋转（长轴转至正 Y）", "Real-time Spot Rotation (major axis to +Y)"));   // named "real-time" to divide roles with the one-shot button
	m_rotSpot->setToolTip(L("测试光斑可能倾斜：逐帧按二阶矩方位角把整帧旋转至椭圆长轴竖直（正 Y 方向），\n"
	                        "靶面裁剪为旋转后的最大内接矩形（相应缩小）；分析/视图/历史全按旋转后帧。\n"
	                        "自动检测用 50% 峰值门控（核心主导，弥散光晕不进掩膜，避免光晕伪长轴导致过转）。\n"
	                        "光斑抖动时帧尺寸会随之微变；要固定几何请改用下方「单次光斑旋转」",
	                        "Tilted spots: each frame is rotated so the ellipse major axis is vertical (+Y),\n"
	                        "target cropped to the largest inscribed rectangle; analysis/views/history all use the rotated frame.\n"
	                        "Auto-detect uses a 50%-peak gate so the diffuse halo never enters the mask (avoids over-rotation from a halo-induced false major axis).\n"
	                        "Frame size wobbles with the spot; use Rotate Spot Once below for fixed geometry."));
	m_rotOnce = new QPushButton(L("单次光斑旋转", "Rotate Spot Once"));   // checkable: press = enter one-shot (text becomes Cancel Spot Rotation), press again cancels;
	                                                                       // the active look is the native checked state (same as the Stop acquisition button)
	m_rotOnce->setCheckable(true);
	m_rotOnce->setToolTip(L("按下那一刻按当前配置计算一次旋转角（自定义角，或自动 50% 门控找平），此后固定该角持续旋转、\n"
	                        "不再逐帧重算——靶面几何与存 CSV 的矩阵行列尺寸随之稳定，不随光斑抖动变化。\n"
	                        "生效中按钮呈按下态高亮（同停止采集按钮样式）、文案变「取消光斑旋转」，再按一次即取消旋转；\n"
	                        "勾选实时/自定义=退出单次模式",
	                        "On press, computes the rotation angle once (custom angle, or auto 50%-gate leveling) and then\n"
	                        "keeps rotating by that fixed angle without re-detecting — the sensor geometry and the CSV matrix\n"
	                        "dimensions stay stable. While active the button shows a pressed highlight (same style as the Stop\n"
	                        "button) and reads Cancel Spot Rotation;\n"
	                        "press again to cancel. Checking real-time or custom angle also exits once-mode."));
	m_rotCustom = new QCheckBox(L("自定义角度（按下方角度旋转）", "Custom Angle (rotate by angle below)"));   // standalone rotation mode; checking it auto-clears the other rotation modes
	m_rotDeg = new QDoubleSpinBox;
	m_rotDeg->setRange(-180.0, 180.0);
	m_rotDeg->setDecimals(1);
	m_rotDeg->setSingleStep(0.1);
	m_rotDeg->setValue(0.0);
	m_rotDeg->setSuffix(" °");
	m_rotDeg->setEnabled(false);
	m_rotCustom->setToolTip(L("勾选后忽略自动检测，整帧按下方固定角度旋转（图像坐标，正=顺时针）",
	                          "When checked, auto-detect is ignored and the frame rotates by the fixed angle below (image coords, positive = clockwise)"));
	// Maintain sensor size after rotation: rotation no longer crops to the inscribed rectangle but enlarges the acquisition sensor by 1/t
	// (t = inscribed ratio, so the crop lands back at the original size = effectively a wider acquisition); clamps to the sensor cap when the
	// enlargement exceeds it (partial maintenance).
	m_rotKeepSize = new QCheckBox(L("旋转后维持靶面尺寸 (上限以内)", "Keep Sensor Size After Rotation (within limits)"));
	m_rotKeepSize->setToolTip(L("旋转会裁掉靶面边缘（内接矩形缩小）。勾选后自动把采集窗口按旋转角放大，"
	                            "使旋转后的画面维持旋转前的靶面尺寸；所需尺寸超过相机靶面上限时只能放大到上限。"
	                            "尺寸变化经应用靶面路径下发（相机短暂停流重配）",
	                            "Rotation crops the frame to its inscribed rectangle. When checked, the acquisition window "
	                            "is enlarged by the rotation angle so the rotated frame keeps its pre-rotation sensor size; "
	                            "if the required size exceeds the sensor limit it is clamped to the limit. Size changes are "
	                            "applied through the Apply ROI path (the camera briefly restarts)"));
	// Global selectability (units/conventions) is centralized in Main Controls: diameter definition, size/power units, acquisition time window, plus the 84% contour toggle/shape and spot rotation
	mainLay->addRow(L("光束直径定义", "Beam Diameter Definition"), m_diamDef);
	mainLay->addRow(L("尺寸单位", "Size Unit"), m_sizeUnit);
	mainLay->addRow(L("功率单位", "Power Unit"), m_powUnit);
	mainLay->addRow(L("采集时间窗", "Acquisition Time Window"), m_winCombo);
	// checkbox rows use the single-argument addRow = span from the label column (the old addRow(QString(),w) counted the long checkbox text into the field column's minimum width, inflating the Home page's English peak)
	mainLay->addRow(m_showCross);
	mainLay->addRow(m_show84);
	mainLay->addRow(L("包围轮廓形状", "Enclosure Shape"), m_shape84);
	mainLay->addRow(m_rotSpot);
	mainLay->addRow(m_rotOnce);   // one-shot rotation button directly below realtime rotation
	mainLay->addRow(m_rotCustom);
	mainLay->addRow(L("旋转角度", "Rotation Angle"), m_rotDeg);
	mainLay->addRow(m_rotKeepSize);   // directly below the rotation-angle row
	homeLay->addWidget(gMain);
	// the ROI group lives in Setup (wired at the end of buildPanel)

	auto gMeas = new QGroupBox(L("测量", "Measures"));
	auto* measLay = new QVBoxLayout(gMeas);
	// value column hugs the entry name (left-aligned); col0 entry column keeps 140px so values stay vertically aligned across the three sub-groups
	auto addRow = [&](QGridLayout* g, int row, const QString& name, QLabel*& val, const QString& unit,
	                  bool unitSync, QVector<QLabel*>* unitSink = nullptr) -> QLabel* {
		auto* cap = captionL(name);   // return the name label so a tooltip can be attached
		g->addWidget(cap, row, 0);
		val = new QLabel("—");
		val->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);   // values hug their entry (left-aligned), no floating center
		g->addWidget(val, row, 1);
		auto* u = new QLabel(unit);
		u->setStyleSheet("color:#666;");
		if (unitSync) m_dUnitLbl.append(u);
		if (unitSink) unitSink->append(u);
		g->addWidget(u, row, 2);
		return cap;
	};
	QVector<QGridLayout*> measGrids;   // collect the three sub-group grids so the value-column calibration adjusts col0 width for all at once
	auto alignMeasGrid = [&](QGridLayout* g) {
		g->setColumnMinimumWidth(0, 140);
		g->setColumnMinimumWidth(1, 72);   // fixed value-column width fixes the unit-column start: every row of all three
		                                   // sub-groups shares one column position = vertically aligned units, with a small
		                                   // constant gap between value and unit (longest readout ~55px)
		g->setColumnStretch(3, 1);         // trailing empty column absorbs leftover width
		measGrids.append(g);
	};
	auto* gPowBox = new QGroupBox(L("功率", "Power"));
	auto* powGrid = new QGridLayout(gPowBox);
	addRow(powGrid, 0, L("功率", "Power"), m_rdPower, "ADU", false, &m_powUnitLbl);
	addRow(powGrid, 1, L("峰值强度", "Peak Intensity"), m_rdPkInt, "ADU", false, &m_powUnitLbl);   // peak-pixel energy (unit follows the power unit)
	// peak saturation moved in from the position sub-group; the unit-background row was removed (redefined in the acquisition page's background-subtraction group)
	addRow(powGrid, 2, L("峰值饱和", "Peak Saturation"), m_rdSat, "%FS", false);
	alignMeasGrid(powGrid);
	measLay->addWidget(gPowBox);
	auto* gDiaBox = new QGroupBox(L("直径", "Diameter"));
	auto* diaGrid = new QGridLayout(gDiaBox);
	// Names use the plain "Diameter X / Diameter Y / Effective Diameter" wording; the formula sqrt(dx*dy) lives in the
	// Effective Diameter hover tooltip (QToolTip rich text = Qt's own rendering, zero system dependency, subscript size
	// readable in a short note); the CSV header d_x,d_y lines stay plain ASCII (data-file convention)
	addRow(diaGrid, 0, L("直径 X", "Diameter X"), m_rdDox, "px", true);
	addRow(diaGrid, 1, L("直径 Y", "Diameter Y"), m_rdDoy, "px", true);
	addRow(diaGrid, 2, L("有效直径", "Effective Diameter"), m_rdEff, "px", true)
		->setToolTip(L("有效直径 = √(d<sub>x</sub>·d<sub>y</sub>)",
		               "Effective diameter = √(d<sub>x</sub>·d<sub>y</sub>)"));
	addRow(diaGrid, 3, L("椭圆度", "Ellipticity"), m_rdEllip, "", false);
	addRow(diaGrid, 4, L("方位角", "Orientation"), m_rdOrient, "°", false);
	alignMeasGrid(diaGrid);
	measLay->addWidget(gDiaBox);
	auto* gPosBox = new QGroupBox(L("位置", "Position"));
	gPosBox->setToolTip(L("坐标口径：靶面左下角原点、y 向上（全软件统一）",
	                      "Coordinates: sensor bottom-left origin, y up (unified across the app)"));
	auto* posGrid = new QGridLayout(gPosBox);
	addRow(posGrid, 0, L("质心 X", "Centroid X"), m_rdCx, "px", true);   // position-row units follow the size unit
	addRow(posGrid, 1, L("质心 Y", "Centroid Y"), m_rdCy, "px", true);
	addRow(posGrid, 2, L("峰值 X", "Peak X"), m_rdPkX, "px", true);
	addRow(posGrid, 3, L("峰值 Y", "Peak Y"), m_rdPkY, "px", true);   // peak saturation / floor rows migrated to the power sub-group (see powGrid)
	alignMeasGrid(posGrid);
	measLay->addWidget(gPosBox);
	homeLay->addWidget(gMeas);
	homeLay->addStretch();
	m_tabs->addTab(scroll(home), L("主页", "Home"));

	// ── Setup: pseudocolor range / ROI / calculation threshold / display / units & photometric calibration (the floor feature moved to the acquisition page's background-power subtraction) ──
	// Pseudocolor default = fixed 2^12 redistribution [-100, 4096]; the auto tier (0.5%~99.5% percentiles) remains selectable
	auto* setup = new QWidget;
	auto* setupLay = new QVBoxLayout(setup);
	auto gPc = new QGroupBox(L("伪彩量程", "Pseudo-Color Range"));
	auto* pcLay = new QVBoxLayout(gPc);
	m_pcAuto = new QCheckBox(L("自动量程", "Auto Range"));
	m_pcAuto->setChecked(false);
	pcLay->addWidget(m_pcAuto);
	auto* loRow = new QHBoxLayout;
	loRow->addWidget(new QLabel(L("最小", "Min")));
	m_loSl = new QSlider(Qt::Horizontal);
	m_loSl->setRange(-100, 4096);
	m_loSl->setValue(-100);
	m_loSl->setSingleStep(16);
	loRow->addWidget(m_loSl, 9);   // stretch 9:1 ~ 90% of the row width
	m_loLbl = new QLabel("-100.0");
	loRow->addWidget(m_loLbl);
	loRow->addStretch(1);
	pcLay->addLayout(loRow);
	auto* hiRow = new QHBoxLayout;
	hiRow->addWidget(new QLabel(L("最大", "Max")));
	m_hiSl = new QSlider(Qt::Horizontal);
	m_hiSl->setRange(-100, 4096);
	m_hiSl->setValue(4096);
	m_hiSl->setSingleStep(16);
	hiRow->addWidget(m_hiSl, 9);   // same 90% as the min row
	m_hiLbl = new QLabel("4096.0");
	hiRow->addWidget(m_hiLbl);
	hiRow->addStretch(1);
	pcLay->addLayout(hiRow);
	setupLay->addWidget(gPc);

	// ROI group (lives in Setup; wiring stays at the end of buildPanel)
	auto gRoi = new QGroupBox("ROI");
	auto* roiLay = new QHBoxLayout(gRoi);
	auto bRoi = new QPushButton(L("框选 ROI", "Draw ROI"));
	auto bRoiClr = new QPushButton(L("清除", "Clear"));
	m_roiLabel = new QLabel(L("全帧", "Full Frame"));
	roiLay->addWidget(bRoi);
	roiLay->addWidget(bRoiClr);
	roiLay->addWidget(m_roiLabel, 1);
	setupLay->addWidget(gRoi);

	// The floor feature moved wholesale to the acquisition page's background-power subtraction (custom unit background + unit-background row);
	// this group keeps only the threshold row, renamed Calculation Threshold. The analyzer applies the threshold to the
	// floor-subtracted peak (subtract the floor first, then split at post-subtraction peak x threshold%).
	auto gBg = new QGroupBox(L("计算阈值", "Calculation Threshold"));   // English labels avoid & (a Qt mnemonic prefix would swallow the character), same below
	auto* bgLay = new QGridLayout(gBg);
	auto* thrCap = captionL(L("阈值 (峰%)", "Threshold (peak%)"));   // kept as a handle = the alignment baseline for the display group's combo column width
	bgLay->addWidget(thrCap, 0, 0);
	m_thr = new QDoubleSpinBox;
	m_thr->setRange(0, 50);
	m_thr->setDecimals(1);
	m_thr->setValue(10.0);  // default 10%: measured diffuse stray light is a radial halo (median 1536, brighter near the beam); a 5% threshold lets the halo tail widen the beam ~15%, while 10% matches the profile-method ground truth (empirically calibrated)
	bgLay->addWidget(m_thr, 0, 1);
	bgLay->setColumnStretch(2, 1);   // the spin column no longer fills (fixed width = same as the exposure row, see the end of buildPanel); the trailing empty column absorbs the rest
	setupLay->addWidget(gBg);

	// Apply exposure/gain: parameter edits push automatically (the old build only honored the Apply button -- turning the spinbox without
	// clicking it changed nothing, one root cause of "the spot brightness doesn't change after adjusting"); the button remains = force resend
	auto pushCamParams = [this] {
		if (!m_active) return;
		// no status message here (redundant); effectiveness is judged by the frame source's worker-side behavior
		m_active->setExposureUs(m_exp->value() * 1000.0);   // UI exposure = ms, node/mailbox = us
		m_active->setGainDb(m_gain->value());
	};
	connect(m_exp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, pushCamParams);
	connect(m_gain, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, pushCamParams);

	// Display group: 3D resolution (three tiers) + shading (two tiers); the 84% contour toggle moved to Home Main Controls; the 3D transparency slider was dropped (surface stays fully opaque)
	auto gDisp = new QGroupBox(L("显示", "Display"));
	auto* dispLay = new QGridLayout(gDisp);
	m_3dQuality = new CenterCombo;
	m_3dQuality->addItems({ L("标准", "Standard"), L("高清", "High"), L("超清", "Ultra") });
	m_3dQuality->setCurrentIndex(1);
	m_3dQuality->setToolTip(L("3D 曲面网格密度：标准 96×76 / 高清 192×152 / 超清 320×256（逐级提高）",
	                          "3D surface mesh density: Standard 96×76 / High 192×152 / Ultra 320×256"));
	m_3dSmooth = new CenterCombo;
	m_3dSmooth->addItems({ L("快速", "Fast"), L("平滑", "Smooth") });
	m_3dSmooth->setCurrentIndex(1);   // default Smooth
	m_3dSmooth->setToolTip(L("快速=逐面元平涂(分级少省算力)；平滑=逐像素颜色插值(颜色丰富过渡自然)",
	                          "Fast = flat per-facet shading (fewer levels, cheaper); Smooth = per-pixel color interpolation"));
	dispLay->addWidget(captionL(L("3D 分辨率", "3D Resolution")), 0, 0);
	dispLay->addWidget(m_3dQuality, 0, 1);
	dispLay->addWidget(captionL(L("3D 着色", "3D Shading")), 1, 0);
	dispLay->addWidget(m_3dSmooth, 1, 1);
	// Fixed width = the natural width of the English entries (measured sizeHint of a throwaway combo with the same style/font):
	// the English mode equals its original natural width exactly, and the Chinese mode no longer shrinks to a two-character sliver
	auto comboENatW = [](const QStringList& items) {
		QComboBox p;
		for (const QString& s : items) p.addItem(s);
		return p.sizeHint().width();
	};
	m_3dQuality->setFixedWidth(comboENatW({ "Standard", "High", "Ultra" }));
	m_3dSmooth->setFixedWidth(comboENatW({ "Fast", "Smooth" }));
	dispLay->setColumnMinimumWidth(0, thrCap->sizeHint().width());   // name column width = the threshold row's name width; both grids share margins/columns, so the combo's left edge aligns with the threshold spin's
	dispLay->setColumnStretch(2, 1);   // trailing empty column absorbs leftover width = combos shrink to natural width hugging their names
	setupLay->addWidget(gDisp);

// Custom view axis bounds (Custom unchecked = auto default, checked = manual bounds; values in the current display unit).
// Column headers Lower/Upper above the two value columns; spinbox suffixes follow the global config (profile Y = power unit tier, size/track = size unit).
	auto gAx = new QGroupBox(L("视图坐标轴", "View Axes"));
	auto* axLay = new QGridLayout(gAx);
	auto* axLoHdr = new QLabel(L("下限", "Lower"));
	auto* axHiHdr = new QLabel(L("上限", "Upper"));
	axLoHdr->setStyleSheet("color:#666;");
	axHiHdr->setStyleSheet("color:#666;");
	axLay->addWidget(axLoHdr, 0, 2);
	axLay->addWidget(axHiHdr, 0, 3);
	auto axRow = [&](int row, const QString& name, QCheckBox*& chk, QDoubleSpinBox*& lo, QDoubleSpinBox*& hi,
	                 double loDef, double hiDef) {
		chk = new QCheckBox(L("自定义", "Custom"));
		lo = new QDoubleSpinBox;
		lo->setRange(-1e6, 1e6);   // actual axis-limit magnitudes stay <= tens of thousands; fewer digits shrink the spin's minimum width (one of the English Setup-page hscroll compressions)
		lo->setDecimals(1);
		lo->setValue(loDef);
		hi = new QDoubleSpinBox;
		hi->setRange(-1e6, 1e6);
		hi->setDecimals(1);
		hi->setValue(hiDef);
		lo->setEnabled(false);
		hi->setEnabled(false);
		lo->setFixedWidth(76);   // width verified by screenshot against the longest default readout "100000.0 ADU"; any shorter clips the suffix
		hi->setFixedWidth(76);
		axLay->addWidget(captionL(name), row, 0);
		axLay->addWidget(chk, row, 1);
		axLay->addWidget(lo, row, 2);
		axLay->addWidget(hi, row, 3);
	};
	// Row order = view page order (profile -> size -> track), titles match the view buttons; data rows start at 1 (0 = column headers)
	axRow(1, L("光斑剖面 Y", "Profile Y"), m_pfYCustom, m_pfYLo, m_pfYHi, 0, 100000);
	axRow(2, L("光斑尺寸 Y", "Beam Size Y"), m_szYCustom, m_szYLo, m_szYHi, 0, 1000);
	axRow(3, L("质心追踪 X", "Track X"), m_trXCustom, m_trXLo, m_trXHi, 0, 1280);
	axRow(4, L("质心追踪 Y", "Track Y"), m_trYCustom, m_trYLo, m_trYHi, 0, 1024);
	axLay->setColumnStretch(4, 1);   // stretch sits on the trailing empty column (the old col1 stretch pushed the spins to the right edge),
	                                  // so name/check/bounds stay hugged to the left as a group
	setupLay->addWidget(gAx);
	// Axis-group unit suffixes follow the config: first refresh at the end of buildPanel + refreshed in the unit-change wiring;
	// axis-limit spin decimals follow the unit (ADU/px = 1, um/mm/uW/mW/W = 3)
	auto refreshAxUnits = [this] {
		const QString pw = " " + m_powUnit->currentText();
		const QString sz = " " + sizeUnit();
		const int pd = m_powUnit->currentIndex() == 0 ? 1 : 3;
		const int sd = m_sizeUnit->currentIndex() == 0 ? 1 : 3;
		m_pfYLo->setSuffix(pw); m_pfYHi->setSuffix(pw);
		m_pfYLo->setDecimals(pd); m_pfYHi->setDecimals(pd);
		m_szYLo->setSuffix(sz); m_szYHi->setSuffix(sz);
		m_szYLo->setDecimals(sd); m_szYHi->setDecimals(sd);
		m_trXLo->setSuffix(sz); m_trXHi->setSuffix(sz);
		m_trXLo->setDecimals(sd); m_trXHi->setDecimals(sd);
		m_trYLo->setSuffix(sz); m_trYHi->setSuffix(sz);
		m_trYLo->setDecimals(sd); m_trYHi->setDecimals(sd);
	};

	// Units & photometric calibration: size px/um/mm converts linearly by pixel pitch; power = camera equation P = sumADU * (e-/ADU)/QE * (hc/lambda) / t_exp * coefficient.
	// QE and conversion gain are not published in the datasheet -- defaults are placeholders, readouts are indicative until calibrated.
	auto gCal = new QGroupBox(L("单位与光度标定", "Units and Photometric Calibration"));
	auto* calLay = new QGridLayout(gCal);   // two-column layout: with the fixed-width right panel, the old four-column grid (two labels + two spinboxes side by side) always exceeded the minimum width = the root cause of "content overflows the page"
	int calRow = 0;
	auto calPair = [&](const QString& name, QWidget* w) {
		calLay->addWidget(captionL(name), calRow, 0);
		calLay->addWidget(w, calRow, 1);
		++calRow;
	};
	m_pixSize = new QDoubleSpinBox;
	m_pixSize->setRange(0.01, 1000.0);
	m_pixSize->setDecimals(3);
	m_pixSize->setValue(4.8);
	m_pixSize->setSuffix(" µm/px");   // units go on the value suffix, not in the entry name
	m_pixSize->setToolTip(L("像元尺寸；MV-CU013-A0UM 官方规格书值 4.8 µm/px",
	                        "Pixel pitch; 4.8 µm/px per the MV-CU013-A0UM official datasheet"));
	calPair(L("像素尺寸", "Pixel Size"), m_pixSize);
	m_waveNm = new QDoubleSpinBox;
	m_waveNm->setRange(100.0, 20000.0);
	m_waveNm->setDecimals(1);
	m_waveNm->setValue(1064.0);
	m_waveNm->setSuffix(" nm");   // unit suffix
	m_waveNm->setToolTip(L("按你的激光实际波长填写（默认 1064 仅为常见 Nd:YAG 线）；内置 QE 曲线有效域 400~1100nm，域外按端点钳位",
	                        "Set to your laser's actual wavelength (1064 default is just the common Nd:YAG line); built-in QE curve valid 400–1100nm, clamped outside"));
	calPair(L("波长", "Wavelength"), m_waveNm);
	// (the power-unit row was removed here: its control lives in Home Main Controls, repeating it would be redundant)
	m_qePct = new QDoubleSpinBox;
	m_qePct->setRange(0.1, 100.0);
	m_qePct->setDecimals(1);
	m_qePct->setValue(builtinQePct(1064.0));   // built-in default = the official response curve value at the wavelength (~5% at 1064nm, log extrapolation beyond 940nm)
	m_qePct->setEnabled(false);                // with Custom off the built-in value is locked
	m_qePct->setSuffix(" %");                  // unit suffix
	m_qePct->setToolTip(L("该波长量子效率——内置值取自官方规格书响应曲线（识图数字化），\n940nm 后按曲线对数线性外推（硅带边截止，1064nm 属极弱寄生响应）；开「自定义」可手改",
	                        "Quantum efficiency at this wavelength — built-in value digitized from the official datasheet response curve,\nlog-linear extrapolation beyond 940nm (Si band-edge cutoff; 1064nm is a very weak parasitic response); enable Custom to override"));
	calPair("QE", m_qePct);
	m_ePerAdu = new QDoubleSpinBox;
	m_ePerAdu->setRange(0.001, 1000.0);
	m_ePerAdu->setDecimals(3);
	m_ePerAdu->setValue(1.0);
	m_ePerAdu->setEnabled(false);
	m_ePerAdu->setSuffix(" e⁻/ADU");   // unit suffix
	m_ePerAdu->setToolTip(L("0dB 增益下每 ADU 对应电子数——官方未公布转换增益，内置 1.0 占位；开「自定义」可手改；模拟增益 dB 自动按 10^(dB/20) 折算",
	                        "Electrons per ADU at 0dB gain — conversion gain not published by the vendor, built-in 1.0 placeholder; enable Custom to override; analog gain dB is folded in as 10^(dB/20)"));
	calPair(L("转换增益", "Conversion Gain"), m_ePerAdu);
	// Long checkbox parentheticals moved into tooltips (the tooltip already carries the full meaning, zero information loss):
	// the English "Custom (off = built-in defaults)" was counted into the value column's minimum width, inflating the Setup page's English peak; layout start unchanged
	m_calCustom = new QCheckBox(L("自定义", "Custom"));
	m_calCustom->setToolTip(L("关：QE 用官方响应曲线内置值、转换增益用 1.0 占位；开：两字段解锁可手改",
	                          "Off: QE from the built-in datasheet curve, gain = 1.0 placeholder; On: both fields unlocked for manual edit"));
	calPair(L("QE/增益", "QE / Gain"), m_calCustom);
	m_calCoef = new QDoubleSpinBox;
	m_calCoef->setRange(0.0001, 9999.0);
	m_calCoef->setDecimals(6);   // calibration coefficient keeps 6 decimals
	m_calCoef->setValue(1.0);
	m_calCoef->setToolTip(L("单点标定：功率计实测一次实际功率后微调本系数使读数一致",
	                        "Single-point calibration: measure real power once with a power meter, then fine-tune this coefficient so the reading matches"));
	calPair(L("标定系数", "Calibration Coefficient"), m_calCoef);
	calLay->setColumnStretch(2, 1);   // trailing empty column absorbs leftover width = parameter boxes shrink to natural width hugging their names
	// (the old orange calibration warning -- QE extrapolation / gain placeholder are indicative only -- was moved out of the UI into the manual)
	setupLay->addWidget(gCal);
	setupLay->addStretch();
	m_tabs->addTab(scroll(setup), L("设置", "Setup"));

	// ── Acquisition: acquisition params / sensor settings / background subtraction / time window / stats (the status-message group was removed; the connection badge is the status outlet) ──
	auto* acq = new QWidget;
	auto* acqLay = new QVBoxLayout(acq);
	// Acquisition params (vertical layout): exposure/gain/frame rate stacked, name column right-aligned, value column vertically aligned,
	// the Apply button vertically centered in the right column (middle cell), live frame rate to the right of the frame-rate row (below Apply); set values send on Apply
	auto gCam = new QGroupBox(L("采集参数", "Acquisition Params"));
	auto* camLay = new QGridLayout(gCam);
	// Column alignment across the acquisition page's three sub-groups: name labels are all created and collected here,
	// and the tail welds col0 width to the widest natural width; left-aligned -- the widest col0 entry's right edge defines
	// the column width, its left edge = col0's left edge, and the shorter names left-align to the same position
	std::vector<QLabel*> col0Lbls;
	// Exposure/gain/frame-rate/sampling-duration names carry indent 23 = aligned with the sensor-range row's "X:/Y:/Origin:" prefix left edges
	// (those prefixes carry the same setIndent(23)); the sensor group's four names stay unindented at col0's left edge
	auto camLbl = [&](const char* zh, const char* en, int indent = 0) {
		auto* l = new QLabel(L(zh, en));
		l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		// Indent 23 is a Chinese-mode artifact (to start-align with the "X:" prefix); in English mode every name
		// (including the Origin:/X:/Y: note lines) hugs the left edge = one shared left edge is tidier, and the
		// indent once pushed the English col0W to 121 ("Unit Background" + 23), driving the sensor-group row to
		// 430px = the real culprit behind the panel width.
		if (indent && g_cn) l->setIndent(indent);
		col0Lbls.push_back(l);
		return l;
	};
	// Exposure uses the ms convention (UI shows ms for convenience; internal/camera-node/ini keys stay in us, the x1000
	// conversion happens only at the UI layer; ini migration in loadSettingsIni). Units go on the value suffix only, not
	// in the entry name (the old "Exposure(ms) 10.000 ms" double spelling was redundant).
	camLay->addWidget(camLbl("曝光", "Exposure", 23), 0, 0);
	m_exp = new QDoubleSpinBox;
	m_exp->setRange(0.009, 10000.0);
	m_exp->setValue(10.0);
	m_exp->setDecimals(3);
	m_exp->setSuffix(" ms");
	camLay->addWidget(m_exp, 0, 1);
	camLay->addWidget(camLbl("增益", "Gain", 23), 1, 0);
	m_gain = new QDoubleSpinBox;
	m_gain->setRange(0.0, 24.0);
	m_gain->setDecimals(3);          // gain with 3 decimals
	m_gain->setSingleStep(0.1);      // uniform 0.1 step
	m_gain->setSuffix(" dB");
	camLay->addWidget(m_gain, 1, 1);
	camLay->addWidget(camLbl("帧率", "Frame rate", 23), 2, 0);
	m_fps = new QDoubleSpinBox;
	m_fps->setRange(1.0, 240.0);
	m_fps->setDecimals(1);
	m_fps->setSingleStep(1.0);
	m_fps->setSuffix(" Hz");
	m_fps->setValue(10.0);   // default 10Hz: per-frame GUI-thread analysis/repaint is the main stutter source; a lower rate eases the per-frame pressure so drags and other events feel smoother
	camLay->addWidget(m_fps, 2, 1);
	auto applyBtn = new QPushButton(L("应用", "Apply"));
	applyBtn->setToolTip(L("下发曝光/增益/帧率到帧源", "Send exposure/gain/frame rate to the frame source"));
	// Apply button in col3 = the same column as the sensor group's Apply/Track buttons (col2 = a same-width empty placeholder matching the hint column)
	camLay->addWidget(applyBtn, 1, 3);   // middle row of the right column = vertically centered
	m_fpsLive = new QLabel(L("实时 — Hz", "Live — Hz"));
	m_fpsLive->setStyleSheet("color:#0b57a4;");
	// Live frame rate in col2 (the hint column), left-aligned = same left edge as the "Max 1280 px" hint above;
	// the welded column width must reserve room for the widest readout (QFontMetrics on the longest English string
	// plus margin, fpsW merged into hintW at the tail), so EMA refreshes that widen the text can't shove the button column askew
	QFontMetrics fpsFm(m_fpsLive->font());
	const int fpsW = fpsFm.horizontalAdvance("Live 240.0 Hz") + 6;
	camLay->addWidget(m_fpsLive, 2, 2, Qt::AlignLeft);   // frame-rate row, hint column
	camLay->setColumnStretch(4, 1);
	acqLay->addWidget(gCam);

	// Sensor settings: cut data volume at the acquisition stage to raise the frame rate (lower sensor resolution = higher rate) --
	// the camera side writes GenICam Width/Height/OffsetX/OffsetY (stop -> write -> restart while streaming), the simulated source
	// just generates the window region. Center coordinates use the sensor bottom-left origin, y up (same convention as position readouts).
	// Layout: the max hint hugs right after the size spin (col2); the four spins share one vertically aligned column; the right
	// column holds two buttons (Apply sensor / Track centroid); the note keeps only two lines = sensor range (unit follows the
	// system size unit) + origin note.
	auto gSensor = new QGroupBox(L("靶面设置", "Sensor Settings"));   // the gRoi/roiLay identifiers are taken by the Setup page's analysis-ROI box
	auto* sensorLay = new QGridLayout(gSensor);
	const int expW = m_exp->sizeHint().width();   // sensor/floor parameter boxes fixed to the exposure box's width (the old col1 stretch filled them too long)
	int hintW = 0;   // natural width of the max-hint column (the acquisition-params group's same-column empty placeholder; column width welded at the tail)
	sensorLay->addWidget(camLbl("X 维采集尺寸", "X size"), 0, 0);   // a space between the letter and the Chinese text = app-wide writing convention
	m_roiW = new QSpinBox;
	m_roiW->setRange(4, kSensorW);
	m_roiW->setSingleStep(1);   // 1px step (multiple-of-4 alignment is still backstopped by the frame source's &~3)
	m_roiW->setValue(kSensorW);
	m_roiW->setSuffix(" px");   // unit on the value (the spin is always px = the camera-native acquisition convention; converted units show only in the sensor-range line)
	m_roiW->setToolTip(L("上限=靶面宽度 1280 px；须为 4 的倍数（相机节点对齐要求）",
	                     "Max = sensor width 1280 px; must be a multiple of 4 (camera node alignment)"));
	sensorLay->addWidget(m_roiW, 0, 1);
	{ auto* hint = new QLabel(L("上限 %1 px", "Max %1").arg(kSensorW));   // max hint hugs right after the parameter box (English kept short to prevent overflow)
	  hint->setStyleSheet("color:#666;"); sensorLay->addWidget(hint, 0, 2);
	  hintW = std::max(hintW, hint->sizeHint().width()); }
	sensorLay->addWidget(camLbl("Y 维采集尺寸", "Y size"), 1, 0);
	m_roiH = new QSpinBox;
	m_roiH->setRange(4, kSensorH);
	m_roiH->setSingleStep(1);   // 1px step
	m_roiH->setValue(kSensorH);
	m_roiH->setSuffix(" px");   // unit on the value
	m_roiH->setToolTip(L("上限=靶面高度 1024 px；须为 4 的倍数（相机节点对齐要求）",
	                     "Max = sensor height 1024 px; must be a multiple of 4 (camera node alignment)"));
	sensorLay->addWidget(m_roiH, 1, 1);
	{ auto* hint = new QLabel(L("上限 %1 px", "Max %1").arg(kSensorH));
	  hint->setStyleSheet("color:#666;"); sensorLay->addWidget(hint, 1, 2);
	  hintW = std::max(hintW, hint->sizeHint().width()); }
	sensorLay->addWidget(camLbl("X 维窗口中心", "X center"), 2, 0);
	m_roiXc = new QSpinBox;
	m_roiXc->setValue(kSensorW / 2);
	m_roiXc->setSuffix(" px");   // unit on the value
	m_roiXc->setToolTip(L("范围随采集尺寸联动（见下方靶面范围行）；坐标原点=靶面左下角",
	                      "Range follows window size (see sensor range line below); origin = sensor bottom-left"));
	sensorLay->addWidget(m_roiXc, 2, 1);
	sensorLay->addWidget(camLbl("Y 维窗口中心", "Y center"), 3, 0);
	m_roiYc = new QSpinBox;
	m_roiYc->setValue(kSensorH / 2);
	m_roiYc->setSuffix(" px");   // unit on the value
	m_roiYc->setToolTip(L("范围随采集尺寸联动（见下方靶面范围行）；坐标原点=靶面左下角、y 向上",
	                      "Range follows window size (see sensor range line below); origin = sensor bottom-left, y up"));
	sensorLay->addWidget(m_roiYc, 3, 1);
	m_roiBtn = new QPushButton(L("应用靶面", "Apply ROI"));
	m_roiBtn->setToolTip(L("下发靶面窗口尺寸与中心到帧源（相机将短暂停流重配）",
	                       "Send the sensor window size/center to the frame source (the camera briefly stops the stream to reconfigure)"));
	// Four buttons in the right column, one per row, aligned with the X size -> Y center rows (no horizontal movement)
	sensorLay->addWidget(m_roiBtn, 0, 3);
	// Sensor quick presets -- set the size (centers unchanged; out-of-range values auto-clamp via the center spin's range)
	// and apply automatically (same path as Track centroid); same column = same width and vertical alignment as Apply ROI
	auto addRoiPreset = [&](QPushButton*& slot, const char* txt, int side, int row) {
		slot = new QPushButton(QString::fromUtf8(txt));
		slot->setToolTip(L(QString::fromUtf8("靶面窗口快速设为 %1×%1 px（中心不动）并自动应用").arg(side).toUtf8().constData(),
		                   QString("Quick-set the sensor window to %1×%1 px (center unchanged) and apply automatically").arg(side).toUtf8().constData()));
		sensorLay->addWidget(slot, row, 3);
		connect(slot, &QPushButton::clicked, this, [this, side] {
			m_roiW->setValue(side);
			m_roiH->setValue(side);
			applyRoi();
		});
	};
	addRoiPreset(m_roiP500, "500×500", 500, 1);   // the member doubles as the selftest probe's programmatic click port
	addRoiPreset(m_roiP200, "200×200", 200, 2);
	m_roiTrack = new QPushButton(L("追踪质心", "Track"));
	m_roiTrack->setToolTip(L("把当前光斑质心填入窗口中心并自动应用靶面（只取按下时刻的快照，不实时跟随）",
	                         "Copy the current beam centroid into the window centers and apply automatically (snapshot at press time, not live tracking)"));   // auto-applies
	sensorLay->addWidget(m_roiTrack, 3, 3);   // aligned with the Y center row
	// Sensor range / origin notes: a stats-bar-style two-column grid ("prefix: value"), three rows, value column
	// aligned with the stats mean column; row order Origin -> X -> Y; col0 welded to col0W with horizontal spacing 4
	// so the value column's left edge sits exactly at the X-size parameter box's left edge (col0W welding below the stats group)
	auto* noteBox = new QWidget;
	auto* noteLay = new QGridLayout(noteBox);
	noteLay->setContentsMargins(0, 0, 0, 0);
	noteLay->setHorizontalSpacing(4);
	noteLay->setVerticalSpacing(2);
	noteLay->setColumnMinimumWidth(0, 84);
	auto noteRow = [&](int row, const QString& name, QLabel*& val) {
		auto* n = new QLabel(name);
		val = new QLabel("—");
		n->setIndent(g_cn ? 23 : 0);   // Chinese mode shifts the prefix right to roughly the "Y center" name's start (value column unmoved, col0 stays 84 to keep stats alignment);
		                               // English mode: Origin:/X:/Y: share the left edge with names like "X size" = no indent (value column unmoved)
		n->setStyleSheet("color:#666;");
		val->setStyleSheet("color:#666;");
		noteLay->addWidget(n, row, 0);
		noteLay->addWidget(val, row, 1);
	};
	noteLay->setColumnStretch(2, 1);   // trailing empty column absorbs leftover width -- otherwise it spreads into col0 and shoves the value column right (measured +81px off)
	noteRow(0, L("原点:", "Origin:"), m_roiNoteOrg);   // Origin first, X/Y below it
	noteRow(1, L("X 维:", "X:"), m_roiNoteX);   // a space in the Chinese text = app-wide writing convention
	noteRow(2, L("Y 维:", "Y:"), m_roiNoteY);
	noteBox->setStyleSheet("color:#666;");
	sensorLay->addWidget(noteBox, 4, 0, 1, 5);   // with the four buttons one-per-row, the note row returns below the four rows
	for (QSpinBox* s : { m_roiW, m_roiH, m_roiXc, m_roiYc }) s->setFixedWidth(expW);   // same width as the exposure box
	sensorLay->setColumnStretch(4, 1);   // parameter/hint/button columns hug left together; the trailing empty column absorbs the rest
	acqLay->addWidget(gSensor);
	syncRoiLimits();
	updateRoiNote(kSensorW, kSensorH, 0, 0, kSensorW, kSensorH);   // initial note = full sensor; follows actual geometry after the first frame

	// Background power subtraction, dual-mode: with a shutter = automatic (close it, sample the stray-light residual, reopen;
	// reopens unconditionally when done); without a shutter = manual (the user blocks the spot, then clicks sample; occlusion
	// detection peak < 2048 ADU gates the button). The button text carries no parenthetical procedure notes -- the flow and
	// occlusion state are all expressed through the state hint label after the button.
	auto gBkg = new QGroupBox(L("背景功率扣除", "Background Power"));
	auto* bkgLay = new QVBoxLayout(gBkg);
	auto* bkgRow = new QHBoxLayout;   // row 1 = button + state hint (state sits after the button; squeezing the duration into one row overflowed the panel, proven by screenshots)
	m_bkgBtn = new QPushButton(L("采集背景功率", "Sample Background"));
	m_bkgBtn->setToolTip(L("快门在位：自动关快门采样遮光时的杂散光残余功率均值，完成后自动重开快门；快门不在位：请先手动遮挡光斑再采样。须与实光同曝光/增益",
	                        "With shutter connected: closes it, averages the stray-light residual, then reopens automatically. Without shutter: block the beam manually before sampling. Use the same exposure/gain as the real beam"));
	bkgRow->addWidget(m_bkgBtn);
	m_bkgState = new QLabel;   // the state hint refreshes per frame with shutter presence / occlusion detection (updateBkgState)
	m_bkgState->setWordWrap(true);
	bkgRow->addWidget(m_bkgState, 1);
	bkgLay->addLayout(bkgRow);
	// Sampling-duration row uses a grid -- name/box column positions align with the sensor group, box width = the X-size box
	auto* bkgGrid = new QGridLayout;   // row 0 = sampling duration; row 1 = custom unit-background checkbox; row 2 = unit background
	bkgGrid->setHorizontalSpacing(4);   // same as camLay/sensorLay after the unification loop (nested layouts fall outside that loop, so set it here)
	bkgGrid->addWidget(camLbl("采样时长", "Duration", 23), 0, 0);   // unit on the value suffix; indent 23 shares the left edge with exposure etc.
	m_bkgSec = new QDoubleSpinBox;                       // sampling duration user-settable, default 2s
	m_bkgSec->setRange(0.5, 60.0);
	m_bkgSec->setDecimals(1);            // 1 decimal
	m_bkgSec->setSingleStep(0.1);        // 0.1s step
	m_bkgSec->setSuffix(" s");
	m_bkgSec->setValue(2.0);
	m_bkgSec->setFixedWidth(expW);       // width benchmarked to the X-size parameter box
	bkgGrid->addWidget(m_bkgSec, 0, 1);
	// The floor feature migrated in from the Setup page's threshold group --
	// row 1 = custom unit-background checkbox (checked = use the manual value below, unchecked = per-frame pixel median auto-tracking),
	// row 2 = unit-background parameter row (name shares the sampling-duration left edge, box vertically aligned with it, both follow bkgGrid's unified column widths)
	m_bgCustom = new QCheckBox(L("自定义单元背景功率", "Custom Unit Background Power"));
	m_bgCustom->setToolTip(L("不勾=每帧取像素中位数作单元背景（实测杂散辉光分钟级漂移，默认自动）；勾选=用下方「单元背景」手填值",
	                         "Off = per-frame pixel median used as unit background (stray glow drifts by minutes; auto by default); On = manual value below"));
	bkgGrid->addWidget(m_bgCustom, 1, 0, 1, 2);
	m_bg = new QDoubleSpinBox;
	m_bg->setRange(0, 4095);   // pixel scale = camera-native 12-bit ADU
	m_bg->setDecimals(1);      // ADU setting fields use 1 decimal (app-wide)
	m_bg->setSuffix(" ADU");
	m_bg->setEnabled(false);   // auto tier by default (this value is used only when Custom is checked)
	bkgGrid->addWidget(camLbl("单元背景", "Unit Background", 23), 2, 0);
	bkgGrid->addWidget(m_bg, 2, 1);
	bkgGrid->setColumnStretch(2, 1);
	bkgLay->addLayout(bkgGrid);
	m_subBkg = new QCheckBox(L("功率读数扣除背景", "Subtract Background from Power"));
	m_subBkg->setEnabled(false);
	// Directly below the power-subtraction checkbox: when checked, raw data such as the 2D intensity image (display/CSV/RAW saves) has the effective floor deducted directly
	m_rawBgSub = new QCheckBox(L("原始强度数据扣除背景", "Subtract Background from Raw Intensity"));
	m_bkgLabel = new QLabel(L("背景功率: 未采集", "Background: not sampled"));
	m_bkgLabel->setStyleSheet("color:#666;");
	m_bkgLabel->setWordWrap(true);   // long status/conversion text wraps as a fallback (the English conversion value + failure hint once pushed the panel into a 21px hscroll)
	m_bkgLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // Qt6 wordWrap labels still report a full-line minimumSizeHint; Ignored keeps them from driving the page's minimum width
	// The row below background power = unit background (the key value recorded alongside the background capture; checking
	// "Custom Unit Background Power" shows the manual override instead of the auto statistic); a plain bkgLay label like
	// the background-power row = naturally left-aligned; the value shows the converted global power unit
	m_bkgUnitLabel = new QLabel(L("单元背景: 未采集", "Unit Background: not sampled"));
	m_bkgUnitLabel->setStyleSheet("color:#666;");
	m_bkgUnitLabel->setWordWrap(true);
	m_bkgUnitLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // same as above
	bkgLay->addWidget(m_subBkg);
	bkgLay->addWidget(m_rawBgSub);
	bkgLay->addWidget(m_bkgLabel);
	bkgLay->addWidget(m_bkgUnitLabel);
	// The two readout rows' parenthetical/global-unit conversions recompute instantly on power-unit tier changes (readout text is event-driven, so tier changes need an explicit refresh)
	connect(m_powUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { refreshBkgLabels(); });
	connect(m_bg, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { refreshBkgLabels(); });
	// (the unified column-width block moved to after the stats group is built, so stat prefixes count into col0W; see after gStat)
	acqLay->addWidget(gBkg);
	connect(m_bkgBtn, &QPushButton::clicked, this, [this] {
		if (!m_active || m_bkgBusy) return;
		if (m_acqBtn && !m_acqBtn->isChecked())
			setAcquiring(true);   // sampling depends on the frame stream; resume acquisition if it was manually stopped
		m_bkgBusy = true; m_bkgSum = 0; m_bkgUnitSum = 0; m_bkgCount = 0; m_bkgTicks = 0;
		const double sec = m_bkgSec->value();            // duration snapshot at click time; spinbox edits during sampling don't affect this round
		if (m_shutterConnected) m_shutter->command(false);   // presence criterion = probe/reply confirmation; the exe existing != the device being present
		m_bkgLabel->setText(L("背景功率: 采样中…（%1 s）", "Background: sampling… (%1 s)").arg(sec, 0, 'f', 1));
		updateBkgState();   // gray out the button during sampling
		auto* t = new QTimer(this);
		t->setInterval(100);
		connect(t, &QTimer::timeout, this, [this, t, sec] {
			++m_bkgTicks;
			// sample one frame per 100ms (every frame differs at 30-45fps); also record the effective floor (sampling period = blocked scene, its mean = the captured unit background)
			if (m_lastR.valid) { m_bkgSum += m_lastR.power; m_bkgUnitSum += m_lastR.backgroundUsed; ++m_bkgCount; }
			if (m_bkgTicks * 0.1 >= sec) {                                    // sample for the configured duration (user-settable)
				t->stop(); t->deleteLater();
				m_bkgBusy = false;
				if (m_shutterConnected) m_shutter->command(true);   // reopen unconditionally (see the gBkg note: root fix for state-tracking desync)
				updateBkgState();   // restore the button (recomputed from presence/occlusion state)
				if (m_bkgCount >= 3) {
					m_bkgAdu = m_bkgSum / m_bkgCount;
					m_bkgUnitAdu = m_bkgUnitSum / m_bkgCount;   // unit background = mean effective floor during sampling
					m_subBkg->setEnabled(true);
					m_subBkg->setChecked(true);
					m_bkgLblSampled = true; m_bkgLblFromIni = false;
					refreshBkgLabels();   // background-power row = ADU main value + parenthesized global unit; unit-background row = converted global unit
					updateReadouts(m_lastR);
				} else {
					m_bkgLblSampled = false;   // failure text is a placeholder, not overwritten by unit-tier changes (recomputed on the next successful sample/restore)
					m_bkgLabel->setText(L("背景功率: 采样失败（%1 s 内有效帧不足——相机未在取流？）",
					                      "Background: sampling failed (too few valid frames in %1 s — camera not streaming?)").arg(sec, 0, 'f', 1));
				}
			}
		});
		t->start();
	});

	// (the "camera status" group was removed wholesale -- redundant; the connection badge after CMOS is the single status outlet)

	auto gStat = new QGroupBox(L("统计（近100帧）", "Statistics (last 100 frames)"));   // the chart time window moved to Home Main Controls
	// min/max on its own line, vertically aligned under the mean on the previous line -- grid: col0 = prefix
	// (Power:/D4sigmax:/D4sigmay:), col1 = two value lines (mean·sigma / min·max); a uniform col1 start across
	// rows and groups achieves the alignment; after the split, long single lines no longer drive the page's minimum width.
	auto* stLay = new QGridLayout(gStat);
	auto statPair = [&](int row, QLabel*& pre, const QString& preTxt, QLabel*& v1, QLabel*& v2) {
		pre = new QLabel(preTxt);
		pre->setIndent(g_cn ? 23 : 0);   // Power/D4sigma prefixes share the left edge with "X:/Y:/Origin:" and the exposure names; English mode hugs the left edge (same as camLbl)
		v1 = new QLabel("—");
		v2 = new QLabel("");
		stLay->addWidget(pre, row, 0);
		stLay->addWidget(v1, row, 1);
		stLay->addWidget(v2, row + 1, 1);
		col0Lbls.push_back(pre);   // prefixes count into col0W (value column's left edge = X-size box's left edge = col0W+4; the stats group sits at the same position)
	};
	// Creation text = runtime text (updateReadouts sets "Power:/D4sigmax:/D4sigmay:" every frame, unit inside the value string) --
	// the old creation text "Power(ADU):" flashed for one frame yet col0W was welded to it, over-reserving 10px in English
	statPair(0, m_statPowLbl, L("功率:", "Power:"), m_statPower, m_statPower2);
	statPair(2, m_statWxLbl, "D4σx:", m_statWidthX, m_statWidthX2);
	statPair(4, m_statWyLbl, "D4σy:", m_statWidthY, m_statWidthY2);
	stLay->setColumnStretch(2, 1);   // without a trailing column the leftover width spread into col0 and pushed the means right;
	                                  // with the trailing empty column absorbing it, the means hug their prefixes and the cross-row alignment holds (col0 welded to col0W in the block below)
	// Unified column-width weld after the stats group is built: col0 = the widest natural width among all acquisition-page
	// name labels (including the indent-23 exposure/duration/stat-prefix ones), col1 = expW; all four groups (acquisition
	// params / sensor / background / stats) share group margins 12 and horizontal spacing 4, so the value column's left edge
	// = col0W+4 = the X-size parameter box's left edge.
	{
		int col0W = 0;
		for (QLabel* l : col0Lbls) col0W = std::max(col0W, l->sizeHint().width());
		const int col2W = std::max(hintW, fpsW);   // col2 weld = the wider of the max-hint and the widest live-frame-rate readout (both groups share the value = button columns stay co-located)
		for (QGridLayout* g : { camLay, sensorLay }) {
			g->setColumnMinimumWidth(0, col0W);
			g->setColumnMinimumWidth(2, col2W);
		}
		camLay->setColumnMinimumWidth(1, expW);
		bkgGrid->setColumnMinimumWidth(0, col0W);
		bkgGrid->setColumnMinimumWidth(1, expW);
		noteLay->setColumnMinimumWidth(0, col0W);
		stLay->setColumnMinimumWidth(0, col0W);
		m_col0WDbg = col0W;   // PANEL_DBG diagnostic (indent 23 inflates col0W, pushing the acquisition page's contentMin toward the viewport; for locating hscroll regressions)
		for (QDoubleSpinBox* s : { m_exp, m_gain, m_fps }) s->setFixedWidth(expW);   // all three boxes match the X-size box (a final weld at the tail backstops this)
	}
	acqLay->addWidget(gStat);
	acqLay->addStretch();
	m_tabs->addTab(scroll(acq), L("采集", "Acquisition"));

	// ── Wiring ──
	connect(applyBtn, &QPushButton::clicked, this, &MainWindow::applyCameraParams);
	connect(m_roiBtn, &QPushButton::clicked, this, &MainWindow::applyRoi);   // the sensor crop applies independently (camera side stops the stream to reconfigure)
	connect(m_roiW, &QSpinBox::valueChanged, this, [this] { syncRoiLimits(); });   // center ranges follow the size
	connect(m_roiH, &QSpinBox::valueChanged, this, [this] { syncRoiLimits(); });
	// the center spins no longer drive the note -- the sensor-range line reflects only the applied window (frame-geometry driven);
	// spin edits / track fills don't preview
	// Track centroid: at press time, snapshot the beam centroid into the two center spins (cx0/cy0 = full-sensor bottom-left-origin
	// coordinates, same convention as the center parameters; out-of-range values auto-clamp via the spin range), not live following,
	// then call applyRoi to send it.
	connect(m_roiTrack, &QPushButton::clicked, this, [this] {
		if (!m_lastR.valid) return;   // silent when there is no valid spot (the status-message group was removed)
		m_roiXc->setValue((int)std::lround(m_lastR.cx0));
		m_roiYc->setValue((int)std::lround(m_lastR.cy0));
		applyRoi();   // auto-apply (applyRoi reports the send result uniformly)
	});
	connect(bRoi, &QPushButton::clicked, this, &MainWindow::startRoiDraw);
	connect(bRoiClr, &QPushButton::clicked, m_view, [this] { m_view->clearRoi(); });
	connect(m_view, &SpotView::roiChanged, this, [this](const QRect& r) {
		m_roiLabel->setText(r.isNull() ? L("全帧", "Full Frame")
			: QString("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));
	});
	connect(m_bgCustom, &QCheckBox::toggled, m_bg, [this](bool on) { m_bg->setEnabled(on); refreshBkgLabels(); });   // checked = custom = the manual value takes effect; the unit-background row switches between the custom and auto-statistic value with the checkbox
	connect(m_pcAuto, &QCheckBox::toggled, this, [this](bool on) {
		m_loSl->setEnabled(!on);
		m_hiSl->setEnabled(!on);
		m_view->setAutoRange(on);
	});
	// pseudocolor range labels display ADU with a uniform 1 decimal
	connect(m_loSl, &QSlider::valueChanged, this, [this](int v) {
		m_loLbl->setText(QString::number(v, 'f', 1));
		if (v >= m_hiSl->value()) m_hiSl->setValue(std::min(4096, v + 1));
		m_view->setRange(v, m_hiSl->value());
	});
	connect(m_hiSl, &QSlider::valueChanged, this, [this](int v) {
		m_hiLbl->setText(QString::number(v, 'f', 1));
		if (v <= m_loSl->value()) m_loSl->setValue(std::max(-100, v - 1));
		m_view->setRange(m_loSl->value(), v);
	});
	// Effective-range report: sync 3D height normalization; in auto mode the sliders follow in real time (blockers prevent a feedback loop)
	connect(m_view, &SpotView::displayRangeChanged, this, [this](int lo, int hi) {
		m_effLo = lo; m_effHi = hi;
		m_3d->setRange(lo, hi);
		if (!m_pcAuto->isChecked()) return;
		QSignalBlocker b1(m_loSl), b2(m_hiSl);
		m_loSl->setValue(lo);
		m_hiSl->setValue(hi);
		m_loLbl->setText(QString::number(lo, 'f', 1));
		m_hiLbl->setText(QString::number(hi, 'f', 1));
		m_view->setRange(lo, hi);
	});
	// Bug fix from a full read-through: in replay mode, toggling these three display switches used to write live parameters
	// (m_l84*/m_lastR) directly, drawing the live frame's crosshair/contour onto the frozen replayed frame -- replay mode now
	// always goes through showDisplay() (redraw from the replayed frame + new toggle state); live mode keeps the lightweight
	// direct write (setCentroid/setBeam84 repaint only the overlay, avoiding a full setFrame recolor).
	connect(m_show84, &QCheckBox::toggled, this, [this](bool on) {
		if (m_rbSel >= 0) { showDisplay(); return; }
		m_view->setBeam84(m_l84cx, m_l84cy, m_l84wx, m_l84wy, m_l84rot, m_shape84->currentIndex(), on);
	});
	connect(m_shape84, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
		if (m_rbSel >= 0) { showDisplay(); return; }
		m_view->setBeam84(m_l84cx, m_l84cy, m_l84wx, m_l84wy, m_l84rot, m_shape84->currentIndex(),
		                  m_show84->isChecked());
	});
	connect(m_showCross, &QCheckBox::toggled, this, [this](bool on) {
		if (m_rbSel >= 0) { showDisplay(); return; }
		m_view->setCentroid(m_lastR.cx, m_lastR.cy, on && m_lastR.valid);
	});
	// The three rotation modes are mutually exclusive: custom angle = standalone fixed-angle mode (checking it auto-clears the others),
	// realtime = auto-leveling exclusive, one-shot = checkable button (press again cancels); activating any one exits the other two.
	connect(m_rotCustom, &QCheckBox::toggled, this, [this](bool on) {
		m_rotDeg->setEnabled(on);
		if (on) {
			exitOnceMode();
			if (m_rotSpot->isChecked()) { QSignalBlocker blk(m_rotSpot); m_rotSpot->setChecked(false); }
		}
		if (m_rotKeepSize->isChecked()) applyRoi();   // entering/leaving custom mode changes the effective angle -> resend the acquisition size needed to maintain the size
	});
	connect(m_rotSpot, &QCheckBox::toggled, this, [this](bool on) {
		if (on) {
			exitOnceMode();
			if (m_rotCustom->isChecked()) {
				QSignalBlocker blk(m_rotCustom);
				m_rotCustom->setChecked(false);
				m_rotDeg->setEnabled(false);   // under the blocker toggled doesn't fire, so sync setEnabled manually
			}
		}
		if (m_rotKeepSize->isChecked()) applyRoi();   // entering/leaving realtime rotation changes the effective angle -> resend the needed acquisition size
	});
	connect(m_rotOnce, &QPushButton::toggled, this, [this](bool on) {
		if (on) {
			double base = 0.0;
			bool detect = true;
			if (m_rotCustom->isChecked()) {          // custom angle = absolute fixed angle: enter one-shot relative to it
				base = m_rotDeg->value();
				detect = false;
			} else if (m_rotSpot->isChecked()) {     // realtime auto mode active: freeze the current effective angle (the display frame is already leveled, no re-measure)
				base = m_rotAngleValid ? m_rotAngle : 0.0;
				detect = false;
			}
			double res = 0.0;
			if (detect && m_lastRaw.valid()) {
				AnalysisResult r0 = analyzeBeam(m_lastRaw, m_view->roi(), bgValue(), 50.0);   // same convention as realtime auto-leveling (50% peak gate); uses the effective raw frame (m_last may already have the floor deducted, preventing double subtraction)
				if (r0.valid) {
					res = 90.0 - r0.orientationDeg;
					if (res > 90.0) res -= 180.0;
					if (res <= -90.0) res += 180.0;
				}
			}
			const double a = base + res;
			if (a > 90.0) res -= 180.0;
			else if (a <= -90.0) res += 180.0;
			m_rotOnceAngle = base + res;
			// entering one-shot = exiting realtime and custom (blockers keep their toggled wiring from clearing the about-to-be-set one-shot flag / resending twice)
			if (m_rotSpot->isChecked()) { QSignalBlocker blk(m_rotSpot); m_rotSpot->setChecked(false); }
			if (m_rotCustom->isChecked()) {
				QSignalBlocker blk(m_rotCustom);
				m_rotCustom->setChecked(false);
				m_rotDeg->setEnabled(false);
			}
		} else {
			m_rotOnceAngle = 0.0;
		}
		m_rotOnceActive = on;
		// The Stop acquisition button carries zero custom style = native checkable checked-state rendering; this button is also
		// checkable and holds the checked state while active, so the hand-painted light blue was dropped -- the native checked
		// state is naturally identical to Stop's (width locked by the weld at the end of buildPanel, unaffected by state
		// switches); the text still toggles.
		m_rotOnce->setText(on ? L("取消光斑旋转", "Cancel Spot Rotation") : L("单次光斑旋转", "Rotate Spot Once"));
		if (m_rotKeepSize->isChecked()) applyRoi();   // the one-shot fixed angle changed -> resend the acquisition size needed to maintain the size
	});
	// Maintain sensor size after rotation: checking/unchecking = switching between enlarged acquisition and configured size, sent immediately;
	// custom-angle tweaks change the needed size -> debounce 300ms then compare before sending (QDoubleSpinBox fires valueChanged in bursts; no repeated stream stops while dragging)
	connect(m_rotKeepSize, &QCheckBox::toggled, this, [this](bool) { applyRoi(); });
	connect(m_rotDeg, &QDoubleSpinBox::valueChanged, this, [this](double) {
		if (m_rotKeepSize->isChecked() && m_rotCustom->isChecked())
			QTimer::singleShot(300, this, [this] { rotKeepCheck(); });
	});
	connect(m_winCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
		static const int w[] = {60, 300, 600, 1800, 3600};
		m_winSec = w[i];
		rebuildSizeChart();
	});
	// Size-unit / pixel-pitch changes -> rebuild charts and axes at the new scale + refresh readout rows immediately
	connect(m_sizeUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, refreshAxUnits] {
		rebuildSizeChart();
		m_track->setUnit(sizeScale(), sizeUnit());
		m_view->setUnit(sizeScale(), sizeUnit());   // 2D view axis ticks follow the global unit
		updateReadouts(m_lastR);
		refreshAxUnits();   // the axis group's custom-value suffixes follow the size unit
		// the sensor-range line stays px and no longer follows the unit (the old updateRoiNote re-render call was removed with that decision)
	});
	connect(m_pixSize, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
		rebuildSizeChart();
		updateReadouts(m_lastR);
		m_view->setUnit(sizeScale(), sizeUnit());   // pixel pitch changed -> um/mm scale changed, 2D axis ticks follow (the range line stays px, see updateRoiNote)
	});
	connect(m_diamDef, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateReadouts(m_lastR); });
	connect(m_powUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, refreshAxUnits] {
		updateReadouts(m_lastR);
		if (m_stack->currentIndex() == 2 && m_lastR.valid) updateProfileChart(m_lastR);   // profile Y-axis unit refreshes with the power unit immediately (page order = 2)
		refreshAxUnits();   // the profile-Y custom-value suffixes follow the power unit tier
	});
	// QE/conversion-gain custom gate: off = built-in defaults (QE from the official curve at the wavelength, gain = 1.0 placeholder), on = manually editable
	connect(m_calCustom, &QCheckBox::toggled, this, [this](bool on) {
		m_qePct->setEnabled(on);
		m_ePerAdu->setEnabled(on);
		if (!on) {
			m_qePct->setValue(builtinQePct(m_waveNm->value()));
			m_ePerAdu->setValue(1.0);
		}
		updateReadouts(m_lastR);
	});
	connect(m_waveNm, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double nm) {
		if (!m_calCustom->isChecked()) {
			m_qePct->setValue(builtinQePct(nm));   // built-in QE follows the wavelength (official response curve interpolation/extrapolation)
			updateReadouts(m_lastR);
		}
	});
	// 3D resolution / shading tiers
	connect(m_3dQuality, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
		m_3d->setQuality(i);
	});
	connect(m_3dSmooth, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
		m_3d->setSmooth(i == 1);
	});
	// Custom view axis bounds: checked = fixed range (values in the current display unit), unchecked = back to auto
	auto pushTrackRange = [this] {
		m_track->setCustomRange(m_trXCustom->isChecked(), m_trXLo->value(), m_trXHi->value(),
		                        m_trYCustom->isChecked(), m_trYLo->value(), m_trYHi->value());
	};
	connect(m_szYCustom, &QCheckBox::toggled, this, [this](bool on) {
		m_szYLo->setEnabled(on); m_szYHi->setEnabled(on);
		rebuildSizeChart();
	});
	connect(m_szYLo, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { rebuildSizeChart(); });
	connect(m_szYHi, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { rebuildSizeChart(); });
	connect(m_pfYCustom, &QCheckBox::toggled, this, [this](bool on) {
		m_pfYLo->setEnabled(on); m_pfYHi->setEnabled(on);
		if (m_lastR.valid) updateProfileChart(m_lastR);
	});
	connect(m_pfYLo, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
		if (m_lastR.valid) updateProfileChart(m_lastR);
	});
	connect(m_pfYHi, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
		if (m_lastR.valid) updateProfileChart(m_lastR);
	});
	for (QCheckBox* c : { m_trXCustom, m_trYCustom })
		connect(c, &QCheckBox::toggled, this, [this, pushTrackRange](bool on) {
			m_trXLo->setEnabled(m_trXCustom->isChecked());
			m_trXHi->setEnabled(m_trXCustom->isChecked());
			m_trYLo->setEnabled(m_trYCustom->isChecked());
			m_trYHi->setEnabled(m_trYCustom->isChecked());
			pushTrackRange();
		});
	for (QDoubleSpinBox* s : { m_trXLo, m_trXHi, m_trYLo, m_trYHi })
		connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [pushTrackRange] { pushTrackRange(); });

	refreshAxUnits();   // first suffix refresh for the axis group (afterward only on power/size unit changes)

	// Unified group appearance: QGroupBox titles use the same blue as the ribbon captions (#0b57a4 bold); entries inside a
	// group indent right rather than aligning with the title; input controls (spin/combo) are width-capped instead of
	// filling, same rule for both languages. Width budget (measured with PANEL_DBG on the real platform): the English
	// Setup page peaked at 506 against a 485 viewport = a 21px hscroll; the combination of indent 12 + right margin 2 +
	// grid spacing 4 + axis spin range 1e6 (fewer digits = narrower spin minimum) pushed it back under 485.
	for (QGroupBox* g : w->findChildren<QGroupBox*>()) {
		g->setStyleSheet("QGroupBox::title { color:#0b57a4; font-weight:bold; }");
		if (QLayout* gl = g->layout()) {
			gl->setContentsMargins(12, gl->contentsMargins().top(), 2, gl->contentsMargins().bottom());
			if (QGridLayout* grl = qobject_cast<QGridLayout*>(gl)) grl->setHorizontalSpacing(4);
		}
	}
	// Cap 200 -> 120: the Home Main Controls combos no longer stretch to 200 (the main driver of the English long-label page peak);
	// no spin/combo on the panel has content wider than 120 (verified item by item in English)
	for (QSpinBox* s : w->findChildren<QSpinBox*>()) s->setMaximumWidth(120);
	for (QDoubleSpinBox* s : w->findChildren<QDoubleSpinBox*>()) s->setMaximumWidth(120);
	for (QComboBox* c : w->findChildren<QComboBox*>()) c->setMaximumWidth(120);
	// The measurement sub-groups indent one level deeper (sub-boxes read one step further in than groups);
	// the floor/threshold parameter boxes are fixed to the exposure box's width (m_exp is created in the acquisition-page section, so taking its sizeHint here is safe)
	for (QGridLayout* g : { powGrid, diaGrid, posGrid })
		g->setContentsMargins(24, g->contentsMargins().top(), 2, g->contentsMargins().bottom());
	m_bg->setFixedWidth(expW);
	m_thr->setFixedWidth(expW);
	// Root cause of the "acquisition parameter boxes must match the X-size box width" complaint, welded shut: the
	// global maxWidth(120) loop above lifts fixedWidth(expW)'s max back to 120, and QGridLayout then grows the
	// QDoubleSpinBoxes (exposure/gain/fps) to their natural 118 while the QSpinBoxes (X size etc.) stay at 102 —
	// a 16px gap between the two groups (real-platform pixel measurement: 118 vs 102). Welding fixedWidth AFTER
	// the loop (min=max=expW) makes the layout unable to stretch them apart (X-size box width stays put).
	for (QSpinBox* s : { m_roiW, m_roiH, m_roiXc, m_roiYc }) s->setFixedWidth(expW);
	for (QDoubleSpinBox* s : { m_exp, m_gain, m_fps, m_bkgSec }) s->setFixedWidth(expW);
	// "Calibration boxes shorter than the threshold box" (reported on Win11): the five calibration boxes had no
	// fixed width and were only clamped by the global maxWidth(120), while the threshold box uses
	// fixedWidth(expW) which grows with the font — the gap drifted with the environment (expW>120 on Win11 made a
	// 30px difference visible). Welding them onto the same expW chain makes them exactly as long as the threshold
	// box on any platform (same recipe as above: placed after the loop so the max clamp cannot re-apply).
	for (QDoubleSpinBox* s : { m_pixSize, m_waveNm, m_qePct, m_ePerAdu, m_calCoef }) s->setFixedWidth(expW);
	// Calibration group horizontal spacing 4→16 (value boxes shifted right so they don't crowd the names) — must
	// come after the unified-spacing loop or it gets overwritten back to 4; the stats group keeps spacing 4 so the
	// mean/min·max value column's left edge lines up with the X-size acquisition box's left edge (col0W+4).
	calLay->setHorizontalSpacing(16);
	// Align the Controls group entries with the Measures names (Power/Peak Intensity etc.): measurement entries
	// sit at x = group margin 12 + sub-group grid margin 24 = 36, so the Controls left margin is overridden to 36
	// (the unified loop gives every group 12).
	mainLay->setContentsMargins(36, mainLay->contentsMargins().top(), 2, mainLay->contentsMargins().bottom());
	// "The Power sub-box's right border is too close to the Measures parent frame" (Win11 screenshot): the unified
	// loop gives every group layout left 12 / right 2 (a width-saving design), and the three measurement sub-groups
	// ride the parent's margins — 12 left vs 2 right is asymmetric (visible when the screenshot is zoomed).
	// Overriding this group's right margin back to 12 makes the sub-box borders equidistant (Power/Diameter/Position
	// share one layout, so a single override fixes all three). Sub-group content minimum width ≈278 « available
	// ≈366, so there is no horizontal-scroll risk.
	measLay->setContentsMargins(12, measLay->contentsMargins().top(), 12, measLay->contentsMargins().bottom());
	// Shorten the one-shot spot-rotation button: left edge stays at the label column's left edge (spanning rows
	// start there), right edge aligns with the "enclosed contour shape" combo's right edge. That combo grows with
	// the field column, so button maxWidth = label column width + horizontal spacing + field column width; the
	// spacing is explicitly welded to 6 (QFormLayout's default follows the style, which would make the computed
	// basis unstable). The label column width is measured with QFontMetrics on the actual text — NOT sizeHint
	// (during buildPanel the style is not yet polished and its inflated metrics once pushed the button's right
	// edge ~80px past the combo's). Field column remaining width >120 (fixed panel width) → every field's real
	// width = maxWidth 120 (clamped by the width-cap loop).
	{
		int labW = 0, fieldW = 0;
		for (int r = 0; r < mainLay->rowCount(); ++r) {
			auto* li = mainLay->itemAt(r, QFormLayout::LabelRole);
			auto* fi = mainLay->itemAt(r, QFormLayout::FieldRole);
			if (auto* l = li ? qobject_cast<QLabel*>(li->widget()) : nullptr)
				labW = std::max(labW, QFontMetrics(l->font()).horizontalAdvance(l->text()));
			// Spanning rows (checkbox rows) report LabelRole=nullptr and FieldRole=the widget itself; they must be
			// explicitly excluded via SpanningRole — otherwise the checkbox's natural width of 270 leaks into
			// fieldW (Qt 6.11 behavior, confirmed by geometric diagnostics).
			if (fi && !mainLay->itemAt(r, QFormLayout::SpanningRole))
				if (QWidget* fq = fi->widget())
					fieldW = std::max(fieldW, fq->maximumWidth() < 16777215 ? fq->maximumWidth() : fq->sizeHint().width());
		}
		mainLay->setHorizontalSpacing(6);
		m_rotOnce->setMaximumWidth(labW + 6 + fieldW);
	}

	// Chinese mode only: align the measurement sub-boxes' value column left edge with a chosen character inside the
	// Controls checkbox label (after the diameter names were shortened, the 140px name column left too big a gap
	// between name and value). Calibrated by runtime measurement so it adapts to any machine's font/DPI; the
	// English layout is left untouched. singleShot(0) fires after the layout is finalized. The alignment target is
	// one Chinese character further along the label text than its first five glyphs (see the QStringLiteral below).
	if (g_cn) {
		QTimer::singleShot(0, this, [this, grids = measGrids] {
			QStyleOptionButton opt;
			opt.initFrom(m_rotKeepSize);
			const int textX = m_rotKeepSize->style()->subElementRect(QStyle::SE_CheckBoxContents, &opt, m_rotKeepSize).left();
			const int xTgt = m_rotKeepSize->mapToGlobal(
				QPoint(textX + m_rotKeepSize->fontMetrics().horizontalAdvance(QStringLiteral("旋转后维持靶")), 0)).x();
			const int w = qBound(60, 140 + xTgt - m_rdDox->mapToGlobal(QPoint(0, 0)).x(), 140);
			for (QGridLayout* g : grids)
				g->setColumnMinimumWidth(0, w);
		});
	}

	return w;
}

double MainWindow::sizeScale() const
{
	int u = m_sizeUnit->currentIndex();
	double pix = m_pixSize->value();
	return u == 1 ? pix : u == 2 ? pix / 1000.0 : 1.0;
}

QString MainWindow::sizeUnit() const
{
	static const char* u[] = {"px", "µm", "mm"};
	return u[m_sizeUnit->currentIndex()];
}

// Camera equation: P[W] = ΣADU · (e⁻/ADU)/QE · (hc/λ) / t_exp(seconds) · coefficient.
// ΣADU is the electron count accumulated over the exposure (an energy-like quantity); reaching power (a rate)
// REQUIRES dividing by the exposure time in seconds. A past regression removed /t_exp on the mistaken grounds
// that "ΣADU already includes the exposure time" — a dimensional error; the absurd readings back then actually
// came from dividing by the µs number (10000) instead of seconds (0.01), a 1e6x mistake. /t_exp is restored,
// preferring the node-readback exposure (falling back to the spinbox when readback is unavailable). Gain
// linearity cancels e⁻/ADU exactly, so the same optical power reads the same regardless of gain (physically
// self-consistent).
double MainWindow::powWPerAdu() const
{
	if (m_powUnit->currentIndex() == 0) return 0.0;   // ADU tier: no conversion
	double qe = m_qePct->value() / 100.0;
	double lamNm = m_waveNm->value();
	if (qe <= 0 || lamNm <= 0) return 0.0;
	double gainLin = std::pow(10.0, m_gain->value() / 20.0);
	double ePerAdu = m_ePerAdu->value() / gainLin;
	double tUs = m_active ? m_active->exposureUs() : -1.0;
	if (tUs <= 0) tUs = m_exp->value() * 1000.0;   // UI exposure is ms; convert the fallback value to µs
	const double hc = 1.98645e-25;   // J·m
	// /QE: ΣADU converts to collected electrons; incident photons = electrons / QE. An earlier version wrote /QE
	// in the comment but only used qe as a >0 guard without actually dividing, leaving the QE field inert —
	// caught while auditing the camera equation.
	return (ePerAdu / qe) * (hc / (lamNm * 1e-9)) * gainLin * m_calCoef->value() / (tUs * 1e-6);
}

// kPowScale/kPowUnit/powPickIdx live at the top of the file: the background-sampling row in buildPanel also needs
// tier selection, so they must be defined before it.

// Units must match the software-wide unit setting, but a nonzero value must never be truncated to 0 — if there is
// a value, it must be shown. Fixed format keeps at least baseDec decimals; nonzero values get extra decimals by
// magnitude so at least 3 significant digits are visible (0.000072 µW no longer displays as 0.000), with trailing
// zeros trimmed after widening; zero prints at exactly baseDec decimals.
static QString fmtNoTrunc(double v, int baseDec)
{
	int dec = baseDec;
	if (v != 0.0)
		dec = std::max(dec, 3 - (int)std::floor(std::log10(std::fabs(v))) - 1);
	QString s = QString::number(v, 'f', dec);
	if (dec > baseDec && s.contains('.')) {
		while (s.endsWith('0')) s.chop(1);
		if (s.endsWith('.')) s.chop(1);
	}
	return s;
}

// Returns the number only, no unit suffix — the Measures Power/Peak-Intensity rows have their own unit column
// (powUnitLabel, synced with tier selection); appending the unit after the number as well would duplicate it.
// Callers that need the unit take it from powUnitLabel().
QString MainWindow::fmtPower(double adu) const
{
	// Software-wide convention: ADU tier = 1 decimal, µW/mW/W tiers = 3 decimals (no magnitude-adaptive decimals).
	if (m_powUnit->currentIndex() == 0)
		return QString::number(adu, 'f', 1);
	double w = adu * powWPerAdu();
	int i = powPickIdx(w);
	double v = w / kPowScale[i];
	return QString("%1").arg(v, 0, 'f', 3);
}

// Effective power: subtraction checked and background sampled = power minus background (stray-light residual, clamped ≥0); otherwise the raw ΣADU.
double MainWindow::effPower(const AnalysisResult& r) const
{
	if (m_subBkg && m_subBkg->isChecked() && m_bkgAdu >= 0)
		return std::max(0.0, r.power - m_bkgAdu);
	return r.power;
}

// The unit tier an ADU value would get when displayed via fmtPower (strictly the same tier pick as inside fmtPower).
QString MainWindow::powUnitForAdu(double adu) const
{
	if (m_powUnit->currentIndex() == 0) return "ADU";
	return QString::fromUtf8(kPowUnit[powPickIdx(adu * powWPerAdu())]);
}

// Unit column of the Power row (the Peak-Intensity row uses powUnitForAdu(peak) instead — bug fix: the two rows
// used to share this function, so the peak value's tier and the unit column's tier could differ by 3 orders of
// magnitude when power and peak sit at different magnitudes).
QString MainWindow::powUnitLabel() const
{
	return powUnitForAdu(effPower(m_lastR));
}

// ADU → global power unit conversion text (tier by value magnitude, no truncation); ADU tier = raw value at 1 decimal.
QString MainWindow::convPowTxt(double adu) const
{
	if (m_powUnit->currentIndex() == 0)
		return QString::number(adu, 'f', 1);
	const double w = adu * powWPerAdu();
	const int i = powPickIdx(w);
	return fmtNoTrunc(w / kPowScale[i], 3);
}

// The Background and Unit-Background readouts are recomputed here in one place (triggered by sampling completion,
// ini restore, unit-tier or custom-value changes). Background row = ADU primary value + global unit in parentheses
// (the two numbers were one meaning, so the "sampled mean =" wording was dropped); the Unit-Background row uses
// the same format: ADU always shown, global unit in parentheses; checking "custom unit background power" shows
// the manually entered value instead of the auto-sampled statistic.
void MainWindow::refreshBkgLabels()
{
	if (m_bkgLabel && m_bkgLblSampled && !m_bkgBusy && m_bkgAdu >= 0) {
		const QString ini = m_bkgLblFromIni ? L("，上次采集", ", last sampled") : QString();
		if (m_powUnit->currentIndex() == 0)
			m_bkgLabel->setText(L("背景功率: %1 ADU", "Background: %1 ADU")
				.arg(m_bkgAdu, 0, 'f', 1) + (m_bkgLblFromIni ? L("（上次采集）", " (last sampled)") : QString()));
		else
			m_bkgLabel->setText(L("背景功率: %1 ADU（%2 %3%4）", "Background: %1 ADU (%2 %3%4)")
				.arg(m_bkgAdu, 0, 'f', 1).arg(convPowTxt(m_bkgAdu), powUnitForAdu(m_bkgAdu), ini));
	}
	if (m_bkgUnitLabel) {
		const double v = m_bgCustom && m_bgCustom->isChecked() ? m_bg->value() : m_bkgUnitAdu;
		if (v < 0)
			m_bkgUnitLabel->setText(L("单元背景: 未采集", "Unit Background: not sampled"));
		else if (m_powUnit->currentIndex() == 0)
			m_bkgUnitLabel->setText(L("单元背景: %1 ADU", "Unit Background: %1 ADU").arg(v, 0, 'f', 1));
		else   // same format as the Background row: ADU primary value always shown + global unit in parentheses (parentheses only for non-ADU tiers)
			m_bkgUnitLabel->setText(L("单元背景: %1 ADU（%2 %3）", "Unit Background: %1 ADU (%2 %3)")
				.arg(v, 0, 'f', 1).arg(convPowTxt(v), powUnitForAdu(v)));
	}
}

void MainWindow::rebuildSizeChart()
{
	QVector<QPointF> px_, py_;
	double s = sizeScale();
	px_.reserve((int)m_sizeHist.size());
	py_.reserve((int)m_sizeHist.size());
	for (const auto& a : m_sizeHist) {
		px_.append(QPointF(a[0] / 60.0, a[1] * s));
		py_.append(QPointF(a[0] / 60.0, a[2] * s));
	}
	// QtCharts was replaced by the self-drawn LineChartView (license-relicensing reason); titles/range/decimal
	// conventions all carried over — Y axis title follows the size unit, the view is uniformly named "Beam Size",
	// tick decimals follow the unit (px = 1, µm/mm = 3).
	m_sizeChart->setSeries({ { L("84%宽度X", "84% Width X"), QColor(0x1f, 0x77, 0xb4), px_ },
	                         { L("84%宽度Y", "84% Width Y"), QColor(0xd6, 0x60, 0x4d), py_ } });
	if (!m_sizeHist.empty()) {
		double tEnd = m_sizeHist.back()[0] / 60.0;
		m_sizeChart->setXRange(tEnd - m_winSec / 60.0, tEnd);
	}
	double sMaxY = 1;
	for (const QPointF& p : px_) sMaxY = std::max(sMaxY, p.y());
	for (const QPointF& p : py_) sMaxY = std::max(sMaxY, p.y());
	// Y upper bound: custom checked = fixed range, unchecked = automatic tight window
	if (m_szYCustom && m_szYCustom->isChecked() && m_szYHi->value() > m_szYLo->value())
		m_sizeChart->setYRange(m_szYLo->value(), m_szYHi->value());
	else
		m_sizeChart->setYRange(0, sMaxY * 1.1);
	m_sizeChart->setTitles(L("光斑尺寸（84% 能量宽度）", "Beam Size (84% Energy Width)"),
	                       L("分钟", "min"), sizeUnit());
	m_sizeChart->setDecimals(1, m_sizeUnit->currentIndex() == 0 ? 1 : 3);
}

void MainWindow::attachSource(CameraSource* src)
{
	printf("[win] attach %p (cur %p)\n", (void*)src, (void*)m_active); fflush(stdout);
	if (m_active) {
		disconnect(m_active, nullptr, this, nullptr);
		m_active->stop();
	}
	m_active = src;
	connect(src, &CameraSource::frameReady, this, &MainWindow::onFrame);
	connect(src, &CameraSource::statusText, this, [this](const QString& s) {
		// The in-UI status-message group was removed as redundant; source messages keep only a stdout mirror for
		// troubleshooting — under the GUI subsystem stderr does not land in redirected files (verified 0 bytes)
		// while stdout redirection works; fflush prevents buffered line loss.
		printf("[cam] %s\n", s.toUtf8().constData());
		fflush(stdout);
	});
	// Green connection badge after the CMOS label in the toolbar — driven by the effective connectionChanged
	// signal (StartGrabbing success = true, stream break / thread exit = false). Switching to the simulator means
	// the camera is genuinely disconnected = grey "Disconnected"; after switching to the camera it starts grey
	// "Connecting…".
	connect(src, &CameraSource::connectionChanged, this, [this](bool on) {
		m_camConn->setText(on ? L("已连接", "Connected") : L("未连接", "Disconnected"));
		m_camConn->setStyleSheet(on ? "color:#0a0;" : "color:#888;");
		// Grabbing ready = worker alive, only then does setRoi accept (it honestly fails otherwise) → re-issue the
		// required acquisition size for keep-size mode right after connection.
		if (on) rotKeepCheck();
	});
	m_camConn->setText(src == m_mvs ? L("连接中…", "Connecting…") : L("未连接", "Disconnected"));
	m_camConn->setStyleSheet("color:#888;");
	m_rb.clear();   // frame geometry and time base both change with the source: the replay buffer is void, return to live
	if (m_rbBtn && m_rbBtn->isChecked()) {   // switching sources exits replay mode (the blocker only resets the
		                                     // button and widget state; it deliberately does not run toggled's
		                                     // setAcquiring — the new source is started uniformly by the caller)
		QSignalBlocker b(m_rbBtn);
		m_rbBtn->setChecked(false);
		m_rbBtn->setText(L("切换回看", "Replay"));
		m_rbSl->setEnabled(false);
		m_rbSpin->setEnabled(false);
	}
	onReplaySel(-1);
	// Keep-size checked = send the acquisition size enlarged by the active rotation angle (an angle restored from
	// ini takes effect immediately after startup/source switch; unchecked sends nothing extra, preserving the old
	// behavior — the ROI only changes the frame source when the user applies it or via linkage). The new source's
	// actual geometry is unknown (camera hardware state can persist across processes), so the applied-size record
	// is reset to unknown; while the camera is not grabbing, setRoi honestly fails, and the pending size is
	// re-issued by the connectionChanged→rotKeepCheck path above once grabbing is ready.
	m_appliedRoiW = -1; m_appliedRoiH = -1;
	if (m_rotKeepSize->isChecked()) applyRoi();
}

// Acquisition start/stop: calls the active source's start/stop and syncs the button state; a failed start
// (camera absent / occupied) bounces the button back unchecked. Source-switch paths (onSourceToggled /
// constructor) go through this same entry so the button always reflects reality.
void MainWindow::setAcquiring(bool on)
{
	if (on) {
		if (m_active && !m_active->start()) {
			if (m_acqBtn) {
				QSignalBlocker b(m_acqBtn);
				m_acqBtn->setChecked(false);
				m_acqBtn->setText(L("开始采集", "Start"));
			}
			return;
		}
	} else if (m_active) {
		m_active->stop();
	}
	if (m_acqBtn) {
		QSignalBlocker b(m_acqBtn);
		m_acqBtn->setChecked(on);
		m_acqBtn->setText(on ? L("停止采集", "Stop") : L("开始采集", "Start"));
	}
	// Starting acquisition manually also exits replay mode (toggled(false) returns to live frames; m_acqBtn is
	// already checked so this function will not be re-entered — no loop).
	if (on && m_rbBtn && m_rbBtn->isChecked()) m_rbBtn->setChecked(false);
}

void MainWindow::onSourceToggled()
{
	printf("[win] toggled: cam=%d\n", m_radioCam->isChecked() ? 1 : 0); fflush(stdout);
	// An exclusive button-group switch fires toggled twice per Qt semantics (old button false + new button true);
	// if the second call's target source is already attached it no-ops, preventing a stop+start storm on the same
	// source (one of the freeze triggers identified while investigating the stream freeze).
	if (m_active && m_active == (m_radioCam->isChecked() ? (CameraSource*)m_mvs : (CameraSource*)m_sim))
		return;
	// Switching sources starts acquisition immediately (start/stop button state synced; attachSource already stopped the old source)
	if (m_radioCam->isChecked()) {
		if (!m_mvs) m_mvs = new MvsSource(this);
		attachSource(m_mvs);
		setAcquiring(true);
	} else {
		if (!m_sim) m_sim = new SimulatedSource(this);
		attachSource(m_sim);
		setAcquiring(true);
	}
}

void MainWindow::applyCameraParams()
{
	if (!m_active) return;
	// The Apply button's status message was removed together with the "camera status" group as redundant; sends go
	// through an asynchronous mailbox and the effective behavior is whatever the source-side worker does (checkable
	// via the stdout mirror); reading nodes mid-grab kills the stream, so readback only happens before grabbing.
	m_active->setExposureUs(m_exp->value() * 1000.0);   // UI exposure is ms; node/mailbox unit is µs
	m_active->setGainDb(m_gain->value());
	m_active->setFpsHz(m_fps->value());   // fps is sent together with Apply (camera frame-rate node / simulator timer)
}

// The currently active rotation angle (same source of truth as onFrame's rotation logic; with the three modes
// mutually exclusive, priority = custom > realtime-auto > one-shot).
static double rotActiveAngle(bool custom, double customDeg, bool spot, bool angleValid, double autoAngle,
                             bool onceActive, double onceAngle)
{
	if (custom) return customDeg;
	if (spot) return angleValid ? autoAngle : 0.0;
	if (onceActive) return onceAngle;
	return 0.0;
}

// Exit one-shot rotation mode: reset the flag plus the button's checked state/text/color (shared by the realtime
// and custom checkboxes and by the restore-defaults path).
void MainWindow::exitOnceMode()
{
	if (!m_rotOnceActive && !m_rotOnce->isChecked())
		return;
	m_rotOnceActive = false;
	m_rotOnceAngle = 0.0;
	QSignalBlocker blk(m_rotOnce);
	m_rotOnce->setChecked(false);   // clears the native checked state too (after the custom-drawn style was withdrawn, visuals are fully native, same as the Stop button)
	m_rotOnce->setText(L("单次光斑旋转", "Rotate Spot Once"));
}

// Sensor-crop send: only reports mailbox acceptance; the effective result is whatever the source-side status
// messages say (same convention as applyCameraParams). Center = bottom-left origin on the full sensor (y up);
// the conversion and 4-multiple alignment happen source-side (the camera's sensorH lives in the worker). With
// keep-size checked the acquisition window is enlarged (rotation output keeps the configured size = wider
// acquisition), clamped to the sensor maximum when exceeded.
void MainWindow::applyRoi()
{
	if (!m_active) return;
	int w = m_roiW->value(), h = m_roiH->value();
	effAcqSize(w, h);
	const int xc = std::clamp(m_roiXc->value(), w / 2, kSensorW - w / 2);   // clamp the center so the enlarged window stays inside the sensor
	const int yc = std::clamp(m_roiYc->value(), h / 2, kSensorH - h / 2);
	// Readback-is-truth: only record the applied size when the source accepts (setRoi true) — while the camera is
	// not grabbing, setRoi honestly fails, and a false record would make rotKeepCheck treat the pending re-issue
	// as already done (the post-connection re-issue via connectionChanged depends on this flag being truthful).
	if (m_active->setRoi(w, h, xc, yc)) {
		m_appliedRoiW = w; m_appliedRoiH = h;
	}
}

// Configured size → actual sent size = the bounding box of the configured rectangle rotated by a
// (width = ⌈w·|cos|+h·|sin|⌉, height = ⌈w·|sin|+h·|cos|⌉, per-axis independent, rounded up to a multiple of 4 for
// camera node alignment, clamped to the sensor maximum) — the acquisition then exactly covers the sampling extent
// of the exact-size rotation output, so black corners are zero; unchecked / no rotation returns unchanged.
void MainWindow::effAcqSize(int& w, int& h) const
{
	if (!m_rotKeepSize || !m_rotKeepSize->isChecked())
		return;
	const double a = rotActiveAngle(m_rotCustom->isChecked(), m_rotDeg->value(), m_rotSpot->isChecked(),
	                                m_rotAngleValid, m_rotAngle, m_rotOnceActive, m_rotOnceAngle);
	if (a == 0.0)
		return;
	const double rad = std::fabs(a) * std::acos(-1.0) / 180.0;
	const double c = std::fabs(std::cos(rad)), s = std::fabs(std::sin(rad));
	const int wd = w, hd = h;   // both axis formulas use the original configured size
	w = std::min(kSensorW, ((int)std::ceil(wd * c + hd * s) + 3) & ~3);
	h = std::min(kSensorH, ((int)std::ceil(wd * s + hd * c) + 3) & ~3);
}

// Re-send only when the angle change alters the required acquisition size vs what was already sent (identical = zero action, avoiding pointless stream stops).
void MainWindow::rotKeepCheck()
{
	if (!m_rotKeepSize || !m_rotKeepSize->isChecked() || !m_active) return;
	int w = m_roiW->value(), h = m_roiH->value();
	effAcqSize(w, h);
	if (w != m_appliedRoiW || h != m_appliedRoiH)
		applyRoi();
}

// Center range follows the size (the note refresh was split out to updateRoiNote — the sensor-range rows only
// reflect the window actually in effect, so spin edits no longer touch them early): spin range = the legal center
// interval (the window must stay entirely inside the sensor → X ∈ [w/2, 1280−w/2]).
void MainWindow::syncRoiLimits()
{
	const int w = m_roiW->value(), h = m_roiH->value();
	m_roiXc->setRange(w / 2, kSensorW - w / 2);
	m_roiYc->setRange(h / 2, kSensorH - h / 2);
}

// Sensor-range note: shows the actually-effective view's coverage interval with the unit after the numbers.
// Data source = the rotated frame's real geometry (the note must describe the post-rotation view — the inscribed
// rectangle's origin has shifted with the crop, w/h are the inscribed-rect dimensions; the sensor settings
// themselves are untouched), updated only when a new frame arrives after a sensor/rotation geometry change.
// Converting the top-left origin to bottom-left-origin coverage: X = [ox, ox+w], Y = [sh−oy−h, sh−oy].
void MainWindow::updateRoiNote(int w, int h, int ox, int oy, int sw, int sh)
{
	// The X/Y range rows are always in px, independent of the global size unit — px reads best here (the
	// acquisition-size and window-center parameters are inherently px and the range rows match their convention;
	// converted units only appear in the readouts and the view axes).
	const double sc = 1.0;
	const int dec = 1;
	const QString u = QStringLiteral("px");
	m_roiNoteX->setText(QString("%1 %3 – %2 %3").arg(ox * sc, 0, 'f', dec).arg((ox + w) * sc, 0, 'f', dec).arg(u));
	m_roiNoteY->setText(QString("%1 %3 – %2 %3").arg((sh - oy - h) * sc, 0, 'f', dec).arg((sh - oy) * sc, 0, 'f', dec).arg(u));
	m_roiNoteOrg->setText(L("相机原始靶面左下角", "sensor bottom-left"));
}

void MainWindow::shutterOpen()  { m_shutter->command(true); }
void MainWindow::shutterClose() { m_shutter->command(false); }

// Background-subtraction status hint + button gating: shutter present = auto-calibration ready, button always
// enabled; shutter absent = manual block mode — peak <50% of full scale (2048 ADU, 12-bit) means "beam blocked"
// and releases the button, otherwise "beam not blocked" keeps it greyed (prevents sampling away real light);
// no valid frame = "please block the beam manually" guidance.
void MainWindow::updateBkgState()
{
	if (!m_bkgState) return;
	QString txt;
	bool btnOk;
	if (m_shutterConnected) {
		txt = L("自动开关快门校准状态就绪", "Auto shutter calibration ready");
		btnOk = true;
	} else if (!m_lastR.valid) {
		txt = L("快门未连接，请手动遮挡光斑", "Shutter not connected — please block the beam manually");
		btnOk = false;
	} else if (m_beamBlocked) {
		txt = L("快门未连接，光斑已遮挡", "Shutter not connected — beam blocked");
		btnOk = true;
	} else {
		txt = L("快门未连接，光斑未遮挡", "Shutter not connected — beam not blocked");
		btnOk = false;
	}
	if (m_bkgState->text() != txt) {
		m_bkgState->setText(txt);
		m_bkgState->setStyleSheet(btnOk ? "color:#0a0;" : "color:#c60;");
	}
	m_bkgBtn->setEnabled(btnOk && !m_bkgBusy);
}

double MainWindow::bgValue() const
{
	return m_bgCustom->isChecked() ? m_bg->value() : -1.0;   // checked = custom manual value; unchecked = -1 sentinel = per-frame median auto mode in the analyzer
}

void MainWindow::startRoiDraw()
{
	m_view->setRoiDrawMode(true);
	m_roiLabel->setText(L("拖拽框选中…", "Drag to select…"));
}

void MainWindow::pushStat(std::deque<double>& q, double v)
{
	q.push_back(v);
	if (q.size() > 100) q.pop_front();
}

// Stats text split into two lines = {mean·σ line, min·max line} so the min/max values align with the mean.
QStringList MainWindow::statLines(const std::deque<double>& q, double scale, int dec, const QString& unit)
{
	if (q.empty()) return { "—", "" };
	auto [mn, mx] = std::minmax_element(q.begin(), q.end());
	double mean = std::accumulate(q.begin(), q.end(), 0.0) / q.size();
	double var = 0;
	for (double v : q) var += (v - mean) * (v - mean);
	var /= q.size();
	// The unit follows every parameter value (consistent with the other columns); name columns no longer carry parenthesized units.
	return { L("均值 %1 %3 · σ %2 %3", "mean %1 %3 · σ %2 %3")
	         .arg(mean * scale, 0, 'f', dec).arg(std::sqrt(var) * scale, 0, 'f', dec).arg(unit),
	         L("min %1 %3 · max %2 %3", "min %1 %3 · max %2 %3")
	         .arg(*mn * scale, 0, 'f', dec).arg(*mx * scale, 0, 'f', dec).arg(unit) };
}

// Size readout: converted by the current unit (software-wide convention: px = 1 decimal, µm/mm = 3 decimals).
QString MainWindow::fmtSize(double pxVal) const
{
	double v = pxVal * sizeScale();
	return QString::number(v, 'f', m_sizeUnit->currentIndex() == 0 ? 1 : 3);
}

// Refresh the three Home readout columns (primary diameter follows m_diamDef; unit columns sync with the size unit).
void MainWindow::updateReadouts(const AnalysisResult& r)
{
	if (!r.valid) {
		for (QLabel* l : { m_rdPower, m_rdPkInt, m_rdDox, m_rdDoy, m_rdEff, m_rdEllip, m_rdOrient,
		                   m_rdCx, m_rdCy, m_rdPkX, m_rdPkY, m_rdSat })
			l->setText("—");
		m_rdPower->setText(L("无有效信号", "No valid signal"));
		return;
	}
	QString su = sizeUnit();
	for (QLabel* l : m_dUnitLbl) l->setText(su);
	int def = m_diamDef->currentIndex();
	double dox = def == 0 ? r.d4sigmaX : def == 1 ? r.d84X : r.fwhmX;
	double doy = def == 0 ? r.d4sigmaY : def == 1 ? r.d84Y : r.fwhmY;
	m_rdPower->setText(fmtPower(effPower(r)));   // with subtraction checked = effective power after background subtraction
	m_rdPkInt->setText(fmtPower(r.peak));   // peak intensity = peak pixel ADU under the same conversion (a per-pixel quantity, not subject to background subtraction)
	// Bug fix: each unit column picks its tier from the same value as its own number — the two rows used to share
	// powUnitLabel (tier by power), so the peak row's number (tiered by peak) and its unit could disagree by 3
	// orders of magnitude at different magnitudes.
	if (m_powUnitLbl.size() >= 2) {
		m_powUnitLbl[0]->setText(powUnitForAdu(effPower(r)));
		m_powUnitLbl[1]->setText(powUnitForAdu(r.peak));
	}
	m_rdDox->setText(fmtSize(dox));
	m_rdDoy->setText(fmtSize(doy));
	m_rdEff->setText(fmtSize(std::sqrt(dox * doy)));
	m_rdEllip->setText(QString::number(r.ellipticity, 'f', 3));
	m_rdOrient->setText(QString::number(r.orientationDeg, 'f', 1));
	m_rdCx->setText(fmtSize(r.cx0));   // position rows follow the size unit (an old version always reported px)
	m_rdCy->setText(fmtSize(r.cy0));   // the *0 fields = full-sensor coordinates, bottom-left origin y up (mapped
	                                   // back to the raw frame + frameToTarget; cx/cy are unsuitable because the
	                                   // rotation angle sweeps positions around in the rotated frame)
	m_rdPkX->setText(fmtSize(r.peakX0));
	m_rdPkY->setText(fmtSize(r.peakY0));
	m_rdSat->setText(QString::number(r.peak / 4095.0 * 100.0, 'f', 1));   // 12-bit full scale = 4095
}

// Profile chart refresh, decimated to ≤640 points per series (redraw cost climbs steeply with point count; 640 points is visually lossless — part of the stutter remediation).
void MainWindow::updateProfileChart(const AnalysisResult& r)
{
	if (!r.valid) return;
	// Global unit convention: X = position converted by the size unit, Y = intensity following the power unit
	// (ADU tier = per-column/row pixel sums in ADU); all four combo tiers {ADU,µW,mW,W} are mapped (an old version
	// displayed everything non-ADU as W, leaving µW/mW unconverted — that shortcut was explicitly called out).
	static const double kPfDiv[] = { 1.0, 1e-6, 1e-3, 1.0 };   // watts per unit for each tier (µW/mW/W)
	static const char* kPfUnit[] = { "ADU", "µW", "mW", "W" };
	int pu = m_powUnit->currentIndex();
	double s = sizeScale();
	double vmax = std::max(r.profileX.empty() ? 1.0 : *std::max_element(r.profileX.begin(), r.profileX.end()),
	                       r.profileY.empty() ? 1.0 : *std::max_element(r.profileY.begin(), r.profileY.end()));
	// Y-axis unit auto-adaptation (a weak beam pinned to the mW tier renders all ticks as 0.000 = unreadable):
	// pick the tier by the axis peak, floor reaching down to nW/pW; with custom bounds checked = follow the combo
	// unit (the bound spins' suffixes match); global ADU tier = ADU.
	const bool pfCustom = m_pfYCustom && m_pfYCustom->isChecked();
	int yIdx = -1;                 // -1 = ADU tier
	double pScale = 1.0;
	QString yUnit = QStringLiteral("ADU");
	if (pu != 0) {
		if (pfCustom) { yIdx = pu; pScale = powWPerAdu() / kPfDiv[pu]; yUnit = QString::fromUtf8(kPfUnit[pu]); }
		else { int i = powPickIdx(vmax * powWPerAdu()); yIdx = i; pScale = powWPerAdu() / kPowScale[i];
		       yUnit = QString::fromUtf8(kPowUnit[i]); }
	}
	auto decimate = [](const std::vector<double>& v, double origin, double step, double xk, double yk) {
		int stride = std::max(1, (int)v.size() / 640);
		QVector<QPointF> pts;
		pts.reserve((int)v.size() / stride + 1);
		for (size_t i = 0; i < v.size(); i += stride)
			pts.append(QPointF((origin + step * (double)i) * xk, v[i] * yk));
		return pts;
	};
	// Position axis in full-sensor coordinates (bottom-left origin): X profile x = originX + column index; the Y
	// profile's row index is flipped (step = −1, first row = topmost row = maximum sensor y). With spot rotation
	// on, m_last is the rotated inscribed-rect frame and the axis coordinates are that region's coordinates
	// (display reference).
	const int ox = m_last.originX, oy = m_last.originY;
	const int sh = m_last.sensorH > 0 ? m_last.sensorH : m_last.h;
	const double xLo = (ox + r.roiX) * s, xHi = (ox + r.roiX + r.roiW) * s;
	const double yC = (sh - 1) - (oy + r.roiY);                          // sensor y (px) of the Y profile's first row
	// QtCharts was replaced by the self-drawn LineChartView (license-relicensing reason); all axis conventions
	// carried over — bottom-left origin, tick decimals follow the unit (px = 1, µm/mm = 3; Y axis ADU = 1,
	// µW/mW/W = 3), custom upper/lower bounds, and the "summed per column/row" axis title.
	QVector<LineChartView::Series> ser;
	ser.append({ L("X 剖面", "X Profile"), QColor(0x1f, 0x77, 0xb4), decimate(r.profileX, ox + r.roiX, 1.0, s, pScale) });
	ser.append({ L("Y 剖面", "Y Profile"), QColor(0xd6, 0x60, 0x4d), decimate(r.profileY, yC, -1.0, s, pScale) });
	m_profileChart->setSeries(ser);
	m_profileChart->setXRange(std::min(xLo, (yC - r.roiH) * s), std::max(xHi, yC * s));
	if (pfCustom && m_pfYHi->value() > m_pfYLo->value())
		m_profileChart->setYRange(m_pfYLo->value(), m_pfYHi->value());
	else
		m_profileChart->setYRange(0, vmax * pScale * 1.05);
	m_profileChart->setTitles(QString(),
	                          L("位置", "Position") + " (" + sizeUnit() + ")",
	                          yIdx < 0 ? L("强度 (ADU，每列/每行求和)", "Intensity (ADU, summed per column/row)")
	                                   : L("强度 (%1，每列/每行求和)", "Intensity (%1, summed per column/row)").arg(yUnit));
	m_profileChart->setDecimals(m_sizeUnit->currentIndex() == 0 ? 1 : 3, pu == 0 ? 1 : 3);
}

// Beam-track drawing lives in TrackView::paintEvent (track_view.h): history always stores raw px; paint converts
// with the current size scale + tight window. (The QChart version was abandoned: QtCharts value axes do not
// support inverted ranges, and series attached to such an axis silently render nothing — confirmed with a pixel probe.)

// View switch: change stack page + sync buttons + re-feed the newly shown page with the latest data (the other
// half of mutually-exclusive frame feeding; history is recorded in full so switching back loses nothing).
void MainWindow::onViewPage(int page)
{
	m_stack->setCurrentIndex(page);
	for (int i = 0; i < 5; ++i)
		if (m_viewBtn[i]) m_viewBtn[i]->setChecked(i == page);
	m_replayRow->setVisible(page <= 2);   // the replay row is shared by the 2D intensity / 3D surface / profile pages
	m_trackClearRow->setVisible(page == 4);   // the clear-track row only shows on the centroid-track page
	switch (page) {
	case 0:   // 2D intensity: axis units follow the global size unit (the page-switch of three wirings: startup / page switch / unit change)
		m_view->setUnit(sizeScale(), sizeUnit());
		Q_FALLTHROUGH();
	case 1:   // 3D surface
	case 2:   // profile: all three pages go through the display router (replay feeds the selected frame, not the live one)
		showDisplay();
		break;
	case 3: rebuildSizeChart(); break;
	case 4: m_track->setUnit(sizeScale(), sizeUnit()); break;   // setUnit also updates (paint pulls m_trackHist in full itself)
	}
}

// Display-layer router (replay system): pick the live frame/result (m_last/m_lastR) or the replayed one per
// m_rbSel, feed the currently visible page + refresh the timestamp; Home readouts follow the replayed frame;
// stats / size chart / centroid track do not go through this path and stay live always.
void MainWindow::showDisplay()
{
	const bool replay = m_rbSel >= 0 && m_rbSel < (int)m_rb.size();
	const Frame& fr = replay ? m_rb[m_rbSel].fr : m_last;
	const AnalysisResult& r = replay ? m_rb[m_rbSel].r : m_lastR;
	if (m_tsLbl)
		m_tsLbl->setText(fr.captureMs > 0
			? QDateTime::fromMSecsSinceEpoch(fr.captureMs).toString("yyyy-MM-dd HH:mm:ss.zzz")
			: QString());
	if (!fr.valid()) return;
	switch (m_stack->currentIndex()) {
	case 0:
		m_view->setFrame(fr);
		m_view->setCentroid(r.cx, r.cy, r.valid && m_showCross->isChecked());
		if (replay)
			m_view->setBeam84(r.cx, r.cy, r.d84X, r.d84Y, 0.0, m_shape84->currentIndex(), m_show84->isChecked());
		else
			m_view->setBeam84(m_l84cx, m_l84cy, m_l84wx, m_l84wy, m_l84rot, m_shape84->currentIndex(), m_show84->isChecked());
		break;
	case 1:
		m_3d->setRange(m_effLo, m_effHi);
		m_3d->setFrame(fr);
		break;
	case 2:
		if (r.valid) updateProfileChart(r);
		break;
	default: break;   // pages 3/4 stay on the live pipeline
	}
	if (replay) updateReadouts(r);   // Home readouts follow the replayed frame (live mode is refreshed per-frame by onFrame, no double work)
}

// Replay position change (-1 = live): sync slider / position box / frame-number label + refresh the display (shared by all entry points).
void MainWindow::onReplaySel(int idx)
{
	const int sz = (int)m_rb.size();
	const bool replayMode = m_rbBtn && m_rbBtn->isChecked();
	// Out of range (buffer trimmed by the cap / cleared): in replay mode clamp to the newest frame — the mode does not change because of it; live mode falls back to -1
	if (idx >= sz) idx = (replayMode && sz > 0) ? sz - 1 : -1;
	m_rbSel = idx;
	{
		QSignalBlocker b1(m_rbSl), b2(m_rbSpin);
		// Replay mode: range 0..sz-1, every position is a replayed frame (rightmost = newest; dragging to the end
		// does not switch to live — only the toggle button changes the mode); live mode: range 0..sz (rightmost =
		// live, controls disabled and undraggable).
		m_rbSl->setRange(0, replayMode ? std::max(0, sz - 1) : sz);
		m_rbSl->setValue(idx >= 0 ? idx : (replayMode ? std::max(0, sz - 1) : sz));
		m_rbSpin->setRange(replayMode && sz > 0 ? 1 : 0, sz);
		m_rbSpin->setValue(idx >= 0 ? idx + 1 : (replayMode && sz > 0 ? 1 : 0));   // replay frame k (forward order: 1 = oldest)
	}
	m_rbLbl->setText(idx < 0 ? L("实时", "Live")
	                         : L("回看第 %1/%2 帧", "frame %1/%2").arg(idx + 1).arg(sz));   // forward frame numbering (slider far left = frame 1 = oldest)
	showDisplay();
}

// Current view title (default file name for Save PNG/CSV/RAW; same wording as the bottom switch buttons).
QString MainWindow::viewTitle() const
{
	static const char* kZh[5] = { "2D 强度", "3D 曲面", "光斑剖面", "光斑尺寸", "质心追踪" };
	static const char* kEn[5] = { "2D Intensity", "3D Surface", "Beam Profile", "Beam Size", "Centroid Track" };
	const int i = m_stack ? m_stack->currentIndex() : 0;
	return QString::fromUtf8(g_cn ? kZh[i] : kEn[i]);
}

// ── Settings persistence (QSettings INI): path goes through dataDir() — writable exe directory keeps
//    dev/portable installs unchanged, read-only installs (Program Files) fall back to %APPDATA%\SteBeam ──
QString MainWindow::iniPath() const
{
	return dataDir() + "/settings.ini";
}

// Path = whatever the dialog chose (custom location and file name allowed); effectiveness readback = file exists and is non-empty after sync.
void MainWindow::saveSettingsIni(const QString& path)
{
	QSettings s(path, QSettings::IniFormat);
	s.setValue("acquisition/expMs", m_exp->value());   // exposure UI unit is ms; the key name follows the unit
	s.setValue("acquisition/gainDb", m_gain->value());
	s.setValue("acquisition/fpsHz", m_fps->value());
	s.setValue("acquisition/roiW", m_roiW->value());   // sensor crop persisted
	s.setValue("acquisition/roiH", m_roiH->value());
	s.setValue("acquisition/roiXc", m_roiXc->value());
	s.setValue("acquisition/roiYc", m_roiYc->value());
	s.setValue("acquisition/winIdx", m_winCombo->currentIndex());
	s.setValue("home/diamDef", m_diamDef->currentIndex());
	s.setValue("home/sizeUnit", m_sizeUnit->currentIndex());
	s.setValue("home/showCross", m_showCross->isChecked());
	s.setValue("home/show84", m_show84->isChecked());   // moved to the Home section, key follows
	s.setValue("home/shape84", m_shape84->currentIndex());
	s.setValue("home/rotSpot", m_rotSpot->isChecked());
	s.setValue("home/rotCustom", m_rotCustom->isChecked());   // custom rotation angle
	s.setValue("home/rotDeg", m_rotDeg->value());
	s.setValue("home/rotKeepSize", m_rotKeepSize->isChecked());   // keep sensor size after rotation
	s.setValue("setup/pcAuto", m_pcAuto->isChecked());
	s.setValue("setup/pcLo", m_loSl->value());
	s.setValue("setup/pcHi", m_hiSl->value());
	s.setValue("setup/bg", m_bg->value());
	s.setValue("setup/bgCustom", m_bgCustom->isChecked());    // inverted-semantics key (checked = custom value; unchecked = auto median mode)
	s.setValue("setup/rawBgSub", m_rawBgSub->isChecked());    // subtract background from raw intensity data
	s.setValue("setup/thrPct", m_thr->value());
	s.setValue("setup/pixSize", m_pixSize->value());
	s.setValue("setup/waveNm", m_waveNm->value());
	s.setValue("home/powUnit", m_powUnit->currentIndex());   // control moved into Main Controls, key follows
	s.setValue("setup/calCustom", m_calCustom->isChecked());
	s.setValue("setup/qePct", m_qePct->value());
	s.setValue("setup/ePerAdu", m_ePerAdu->value());
	s.setValue("setup/calCoef", m_calCoef->value());
	s.setValue("acq/subBkg", m_subBkg->isChecked());
	s.setValue("acq/bkgAdu", m_bkgAdu);
	s.setValue("acq/bkgUnitAdu", m_bkgUnitAdu);   // per-unit background sample persisted with the background power
	s.setValue("acq/bkgSec", m_bkgSec->value());
	s.setValue("setup/rbMax", m_rbMax->value());        // replay frame cap (0 = feature disabled)
	s.setValue("setup/csvEvery", m_csvEvery->value());  // realtime-CSV interval in frames (the button state is deliberately not persisted, so a restart never auto-starts recording)
	s.setValue("setup/csvDir", m_csv.dir());            // realtime-CSV folder (user-customizable)
	s.setValue("view3d/quality", m_3dQuality->currentIndex());
	s.setValue("view3d/smooth", m_3dSmooth->currentIndex());
	s.setValue("axes/szYCustom", m_szYCustom->isChecked());
	s.setValue("axes/szYLo", m_szYLo->value());
	s.setValue("axes/szYHi", m_szYHi->value());
	s.setValue("axes/pfYCustom", m_pfYCustom->isChecked());
	s.setValue("axes/pfYLo", m_pfYLo->value());
	s.setValue("axes/pfYHi", m_pfYHi->value());
	s.setValue("axes/trXCustom", m_trXCustom->isChecked());
	s.setValue("axes/trXLo", m_trXLo->value());
	s.setValue("axes/trXHi", m_trXHi->value());
	s.setValue("axes/trYCustom", m_trYCustom->isChecked());
	s.setValue("axes/trYLo", m_trYLo->value());
	s.setValue("axes/trYHi", m_trYHi->value());
	s.setValue("view/page", m_stack->currentIndex());
	s.sync();
	// Effectiveness readback: a successful sync() return is not the criterion — independently verify the file exists on disk and is non-empty
	const bool ok = QFile(path).exists() && QFileInfo(path).size() > 0;
	statusBar()->showMessage(ok ? L("设置已保存：%1", "Config saved: %1").arg(path)
	                            : L("设置保存失败：%1", "Config save FAILED: %1").arg(path), 6000);
}

// path = startup default or dialog choice; quiet = automatic startup load (missing file / no keys silently keep
// defaults); false = button-triggered load (always reports the outcome — picking the wrong file is not silently swallowed).
void MainWindow::loadSettingsIni(const QString& path, bool quiet)
{
	QSettings s(path, QSettings::IniFormat);
	if (!s.contains("acquisition/expMs") && !s.contains("acquisition/expUs")) {
		if (!quiet)
			statusBar()->showMessage(L("文件无 SteBeam 设置项，未加载：%1", "No SteBeam settings in file, not loaded: %1").arg(path), 6000);
		return;   // first run / not a settings file: keep the widget defaults
	}
	// Exposure key migration: read expMs directly; an old ini only has expUs (µs), divide by 1000
	m_exp->setValue(s.contains("acquisition/expMs")
		? s.value("acquisition/expMs").toDouble()
		: s.value("acquisition/expUs").toDouble() / 1000.0);
	m_gain->setValue(s.value("acquisition/gainDb").toDouble());
	m_fps->setValue(s.value("acquisition/fpsHz", 10.0).toDouble());   // fps persisted; default 10Hz (same as the creation site)
	m_roiW->setValue(s.value("acquisition/roiW", kSensorW).toInt());   // sensor crop persisted (old ini without the key = full sensor)
	m_roiH->setValue(s.value("acquisition/roiH", kSensorH).toInt());
	m_roiXc->setValue(s.value("acquisition/roiXc", kSensorW / 2).toInt());
	m_roiYc->setValue(s.value("acquisition/roiYc", kSensorH / 2).toInt());
	syncRoiLimits();   // re-derive the center range from the restored sizes (setValue order can briefly put the center out of range; clamp uniformly here)
	m_winCombo->setCurrentIndex(s.value("acquisition/winIdx").toInt());
	m_diamDef->setCurrentIndex(s.value("home/diamDef").toInt());
	m_sizeUnit->setCurrentIndex(s.value("home/sizeUnit").toInt());
	// Bug fix from a full read-through: a missing single key (hand-edited ini / old-version migration) makes
	// value() default to 0/false, disconnected from the widget's creation default (calCoef→0 pins power at 0,
	// thrPct→0 disables the threshold, pcLo/pcHi→0 collapses the range, pixSize→clamped 0.01, waveNm→clamped
	// 100, showCross→false). Every key now carries an explicit default equal to its widget's creation default;
	// the normal full-write path is unaffected.
	m_showCross->setChecked(s.value("home/showCross", true).toBool());
	m_pcAuto->setChecked(s.value("setup/pcAuto", false).toBool());
	m_loSl->setValue(s.value("setup/pcLo", -100).toInt());
	m_hiSl->setValue(s.value("setup/pcHi", 4096).toInt());
	m_bg->setValue(s.value("setup/bg", 0.0).toDouble());
	m_bgCustom->setChecked(s.value("setup/bgCustom", false).toBool());   // default = auto mode (inverted-semantic key)
	m_rawBgSub->setChecked(s.value("setup/rawBgSub", false).toBool());
	m_thr->setValue(s.value("setup/thrPct", 10.0).toDouble());
	m_show84->setChecked(s.contains("home/show84") ? s.value("home/show84").toBool()
	                       : s.value("setup/show84", true).toBool());   // key moved to home/; the old setup/show84 is read for one version of compatibility
	m_shape84->setCurrentIndex(s.value("home/shape84").toInt());
	m_rotSpot->setChecked(s.value("home/rotSpot").toBool());
	m_rotCustom->setChecked(s.value("home/rotCustom").toBool());   // custom rotation angle (the toggled wiring already exists from buildUi; the explicit setEnabled sync is belt-and-braces)
	m_rotDeg->setValue(s.value("home/rotDeg", 0.0).toDouble());
	m_rotDeg->setEnabled(m_rotCustom->isChecked());
	m_rotKeepSize->setChecked(s.value("home/rotKeepSize").toBool());   // keep-sensor-size (setting it fires toggled → re-sends per the restored angle state; during startup m_active is null, attachSource re-issues at its tail)
	m_pixSize->setValue(s.value("setup/pixSize", 4.8).toDouble());
	m_waveNm->setValue(s.value("setup/waveNm", 1064.0).toDouble());
	m_powUnit->setCurrentIndex(s.value("home/powUnit").toInt());
	m_calCustom->setChecked(s.value("setup/calCustom", false).toBool());
	m_qePct->setValue(s.value("setup/qePct", builtinQePct(1064.0)).toDouble());
	m_ePerAdu->setValue(s.value("setup/ePerAdu", 1.0).toDouble());
	if (!m_calCustom->isChecked()) {   // built-in mode: hand-edited values are void, QE follows the official curve for the wavelength and gain returns to its placeholder (gated)
		m_qePct->setValue(builtinQePct(m_waveNm->value()));
		m_ePerAdu->setValue(1.0);
	}
	m_qePct->setEnabled(m_calCustom->isChecked());
	m_ePerAdu->setEnabled(m_calCustom->isChecked());
	m_calCoef->setValue(s.value("setup/calCoef", 1.0).toDouble());   // default 1.0 when the key is absent (reading 0 would pin power at 0)
	m_bkgSec->setValue(s.value("acq/bkgSec", 2.0).toDouble());
	m_rbMax->setValue(s.value("setup/rbMax", 100).toInt());     // replay cap (old ini without the key = default 100)
	m_csvEvery->setValue(s.value("setup/csvEvery", 10).toInt());   // realtime-CSV interval
	{   // realtime-CSV folder (old ini without the key = keep default exe\realtime_csv)
		const QString d = s.value("setup/csvDir").toString();
		if (!d.isEmpty()) m_csv.setDir(d);
		m_csvDirBtn->setToolTip(L("自定义存储文件夹（当前：", "Custom folder (current: ") + m_csv.dir() + "）");
	}
	m_3dQuality->setCurrentIndex(s.value("view3d/quality", 1).toInt());
	m_3dSmooth->setCurrentIndex(s.value("view3d/smooth", 1).toInt());   // default = smooth (when the ini has no key)
	m_3d->setQuality(m_3dQuality->currentIndex());
	m_3d->setSmooth(m_3dSmooth->currentIndex() == 1);
	m_szYCustom->setChecked(s.value("axes/szYCustom").toBool());
	m_szYLo->setValue(s.value("axes/szYLo", 0.0).toDouble());
	m_szYHi->setValue(s.value("axes/szYHi", 1000.0).toDouble());
	m_pfYCustom->setChecked(s.value("axes/pfYCustom").toBool());
	m_pfYLo->setValue(s.value("axes/pfYLo", 0.0).toDouble());
	m_pfYHi->setValue(s.value("axes/pfYHi", 100000.0).toDouble());
	m_trXCustom->setChecked(s.value("axes/trXCustom").toBool());
	m_trXLo->setValue(s.value("axes/trXLo", 0.0).toDouble());
	m_trXHi->setValue(s.value("axes/trXHi", 1280.0).toDouble());
	m_trYCustom->setChecked(s.value("axes/trYCustom").toBool());
	m_trYLo->setValue(s.value("axes/trYLo", 0.0).toDouble());
	m_trYHi->setValue(s.value("axes/trYHi", 1024.0).toDouble());
	m_szYLo->setEnabled(m_szYCustom->isChecked());
	m_szYHi->setEnabled(m_szYCustom->isChecked());
	m_pfYLo->setEnabled(m_pfYCustom->isChecked());
	m_pfYHi->setEnabled(m_pfYCustom->isChecked());
	m_trXLo->setEnabled(m_trXCustom->isChecked());
	m_trXHi->setEnabled(m_trXCustom->isChecked());
	m_trYLo->setEnabled(m_trYCustom->isChecked());
	m_trYHi->setEnabled(m_trYCustom->isChecked());
	m_track->setCustomRange(m_trXCustom->isChecked(), m_trXLo->value(), m_trXHi->value(),
	                        m_trYCustom->isChecked(), m_trYLo->value(), m_trYHi->value());
	m_bkgAdu = s.value("acq/bkgAdu", -1.0).toDouble();
	m_bkgUnitAdu = s.value("acq/bkgUnitAdu", -1.0).toDouble();   // per-unit background sample restored together with it
	if (m_bkgAdu >= 0) {   // a previous session already sampled the background: restore the checkbox-enabled state
		m_subBkg->setEnabled(true);
		m_subBkg->setChecked(s.value("acq/subBkg").toBool());
		m_bkgLblSampled = true; m_bkgLblFromIni = true;
		refreshBkgLabels();   // rebuild both readout rows through the unified entry (global unit conversion + "last sampled" annotation)
	}
	onViewPage(s.value("view/page").toInt());
}

void MainWindow::applySettingsDefaults()
{
	m_exp->setValue(10.0);   // exposure default in the UI unit ms (matches the widget's construction default)
	m_gain->setValue(0.0);
	m_fps->setValue(30.0);   // fps default 30Hz
	m_roiW->setValue(kSensorW);   // sensor crop default = full sensor
	m_roiH->setValue(kSensorH);
	m_roiXc->setValue(kSensorW / 2);
	m_roiYc->setValue(kSensorH / 2);
	syncRoiLimits();
	m_winCombo->setCurrentIndex(0);
	m_diamDef->setCurrentIndex(0);
	m_sizeUnit->setCurrentIndex(0);
	m_showCross->setChecked(true);
	m_pcAuto->setChecked(false);
	m_loSl->setValue(-100);
	m_hiSl->setValue(4096);
	m_bg->setValue(0.0);
	m_bgCustom->setChecked(false);   // default = auto mode (inverted-semantic checkbox)
	m_rawBgSub->setChecked(false);
	m_thr->setValue(10.0);
	m_show84->setChecked(true);
	m_shape84->setCurrentIndex(0);
	m_rotSpot->setChecked(false);
	exitOnceMode();   // one-shot rotation mode is never restored from ini/defaults; restoring defaults must explicitly exit it (button state reset too)
	m_rotCustom->setChecked(false);
	m_rotDeg->setValue(0.0);
	m_rotDeg->setEnabled(false);
	m_rotKeepSize->setChecked(false);   // defaults exit keep-size (the toggled wiring re-sends the original configured size via the preceding sensor/rotation reset path)
	m_appliedRoiW = -1; m_appliedRoiH = -1;   // the actually-sent size will be re-reported on the next apply
	m_pixSize->setValue(4.8);
	m_waveNm->setValue(1064.0);
	m_powUnit->setCurrentIndex(0);
	m_calCustom->setChecked(false);
	m_qePct->setEnabled(false);
	m_qePct->setValue(builtinQePct(1064.0));
	m_ePerAdu->setEnabled(false);
	m_ePerAdu->setValue(1.0);
	m_calCoef->setValue(1.0);
	m_bkgAdu = -1;
	m_bkgUnitAdu = -1; m_bkgUnitSum = 0;   // per-unit background sample cleared with the defaults
	m_bkgLblSampled = false; m_bkgLblFromIni = false;
	m_subBkg->setEnabled(false);
	m_subBkg->setChecked(false);
	m_bkgLabel->setText(L("背景功率: 未采集", "Background: not sampled"));
	refreshBkgLabels();   // unit-background row returns to "not sampled" (the custom checkbox was reset above; this follows the auto-mode convention)
	m_bkgSec->setValue(2.0);
	m_rbMax->setValue(100);      // replay cap default 100 frames
	m_csvEvery->setValue(10);    // realtime CSV default: every 10 frames
	m_csv.setDir(dataDir() + "/realtime_csv");   // storage folder back to default (dataDir() convention)
	m_csvDirBtn->setToolTip(L("自定义存储文件夹（当前：", "Custom folder (current: ") + m_csv.dir() + "）");
	m_3dQuality->setCurrentIndex(1);
	m_3dSmooth->setCurrentIndex(1);   // defaults = smooth
	m_3d->setQuality(1);
	m_3d->setSmooth(true);
	m_szYCustom->setChecked(false);
	m_szYLo->setValue(0); m_szYHi->setValue(1000);
	m_pfYCustom->setChecked(false);
	m_pfYLo->setValue(0); m_pfYHi->setValue(100000);
	m_trXCustom->setChecked(false);
	m_trXLo->setValue(0); m_trXHi->setValue(1280);
	m_trYCustom->setChecked(false);
	m_trYLo->setValue(0); m_trYHi->setValue(1024);
	for (QDoubleSpinBox* s : { m_szYLo, m_szYHi, m_pfYLo, m_pfYHi, m_trXLo, m_trXHi, m_trYLo, m_trYHi })
		s->setEnabled(false);
	rebuildSizeChart();
	if (m_lastR.valid) updateProfileChart(m_lastR);
	m_track->setCustomRange(false, 0, 0, false, 0, 0);
}

void MainWindow::resetSettingsIni()
{
	// ui/lang survives restore-defaults: language is a UI preference, not measurement configuration (otherwise an English UI would be bounced back to Chinese by the reset)
	QSettings s0(iniPath(), QSettings::IniFormat);
	const QString lang = s0.value("ui/lang", "cn").toString();
	QFile::remove(iniPath());
	{
		QSettings s1(iniPath(), QSettings::IniFormat);
		s1.setValue("ui/lang", lang);
		s1.sync();
	}
	applySettingsDefaults();
	statusBar()->showMessage(L("已恢复默认设置（settings.ini 已删除）", "Defaults restored (settings.ini deleted)"), 4000);
}

void MainWindow::onFrame(const Frame& f)
{
	static QElapsedTimer s_dbgClock;
	static qint64 s_dbgTotal = 0, s_dbgN = 0;
	static bool s_dbgInit = (s_dbgClock.start(), true);
	Q_UNUSED(s_dbgInit);
	const qint64 t0 = s_dbgClock.nsecsElapsed();
	++m_framesSeen;
	// One-shot rotation probe (STEBEAM_ROT_ONCE=frame number): programmatically click the button at that frame and
	// report the effective frame geometry before/after (effectiveness criterion = frame-size readback: after the
	// click the inscribed-rect size stays constant instead of varying per frame).
	static const int s_rotOnceAt = qEnvironmentVariable("STEBEAM_ROT_ONCE").toInt();
	if (s_rotOnceAt > 0 && m_framesSeen == s_rotOnceAt)
		m_rotOnce->click();
	// Sensor-preset probe (STEBEAM_ROI_PRESET=frame number): programmatically press the 500×500 button at that
	// frame, then read back the applied size and the spin values (effectiveness criterion = applied-sensor message
	// + spin values, not the click's return).
	static const int s_roiPresetAt = qEnvironmentVariable("STEBEAM_ROI_PRESET").toInt();
	if (s_roiPresetAt > 0 && m_framesSeen == s_roiPresetAt)
		m_roiP500->click();
	if (s_roiPresetAt > 0 && m_framesSeen == s_roiPresetAt + 1)   // read back +1 frame (applyRoi updates synchronously; +3 would collide with the selftest camera switch and report -1)
		printf("[roipreset] applied=%dx%d spin=%dx%d\n", m_appliedRoiW, m_appliedRoiH, m_roiW->value(), m_roiH->value());

	// Live frame rate: EMA of the frame interval (α=0.1 ≈ 10-frame window), display refreshed every ~15 frames to avoid high-frequency repaints
	{
		const qint64 nowMs = m_clock.elapsed();
		if (m_lastFrameMs >= 0 && nowMs > m_lastFrameMs) {
			double inst = 1000.0 / (double)(nowMs - m_lastFrameMs);
			m_fpsEma = m_fpsEma < 0 ? inst : m_fpsEma * 0.9 + inst * 0.1;
			// show once within the first two frames (a reading at startup instantly) + every 15 frames after (≈0.5s @30fps, avoids high-frequency repaints)
			if (m_fpsLive && (++m_fpsLiveTick % 15 == 0 || m_fpsLiveTick <= 2))
				m_fpsLive->setText(L("实时 %1 Hz", "Live %1 Hz").arg(m_fpsEma, 0, 'f', 1));
		}
		m_lastFrameMs = nowMs;
	}

	// Spot rotation, two root fixes:
	// (1) Auto-detected angle now uses 50%-of-peak gating (previously it followed the UI threshold, default 10%).
	//     On a measured frame, 10% gating let the entire diffuse halo into the mask; the near-circular halo
	//     (ell=0.976) dominated the second moments and produced a false major axis at orient=23°, while the true
	//     core axis sits at 71~77° (the converging band across 20-90% gating) — rotating by the false angle moved
	//     67° instead of the ideal 17°, roughly double the needed correction. 50%
	//     gating is core-dominated and matches the visually perceived major axis.
	// (2) Custom-angle override (0.1° precision): checked = rotate by a fixed angle, skipping auto detection.
	// The sensor crop shrinks to the inscribed rectangle accordingly; from here on, analysis/views/readouts/
	// history/image-save all use the rotated frame (one consistent convention).
	// The three modes are mutually exclusive: custom angle = fixed-angle exclusive, realtime = auto-leveling,
	// one-shot = button-captured fixed angle (press again to cancel).
	Frame fr = f;
	double rotDeg = 0.0;                              // the rotation actually applied (used by the position inverse transform)
	double a = 0.0;
	bool doRot = false;
	if (m_rotCustom->isChecked()) {
		a = m_rotDeg->value();
		doRot = true;
	} else if (m_rotSpot->isChecked()) {
		// Performance: re-detect the auto-leveling angle every 8 frames and reuse the cache in between — the
		// beam azimuth is a slowly varying quantity, and the skipped full-frame analyzeBeam (≈10ms @1280×1024)
		// was the biggest single contributor to per-frame saturation at 30fps.
		if (!m_rotAngleValid || m_framesSeen - m_rotAngleFrame >= 8) {
			AnalysisResult r0 = analyzeBeam(f, m_view->roi(), bgValue(), 50.0);
			if (r0.valid) {
				double na = 90.0 - r0.orientationDeg;
				if (na > 90.0) na -= 180.0;
				if (na <= -90.0) na += 180.0;
				m_rotAngle = na; m_rotAngleValid = true; m_rotAngleFrame = m_framesSeen;
				if (qEnvironmentVariableIsSet("STEBEAM_ROT_DBG"))   // diagnostic probe: report the auto-detected angle (hook for on-machine "over-rotation" debugging)
					printf("[rot] auto orient=%.2f alpha=%.2f ell=%.3f d4x=%.1f d4y=%.1f\n",
					       r0.orientationDeg, na, r0.ellipticity, r0.d4sigmaX, r0.d4sigmaY);
				// Auto-leveling drift changes the acquisition size required by keep-size → throttled re-send
				// (1.5s interval prevents repeated stream stops during convergence; rotKeepCheck compares against
				// the applied size, so once the angle stabilizes it is a no-op and stops happening).
				if (m_rotKeepSize->isChecked() && m_clock.elapsed() - m_rotKeepLastApplyMs > 1500) {
					m_rotKeepLastApplyMs = m_clock.elapsed();
					rotKeepCheck();
				}
			}
		}
		if (m_rotAngleValid) {
			a = m_rotAngle;
			doRot = true;
		}
	} else if (m_rotOnceActive && m_rotOnceAngle != 0.0) {
		// One-shot spot rotation: keep rotating by the fixed angle computed when the button was pressed, no
		// per-frame recomputation — output size and origin stay constant, so the CSV matrix's row/column counts
		// stop fluctuating with beam jitter.
		a = m_rotOnceAngle;
		doRot = true;
	}
	if (doRot && a != 0.0) {
		// Keep-size tier = output exactly the configured size (acquisition was already enlarged to the bounding
		// box → zero black corners; when acquisition gets clamped by the sensor maximum the corner regions show
		// black = the honest presentation within the limit); unchecked = the old inscribed-rectangle crop.
		if (m_rotKeepSize->isChecked())
			fr = rotateFrameBilinear(f, a, m_roiW->value(), m_roiH->value());
		else
			fr = rotateFrameBilinear(f, a);
		rotDeg = a;
		// Rotated-frame origin passthrough: center-aligned crop/resample shifts the frame's top-left corner
		// coordinate on the full sensor accordingly (position readouts are unaffected — cx0/cy0 are converted
		// exactly via unrotate; this only affects the profile axes' display reference).
		fr.originX = f.originX + (f.w - fr.w) / 2;
		fr.originY = f.originY + (f.h - fr.h) / 2;
		fr.sensorW = f.sensorW; fr.sensorH = f.sensorH;
	}
	m_last = fr;
	m_lastRaw = fr;   // snapshot of the effective raw frame (m_last may be replaced by a background-subtracted frame; re-analysis paths use this)
	if (s_rotOnceAt > 0 && (m_framesSeen == s_rotOnceAt - 1 || m_framesSeen == s_rotOnceAt + 1 || m_framesSeen == s_rotOnceAt + 6))
		printf("[rotonce] f=%d fr=%dx%d once=%d a=%.2f rotSpot=%d\n",
		       m_framesSeen, fr.w, fr.h, m_rotOnceActive, m_rotOnceAngle, m_rotSpot->isChecked());

	// Range note = the effective frame's real geometry (after the rotation's inscribed-rect crop) — origin has
	// shifted with the crop, sensor still reports the full target area; the sensor settings themselves are untouched.
	if (fr.w != m_noteGeom[0] || fr.h != m_noteGeom[1] || fr.originX != m_noteGeom[2] ||
		fr.originY != m_noteGeom[3] || fr.sensorW != m_noteGeom[4] || fr.sensorH != m_noteGeom[5]) {
		const int sw = fr.sensorW > 0 ? fr.sensorW : fr.w, sh = fr.sensorH > 0 ? fr.sensorH : fr.h;
		m_noteGeom[0] = fr.w; m_noteGeom[1] = fr.h; m_noteGeom[2] = fr.originX;
		m_noteGeom[3] = fr.originY; m_noteGeom[4] = fr.sensorW; m_noteGeom[5] = fr.sensorH;
		updateRoiNote(fr.w, fr.h, fr.originX, fr.originY, sw, sh);
	}

	// Manual block detection (peak-threshold method): full-frame peak <50% of full scale (2048/4095 ADU) means
	// "beam blocked". Independent of analyzeBeam validity — with the beam properly blocked there is no spot and
	// analysis always reports invalid, so gating the detection on r.valid would leave the button stuck at "please
	// block" in self-contradiction; sampling every 3rd pixel is enough to hit the spot core (the beam is a
	// tens-of-pixels blob) and saves CPU.
	{
		quint16 maxAdu = 0;
		const auto& px = fr.px;
		for (size_t k = 0; k < px.size(); k += 3)
			if (px[k] > maxAdu) maxAdu = px[k];
		m_beamBlocked = maxAdu < 2048;
	}

	AnalysisResult r = analyzeBeam(fr, m_view->roi(), bgValue(), m_thr->value());
	if (!r.valid) {
		m_lastR = r;
		updateReadouts(r);
		updateBkgState();   // keep the block state current per frame (invalid frames must also release/gate the button)
		if (m_stack->currentIndex() == 0) m_view->setCentroid(0, 0, false);
		if (m_active) m_active->frameConsumed();   // backpressure ack: invalid frames must also release the producer side
		return;
	}
	// Map position quantities (centroid/peak) back to raw-frame coordinates — this fixed the diagonal streaks in
	// the track view: rotation pivots on the frame center, so when the centroid is off-center, angle changes sweep
	// the position along an arc inside the rotated frame. The crosshair and the 84% contour still use cx/cy
	// (self-consistent with the rotated display); shape quantities (widths/azimuth/ellipticity) stay in the rotated frame.
	r.cx0 = r.cx; r.cy0 = r.cy; r.peakX0 = r.peakX; r.peakY0 = r.peakY;
	if (rotDeg != 0.0) {
		unrotatePos(f, fr, rotDeg, r.cx0, r.cy0);
		double px0 = r.peakX, py0 = r.peakY;
		unrotatePos(f, fr, rotDeg, px0, py0);
		r.peakX0 = (int)std::lround(px0); r.peakY0 = (int)std::lround(py0);
	}
	// Convert positions to full-sensor coordinates (software-wide bottom-left origin): raw-frame coordinates go
	// through frameToTarget — translate + flip y about the sensor's bottom edge. After a sensor crop, the beam's
	// physical position must not move with the window; readouts and the track view share this convention.
	frameToTarget(f, r.cx0, r.cy0, r.cx0, r.cy0);
	{
		double px = r.peakX0, py = r.peakY0;
		frameToTarget(f, px, py, px, py);
		r.peakX0 = (int)std::lround(px); r.peakY0 = (int)std::lround(py);
	}
	m_lastR = r;
	updateReadouts(r);
	// "Subtract background from raw intensity data": when checked, the display frames (2D/3D/replay/CSV/RAW save)
	// subtract the effective floor directly, clamping negatives to 0; analysis still runs on the raw frame (the
	// analyzer already subtracts the same floor internally — prevents double subtraction), and m_lastRaw keeps the
	// raw frame for re-analysis.
	if (m_rawBgSub->isChecked()) {
		Frame d = fr;
		const uint16_t bg = (uint16_t)std::lround(r.backgroundUsed);
		if (bg > 0)
			for (auto& p : d.px)
				p = p > bg ? uint16_t(p - bg) : uint16_t(0);
		m_last = d;
	}
	updateBkgState();   // with the shutter absent, the button gate tracks the block state per frame
	m_l84cx = r.cx; m_l84cy = r.cy; m_l84wx = r.d84X; m_l84wy = r.d84Y;
	// After auto-leveling the major axis is already vertical (no contour compensation rotation); in custom-angle
	// mode it need not be vertical, so the contour follows the rotated frame's measured azimuth. In non-leveling
	// modes the azimuth comes from an independent 50%-peak-gated analysis: r.orientationDeg is gated by the UI
	// threshold (default 10%), and the halo drags the principal axis into a false angle (the same failure
	// mechanism as auto-leveling's over-rotation), tilting the contour with it; 50% gating is core-dominated and
	// matches the visual major axis (same convention as auto-leveling). Only computed when the contour display is
	// on, saving CPU.
	m_l84rot = 0.0;
	if (m_show84->isChecked() && !m_rotSpot->isChecked()) {   // mode exclusivity: auto-leveling owns the realtime checkbox (axis already vertical, no compensation); custom/one-shot both follow the measured azimuth
		// Performance: recompute the contour azimuth every 8 frames, reuse the cache in between (slow variable; saves a second full-frame 50%-gated analysis)
		if (!m_l84rotValid || m_framesSeen - m_l84rotFrame >= 8) {
			AnalysisResult r50 = analyzeBeam(fr, m_view->roi(), bgValue(), 50.0);
			m_l84rotAngle = r50.valid ? r50.orientationDeg : r.orientationDeg;
			m_l84rotValid = true; m_l84rotFrame = m_framesSeen;
		}
		m_l84rot = m_l84rotValid ? m_l84rotAngle : r.orientationDeg;
	}

	// Stats always record (regardless of the visible page); the power queue stores the background-subtracted effective value (same convention as the Home readouts)
	pushStat(m_statPowerQ, effPower(r));
	pushStat(m_statWxQ, r.d4sigmaX);
	pushStat(m_statWyQ, r.d4sigmaY);
	double s = sizeScale();
	QString su = sizeUnit();
	QString pUnitTxt = "ADU";
	double pScale = 1.0;
	if (m_powUnit->currentIndex() != 0 && !m_statPowerQ.empty()) {   // unit column adapts its tier to the mean's magnitude (same criterion as fmtPower)
		double mean = std::accumulate(m_statPowerQ.begin(), m_statPowerQ.end(), 0.0) / m_statPowerQ.size();
		int i = powPickIdx(mean * powWPerAdu());
		pUnitTxt = kPowUnit[i];
		pScale = powWPerAdu() / kPowScale[i];
	}
	// ADU tier = 1 decimal, µW/mW/W tiers = 3 (software-wide); the prefix and the two value lines sit in separate cells so min/max align with the mean
	const QStringList stPow = statLines(m_statPowerQ, pScale, m_powUnit->currentIndex() == 0 ? 1 : 3, pUnitTxt);
	const QStringList stWx = statLines(m_statWxQ, s, m_sizeUnit->currentIndex() == 0 ? 1 : 3, su);
	const QStringList stWy = statLines(m_statWyQ, s, m_sizeUnit->currentIndex() == 0 ? 1 : 3, su);
	m_statPowLbl->setText(L("功率:", "Power:"));
	m_statPower->setText(stPow[0]);
	m_statPower2->setText(stPow[1]);
	m_statWxLbl->setText("D4σx:");
	m_statWidthX->setText(stWx[0]);
	m_statWidthX2->setText(stWx[1]);
	m_statWyLbl->setText("D4σy:");
	m_statWidthY->setText(stWy[0]);
	m_statWidthY2->setText(stWy[1]);

	// Size history always records (switching back loses nothing); the chart is only fed incrementally while its page is visible (mutually-exclusive feeding; onViewPage rebuilds in full on switch-back)
	int page = m_stack->currentIndex();
	double t = m_clock.elapsed() / 1000.0;
	m_sizeHist.push_back({t, r.d84X, r.d84Y});
	size_t nPrune = 0;
	while (!m_sizeHist.empty() && t - m_sizeHist.front()[0] > m_winSec) {
		m_sizeHist.pop_front();
		++nPrune;
	}
	if (m_sizeHist.size() > 4000) {   // over-cap exponential thinning (60min@44fps ≈160k points → ≤4000 to keep rendering smooth)
		std::deque<std::array<double, 3>> th;
		for (size_t i = 0; i < m_sizeHist.size(); i += 2) th.push_back(m_sizeHist[i]);
		if (th.empty() || th.back()[0] != m_sizeHist.back()[0]) th.push_back(m_sizeHist.back());
		m_sizeHist.swap(th);
		if (page == 3) rebuildSizeChart();   // Beam Size page
	} else if (page == 3) {
		// The self-drawn LineChartView has no incremental append API — m_sizeHist is already a synced sliding
		// window, so just rebuild in full (≤4000 points across two polylines; negligible cost even at 44fps; the
		// old QLineSeries incremental window path went away with QtCharts).
		rebuildSizeChart();
	}

	// Track history always records (≈2000 points, full-sensor px, bottom-left origin y up — cx0/cy0 avoid the
	// rotation sweep; frameToTarget unifies the coordinate system so TrackView's vertical axis points up; each
	// point carries the capture time in ms for CSV export of X/Y/timestamp). While the track page is visible, only
	// a repaint is triggered (TrackView::paint pulls the full history and converts with the current scale).
	m_trackHist.push_back(TrackPoint{ r.cx0, r.cy0, fr.captureMs });
	if (m_trackHist.size() > 2000) m_trackHist.pop_front();
	if (page == 4) m_track->update();   // Centroid Track page

	// Replay buffer always records (cap = the replay spin, 0 disables the feature; stores the display frame + its
	// analysis; oldest evicted past the cap; in live mode the slider/position-box ranges grow with the record —
	// signal blockers prevent re-entering onReplaySel).
	if (m_rbMax->value() > 0) {
		m_rb.push_back({ m_last, r });   // stores the display frame (with raw-intensity subtraction checked, that is the subtracted frame — same convention for replay display and CSV save)
		while ((int)m_rb.size() > m_rbMax->value()) m_rb.pop_front();
		if (m_rbSel >= (int)m_rb.size()) m_rbSel = -1;   // buffer trimmed below the selection → back to live
		if (m_rbSel < 0) {
			QSignalBlocker b1(m_rbSl), b2(m_rbSpin);
			m_rbSl->setRange(0, (int)m_rb.size());
			m_rbSl->setValue((int)m_rb.size());
			m_rbSpin->setRange(0, (int)m_rb.size());
		}
	}

	// Realtime CSV: when checked, every N frames hand the live frame to the async writer (unaffected by replay; frames dropped while the worker is busy are counted = backpressure)
	if (m_csvBtn->isChecked() && m_framesSeen % std::max(1, m_csvEvery->value()) == 0) {
		m_csv.enqueue(m_last, csvHeader2d(r));   // each frame carries the English metadata header block (computed on the GUI thread; the worker has no GUI state)
		m_csvLbl->setText(L("已存 %1", "Saved %1").arg(m_csv.saved()));
	}

	// Mutually-exclusive view feeding: 2D and 3D each compute only while their page is visible, stopping on switch-away;
	// during replay the display freezes on the selected frame and no live frames are fed, while stats / size chart /
	// centroid track / realtime CSV stay live.
	if (m_rbSel < 0) {
		switch (page) {
		case 0:
			m_view->setFrame(m_last);   // feed the display frame (with raw-intensity subtraction checked, the subtracted one)
			m_view->setCentroid(r.cx, r.cy, m_showCross->isChecked());
			m_view->setBeam84(r.cx, r.cy, r.d84X, r.d84Y, m_l84rot, m_shape84->currentIndex(), m_show84->isChecked());
			break;
		case 1: m_3d->setFrame(m_last); break;   // 3D surface (same display frame)
		case 2: updateProfileChart(r); break;
		default: break;   // the pages 3/4 charts/scatter were handled above
		}
		if (m_tsLbl && fr.captureMs > 0)   // bottom-right capture timestamp (refreshed per frame in live mode)
			m_tsLbl->setText(QDateTime::fromMSecsSinceEpoch(fr.captureMs).toString("yyyy-MM-dd HH:mm:ss.zzz"));
	}

	if (m_active) m_active->frameConsumed();   // backpressure ack: this frame is fully consumed, release the producer to keep delivering

	if (qEnvironmentVariableIsSet("STEBEAM_ONFRAME_DBG")) {   // diagnostic: onFrame average duration + count (reported every 10 frames)
		s_dbgTotal += s_dbgClock.nsecsElapsed() - t0;
		if (++s_dbgN % 10 == 0)
			printf("[onframe] avg=%.1fms n=%lld t=%lldms\n", s_dbgTotal / (double)s_dbgN / 1e6,
			       (long long)s_dbgN, s_dbgClock.elapsed()), fflush(stdout);
	}
}

// 2D intensity CSV metadata header block (same template for static and realtime CSV save). Regardless of UI
// language, stored text is always English to keep the template consistent; units appear only on line 1 and every
// size/power value afterwards uses line 1's unit (power = the tier picked by power magnitude, then fixed for the
// whole file, same criterion as the Home readouts); the data matrix = the display frame (with raw-intensity
// subtraction checked, the effective floor is already removed).
QString MainWindow::csvHeader2d(const AnalysisResult& r) const
{
	const QString su = sizeUnit();
	const int sd = m_sizeUnit->currentIndex() == 0 ? 1 : 3;
	QString pu = "ADU";
	double pScale = 1.0;
	int pDec = 1;
	if (m_powUnit->currentIndex() != 0 && powWPerAdu() > 0) {
		const int i = powPickIdx(effPower(r) * powWPerAdu());
		pu = QString::fromUtf8(kPowUnit[i]);
		pScale = powWPerAdu() / kPowScale[i];
		pDec = 3;
	}
	// "Unit Background Power shows 0" fix: the unit still matches line 1, but a nonzero value must never truncate
	// to 0 — fmtNoTrunc widens decimals by magnitude to keep 3 significant digits (0.000072 no longer prints as 0.000).
	auto pv = [&](double adu) { return fmtNoTrunc(adu * pScale, pDec); };   // ADU → line 1's power unit
	auto sv = [&](double px) { return fmtNoTrunc(px * sizeScale(), sd); };  // px → line 1's size unit
	static const char* kDefEn[] = { "D4sigma (ISO)", "84% Energy", "FWHM" };
	const int def = m_diamDef->currentIndex();
	const double dox = def == 0 ? r.d4sigmaX : def == 1 ? r.d84X : r.fwhmX;
	const double doy = def == 0 ? r.d4sigmaY : def == 1 ? r.d84Y : r.fwhmY;
	return QString("Size Unit,%1,Power Unit,%2,Beam Diameter Definition,%3\n")
		       .arg(su, pu, kDefEn[def]) +
	       QString("%1,Background Power,%2,Unit Background Power,%3,Calculation Threshold,%4\n")
		       .arg(m_rawBgSub->isChecked() ? "Background Subtracted" : "Background Not Subtracted",
		            m_bkgAdu >= 0 ? pv(m_bkgAdu) : QStringLiteral("N/A"),
		            pv(r.backgroundUsed), QString::number(m_thr->value(), 'f', 1)) +
	       QString("Power,%1,Peak Intensity,%2\n").arg(pv(effPower(r)), pv(r.peak)) +
	       QString("d_x,%1,d_y,%2\n").arg(sv(dox), sv(doy)) +
	       QString("Pixel Size (um/px),%1,Wavelength (nm),%2,QE (%),%3,Conversion Gain (e-/ADU),%4,Calibration Coefficient,%5\n")
		       .arg(m_pixSize->value(), 0, 'f', 3).arg(m_waveNm->value(), 0, 'f', 1)
		       .arg(m_qePct->value(), 0, 'f', 1).arg(m_ePerAdu->value(), 0, 'f', 3)
		       .arg(m_calCoef->value(), 0, 'f', 6) +
	       QString("Exposure (ms),%1,Gain (dB),%2,Frame Rate (Hz),%3,X Size (px),%4,Y Size (px),%5\n")
		       .arg(m_exp->value(), 0, 'f', 1).arg(m_gain->value(), 0, 'f', 1)
		       .arg(m_fps->value(), 0, 'f', 1)
		       .arg(m_appliedRoiW > 0 ? m_appliedRoiW : m_roiW->value())
		       .arg(m_appliedRoiH > 0 ? m_appliedRoiH : m_roiH->value());   // X/Y Size = the actually-sent acquisition size (keep-size tier = the angle-enlarged value; falls back to the configured value if never sent)
}

// Each save exports the current view's data; the default file name = the view title.
// PNG = a screenshot of the current view (what you see is what you get); numeric data goes to CSV/RAW.
void MainWindow::savePng()
{
	QWidget* cur = m_stack->currentWidget();
	if (!cur) return;
	QString fn = QFileDialog::getSaveFileName(this, L("保存 PNG", "Save PNG"), saveDir() + "/" + viewTitle() + ".png", "PNG (*.png)");   // default lands in "Documents\SteBeam"
	if (fn.isEmpty()) return;
	cur->grab().save(fn);
}

void MainWindow::saveCsv()
{
	const int page = m_stack->currentIndex();
	const bool replay = m_rbSel >= 0 && m_rbSel < (int)m_rb.size();
	const Frame fr = replay ? m_rb[m_rbSel].fr : m_last;
	if (!fr.valid()) return;
	QString fn = QFileDialog::getSaveFileName(this, L("保存 CSV", "Save CSV"), saveDir() + "/" + viewTitle() + ".csv", "CSV (*.csv)");   // default lands in "Documents\SteBeam"
	if (fn.isEmpty()) return;
	QFile f(fn);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
	QTextStream ts(&f);
	switch (page) {
	case 0:   // 2D intensity: metadata header block (English, 6 lines, same template as realtime CSV) + the 2D matrix, one pixel row per line
		ts << csvHeader2d(replay ? m_rb[m_rbSel].r : m_lastR);
		for (int y = 0; y < fr.h; ++y) {
			for (int x = 0; x < fr.w; ++x) {
				if (x) ts << ',';
				ts << fr.px[(size_t)y * fr.w + x];
			}
			ts << '\n';
		}
		break;
	case 1: {   // 3D surface: the downsampled bin-mean grid the view actually renders (row-major, row stride = gridW)
		if (!m_3d->hasGrid()) return;
		const int gw = m_3d->gridW(), gh = m_3d->gridH();
		const std::vector<float>& g = m_3d->gridAdu();
		ts << "# 3D surface grid, " << gw << " x " << gh << " (bin-mean ADU)\n";
		for (int y = 0; y < gh; ++y) {
			for (int x = 0; x < gw; ++x) {
				if (x) ts << ',';
				ts << g[(size_t)y * gw + x];
			}
			ts << '\n';
		}
		break;
	}
	case 2: {   // Beam profile: X/Y profile curves (raw ADU, unconverted)
		const AnalysisResult& r = replay ? m_rb[m_rbSel].r : m_lastR;
		ts << "index,profileX,profileY\n";
		size_t n = std::max(r.profileX.size(), r.profileY.size());
		for (size_t i = 0; i < n; ++i)
			ts << i << ','
			   << (i < r.profileX.size() ? r.profileX[i] : 0) << ','
			   << (i < r.profileY.size() ? r.profileY[i] : 0) << '\n';
		break;
	}
	case 3: {   // Beam size: curve data for the current acquisition window (t minutes + 84% widths X/Y, converted by the current size unit)
		const double s = sizeScale();
		ts << "t_min,d84X_" << sizeUnit() << ",d84Y_" << sizeUnit() << "\n";
		for (const auto& e : m_sizeHist)
			ts << e[0] / 60.0 << ',' << e[1] * s << ',' << e[2] * s << '\n';
		break;
	}
	default: {   // Centroid track: every plotted point's X/Y (full-sensor px, bottom-left origin) + capture timestamp
		ts << "x_px,y_px,timestamp\n";
		for (const TrackPoint& p : m_trackHist)
			ts << p.x << ',' << p.y << ','
			   << QDateTime::fromMSecsSinceEpoch(p.ms).toString("yyyy-MM-dd HH:mm:ss.zzz") << '\n';
		break;
	}
	}
}

void MainWindow::saveRaw()
{
	const bool replay = m_rbSel >= 0 && m_rbSel < (int)m_rb.size();
	const Frame fr = replay ? m_rb[m_rbSel].fr : m_last;   // display frame (during replay, the selected frame is what gets saved)
	if (!fr.valid()) return;
	QString fn = QFileDialog::getSaveFileName(this, L("保存 RAW(16bit)", "Save RAW (16bit)"), saveDir() + "/" + viewTitle() + ".raw", "RAW (*.raw)");   // default lands in "Documents\SteBeam"
	if (fn.isEmpty()) return;
	QFile f(fn);
	if (!f.open(QIODevice::WriteOnly)) return;
	f.write((const char*)fr.px.data(), (qint64)fr.px.size() * 2);
}

// Open-source compliance (GPL-3.0 §5 "Appropriate Legal Notices" shipped with LGPL-3.0): copyright line + free-software
// statement + license and repository pointers + no-warranty disclaimer + Qt source link + third-party notices pointer;
// the LGPL shield (:/logo/lgplv3-88x31.png, GNU public image) serves as the About-box icon = license badge.
void MainWindow::showAbout()
{
	QMessageBox box(this);
	box.setWindowTitle(L("关于 SteBeam", "About SteBeam"));
	box.setIconPixmap(QPixmap(":/logo/lgplv3-88x31.png"));
	const QString body = g_cn
	    ? "<b>SteBeam</b> V0.1<br>"
	      "Copyright © 2026 Stefano's AI Lab<br><br>"
	      "本程序是自由软件，您可以根据自由软件基金会发布的 GNU 宽通用公共许可证"
	      "（LGPL-3.0-or-later）的条款重新分发和/或修改它；"
	      "完整源代码仓库：<a href=\"https://github.com/stefanohe/stebeam\">github.com/stefanohe/stebeam</a>。"
	      "本程序按「现状」提供，不作任何担保，也不对适销性或针对特定用途的适用性"
	      "作任何暗示性担保；测量结果不构成计量认证依据。<br><br>"
	      "本程序使用的 Qt 6 库以 GNU 宽通用公共许可证（LGPL-3.0）合规方式随附，Qt 完整源码见 "
	      "<a href=\"https://download.qt.io/archive/qt/6.11/\">download.qt.io/archive/qt/6.11</a>；"
	      "Qt 库以独立 DLL 随附，可被您自行构建的库替换，本协议不限制您为自身用途修改 "
	      "Qt 库及为此调试而反向工程的权利。<br><br>"
	      "第三方组件许可与归属文本：见安装目录 LICENSES\\ 文件夹（索引 THIRD-PARTY-NOTICES.md）。"
	    : "<b>SteBeam</b> V0.1<br>"
	      "Copyright © 2026 Stefano's AI Lab<br><br>"
	      "This program is free software: you can redistribute it and/or modify it under the terms of "
	      "the GNU Lesser General Public License (LGPL-3.0-or-later) as published by the Free Software "
	      "Foundation; complete source repository: "
	      "<a href=\"https://github.com/stefanohe/stebeam\">github.com/stefanohe/stebeam</a>. "
	      "It is provided \"AS IS\", without warranty of any kind; "
	      "measurement output is not a metrological certification.<br><br>"
	      "The Qt 6 libraries used by this program are bundled in compliance with the "
	      "GNU Lesser General Public License (LGPL-3.0); "
	      "complete Qt source: <a href=\"https://download.qt.io/archive/qt/6.11/\">download.qt.io/archive/qt/6.11</a>. "
	      "The Qt libraries ship as separate DLLs and may be replaced with your own builds; "
	      "this program's license does not restrict your right to modify the Qt libraries for "
	      "your own use, and reverse engineering for debugging such modifications is permitted.<br><br>"
	      "Third-party component licenses and attributions: see the LICENSES\\ folder in the installation directory (index: THIRD-PARTY-NOTICES.md).";
	box.setText(body);
	// Compliance probe: with STEBEAM_ABOUT=<png path>, auto-open the About box → save a screenshot once rendering settles → exit (verify once per language)
	if (qEnvironmentVariableIsSet("STEBEAM_ABOUT")) {
		box.show();
		QTimer::singleShot(800, this, [this, &box] {
			box.grab().save(qEnvironmentVariable("STEBEAM_ABOUT"));
			qApp->quit();
		});
	}
	box.exec();
}

// Headless camera-switch reproduction: toggle() fires toggled twice per Qt semantics (old button false + new
// button true), the same path as a real click; report the frame count after 5 seconds — a count near zero
// reproduces the freeze.
void MainWindow::selfCamTest()
{
	QTimer::singleShot(1000, this, [this] {
		printf("[selfcam] toggle -> camera\n"); fflush(stdout);
		m_radioCam->toggle();
	});
	QTimer::singleShot(6000, this, [this] {
		printf("[selfcam] frames=%d (sim ~30 in 1s is normal; >50 five seconds after switching to the camera)\n", m_framesSeen); fflush(stdout);
		QCoreApplication::quit();
	});
}

// Headless five-view selftest: switch pages on a timer, grab the current view and count non-background pixels —
// the effectiveness readback criterion for mutually-exclusive feeding and the 3D self-drawing (every page should
// have content; nonBg=0 means a blank-render incident). Known offscreen-platform artifact when reading grabs:
// text not drawn by QLabel (self-drawn / stylesheet-painted) renders as tofu boxes (ASCII digits included), while
// QLabel text is fine — real machines show no such artifact, so grabs are judged only on graphic elements and
// text judgment defers to real-machine screenshots (verified).
void MainWindow::selfViewsTest()
{
	auto page = std::make_shared<int>(0);
	auto tick = std::make_shared<std::function<void()>>();
	*tick = [this, page, tick] {
		int p = *page;
		if (p >= 5) {
			printf("[selfviews] done\n"); fflush(stdout);
			QCoreApplication::quit();
			return;
		}
		onViewPage(p);
		// STEBEAM_VIEWS_DELAY=milliseconds → wait for fed frames after the page switch before grabbing (the self-drawn chart pages need an arriving frame to have judgeable content; default 0 = immediate grab as before)
		const int grabDelay = qEnvironmentVariableIntValue("STEBEAM_VIEWS_DELAY");
		auto grabAndNext = [this, tick, page, p] {
		if (p == 1 && qEnvironmentVariableIsSet("STEBEAM_3D_ANG")) {   // 3D rotation probe: STEBEAM_3D_ANG="yaw,pitch" sets the viewing angles before the grab (for judging whether the spot is hidden at overlapping angles)
			QString a = QString::fromLocal8Bit(qgetenv("STEBEAM_3D_ANG"));
			if (qEnvironmentVariableIsSet("STEBEAM_3D_Q"))      // quality-tier probe (0 standard / 1 high / 2 ultra) and smooth-tier probe (must be set before RAW injection — setQuality invalidates the old grid and waits for the next frame)
				m_3d->setQuality(qEnvironmentVariable("STEBEAM_3D_Q").toInt());
			if (qEnvironmentVariableIsSet("STEBEAM_3D_SMOOTH"))
				m_3d->setSmooth(qEnvironmentVariable("STEBEAM_3D_SMOOTH").toInt() != 0);
			m_3d->setAngles(a.section(',', 0, 0).toDouble(), a.section(',', 1, 1).toDouble());
			if (qEnvironmentVariableIsSet("STEBEAM_3D_RAW")) {   // real-machine RAW frame injection (1280×1024 16-bit row-major, fed synchronously before the grab to override the simulated frame)
				QFile rf(QString::fromLocal8Bit(qgetenv("STEBEAM_3D_RAW")));
				if (rf.open(QIODevice::ReadOnly)) {
					Frame f; f.w = 1280; f.h = 1024; f.px.resize((size_t)f.w * f.h);
					if (rf.read((char*)f.px.data(), (qint64)f.px.size() * 2) == (qint64)f.px.size() * 2)
						m_3d->setFrame(f);
				}
			}
		}
		QImage img = m_stack->widget(p)->grab().toImage().convertToFormat(QImage::Format_RGB32);
		int nonBg = 0;
		const QRgb bg = qRgb(0x22, 0x22, 0x26);
		for (int y = 0; y < img.height(); y += 3) {
			const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
			for (int x = 0; x < img.width(); x += 3)
				if (line[x] != bg) ++nonBg;
		}
		printf("[selfviews] page=%d nonBg=%d (%dx%d)\n", p, nonBg, img.width(), img.height()); fflush(stdout);
		if (qEnvironmentVariableIsSet("STEBEAM_DUMP")) {   // one-off debug: per-page grabs + whole-window grab (for the banner/title-bar regions) saved to the system temp dir for human reading
			img.save(QDir::tempPath() + QString("/selfviews_p%1.png").arg(p));
			if (p == 0) {
				if (qEnvironmentVariableIsSet("STEBEAM_TAB"))   // switch the right-panel tab (0/1/2) before the whole-window grab — judging the Setup axis-group horizontal scrollbar requires the Setup page's full window
					m_tabs->setCurrentIndex(qEnvironmentVariable("STEBEAM_TAB").toInt());
				if (qEnvironmentVariableIsSet("STEBEAM_PANEL_DBG")) {   // horizontal-scrollbar effectiveness criterion (hbarMax>0 = horizontal scroll room remains)
					for (int t = 0; t < m_tabs->count(); ++t) {
						auto* sa = qobject_cast<QScrollArea*>(m_tabs->widget(t));
						printf("[panel] tab=%d panelW=%d viewport=%d contentMin=%d hbarMax=%d\n",
						       t, m_tabs->parentWidget()->width(), sa->viewport()->width(),
						       sa->widget()->minimumSizeHint().width(), sa->horizontalScrollBar()->maximum());
					}
					// Parameter-box width diagnostics (for the "acquisition parameter boxes and the X-size box differ in length" complaint): natural width / real width / min / max
					printf("[panel] boxes m_exp nat=%d w=%d min=%d max=%d | m_roiW nat=%d w=%d min=%d max=%d | m_bkgSec w=%d\n",
					       m_exp->sizeHint().width(), m_exp->width(), m_exp->minimumWidth(), m_exp->maximumWidth(),
					       m_roiW->sizeHint().width(), m_roiW->width(), m_roiW->minimumWidth(), m_roiW->maximumWidth(),
					       m_bkgSec->width());
					// Added for localizing the Win11 "calibration boxes shorter than the threshold box": the five calibration boxes' natural/real widths vs the threshold box's real width
					printf("[panel] calboxes thr w=%d | pixSize nat=%d w=%d | wave nat=%d w=%d | qe nat=%d w=%d | ePerAdu nat=%d w=%d | coef nat=%d w=%d\n",
					       m_thr->width(),
					       m_pixSize->sizeHint().width(), m_pixSize->width(),
					       m_waveNm->sizeHint().width(), m_waveNm->width(),
					       m_qePct->sizeHint().width(), m_qePct->width(),
					       m_ePerAdu->sizeHint().width(), m_ePerAdu->width(),
					       m_calCoef->sizeHint().width(), m_calCoef->width());
					// Name-column fixed width + one-shot button right-edge alignment effectiveness criterion (the two `right` values should be equal; for locating h-scroll/alignment regressions)
					printf("[panel] col0W=%d\n", m_col0WDbg);
					printf("[panel] rotOnce x=%d w=%d max=%d right=%d | shape84 x=%d w=%d right=%d\n",
					       m_rotOnce->geometry().x(), m_rotOnce->width(), m_rotOnce->maximumWidth(), m_rotOnce->geometry().right(),
					       m_shape84->geometry().x(), m_shape84->width(), m_shape84->geometry().right());
				}
				grab().toImage().save(QDir::tempPath() + "/selfviews_win.png");
			}
		}
		*page = p + 1;
		QTimer::singleShot(150, this, [tick] { (*tick)(); });
		};   // end of grabAndNext
		if (grabDelay > 0) QTimer::singleShot(grabDelay, this, grabAndNext);
		else grabAndNext();   // default: immediate grab (original semantics)
	};
	QTimer::singleShot(500, this, [tick] { (*tick)(); });
}

// Realtime-CSV effectiveness probe: simulator + interval 1 frame; after 3 seconds stop recording and drain
// in-flight writes, then count the files landed in realtime_csv and read back the newest file's line count and
// first matrix row's comma count to verify matrix integrity (effectiveness criterion = artifact count + content
// readback; completion notifications/counters are not the sole criterion. Draining: after unchecking, the worker
// has at most 1 frame still writing — wait before reading, otherwise you read a half-written file).
void MainWindow::selfCsvTest()
{
	const QString dir = dataDir() + "/realtime_csv";   // dataDir() convention (on the dev machine = the exe directory, unchanged)
	m_csv.setDir(dir);   // the probe always writes the default directory (users may customize the directory; the probe must not land on user disks)
	for (const QString& old : QDir(dir).entryList(QDir::Files)) QFile::remove(dir + "/" + old);
	m_csvEvery->setValue(qEnvironmentVariableIsSet("STEBEAM_CSV_EVERY")
	                     ? qEnvironmentVariable("STEBEAM_CSV_EVERY").toInt() : 1);   // probe parameter: save every frame by default, env can change the interval
	m_csvBtn->setChecked(true);   // same path as a user click (toggled wiring)
	QTimer::singleShot(3000, this, [this, dir] {
		m_csvBtn->setChecked(false);
		QTimer::singleShot(1200, this, [this, dir] {   // drain wait: let in-flight frames finish writing before counting/reading back
			const QStringList files = QDir(dir).entryList(QDir::Files, QDir::Time);
			printf("[selfcsv] frames=%d saved=%d dropped=%d files=%d\n", m_framesSeen, m_csv.saved(),
			       m_csv.dropped(), (int)files.size());
			fflush(stdout);
			if (!files.isEmpty()) {
				QFile f(dir + "/" + files.first());
				if (f.open(QIODevice::ReadOnly)) {
					qint64 rows = 0;
					while (!f.atEnd()) { f.readLine(); ++rows; }   // argument-less readLine reads a whole line (canReadLine has a 4KB false-negative)
					f.seek(0);
					QByteArray matLine;   // file header = the English metadata block of 6 lines, so the matrix's first row is line 7 (criterion reads line 7's comma count)
					for (int i = 0; i < 7 && !f.atEnd(); ++i) matLine = f.readLine();
					printf("[selfcsv] first=%s rows=%lld matrow7commas=%d (want rows=1030 matrow7commas=1279)\n",
					       files.first().toLocal8Bit().constData(), (long long)rows, matLine.count(','));
					fflush(stdout);
				}
			}
			QCoreApplication::quit();
		});
	});
}
