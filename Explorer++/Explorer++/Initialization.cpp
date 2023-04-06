// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "Explorer++.h"
#include "Bookmarks/UI/BookmarksMainMenu.h"
#include "Config.h"
#include "DarkModeHelper.h"
#include "DisplayWindow/DisplayWindow.h"
#include "Explorer++_internal.h"
#include "LoadSaveInterface.h"
#include "MainResource.h"
#include "MainToolbar.h"
#include "MainWindow.h"
#include "MenuHelper.h"
#include "MenuRanges.h"
#include "ResourceHelper.h"
#include "ShellBrowser/ShellBrowser.h"
#include "ShellBrowser/ViewModes.h"
#include "Tab.h"
#include "TabContainer.h"
#include "TaskbarThumbnails.h"
#include "UiTheming.h"
#include "ViewModeHelper.h"
#include "../Helper/CustomGripper.h"
#include "../Helper/iDirectoryMonitor.h"

/*
 * Main window creation.
 *
 * Settings are loaded very early on. Any initial settings must be in place before this.
 */
void Explorerplusplus::OnCreate()
{
	InitializeMainToolbars();

	ILoadSave *pLoadSave = nullptr;
	LoadAllSettings(&pLoadSave);
	ApplyToolbarSettings();

	m_config->shellChangeNotificationType = m_commandLineSettings.shellChangeNotificationType;

	m_iconResourceLoader = std::make_unique<IconResourceLoader>(m_config->iconSet);

	SetLanguageModule();

	if (ShouldEnableDarkMode(m_config->theme))
	{
		SetUpDarkMode();
	}

	m_bookmarksMainMenu = std::make_unique<BookmarksMainMenu>(this, this, &m_bookmarkIconFetcher,
		&m_bookmarkTree, MenuIdRange{ MENU_BOOKMARK_STARTID, MENU_BOOKMARK_ENDID });

	m_mainWindow = MainWindow::Create(m_hContainer, m_config, m_resourceModule, this);

	InitializeMainMenu();

	CreateDirectoryMonitor(&m_pDirMon);

	CreateStatusBar();
	CreateMainControls();
	InitializeDisplayWindow();
	InitializeTabs();
	CreateFolderControls();

	/* All child windows MUST be resized before
	any listview changes take place. If auto arrange
	is turned off in the listview, when it is
	initially sized, all current items will lock
	to the current width. The only was to unlock
	them from this width is to turn auto arrange back on.
	Therefore, the listview MUST be set to the correct
	size initially. */
	ResizeWindows();

	m_taskbarThumbnails =
		TaskbarThumbnails::Create(this, m_tabContainer, m_resourceModule, m_config);

	RestoreTabs(pLoadSave);
	delete pLoadSave;

	// Register for any shell changes. This should be done after the tabs have
	// been created.
	SHChangeNotifyEntry shcne;
	shcne.fRecursive = TRUE;
	shcne.pidl = nullptr;
	m_SHChangeNotifyID = SHChangeNotifyRegister(m_hContainer, SHCNRF_ShellLevel, SHCNE_ASSOCCHANGED,
		WM_APP_ASSOCCHANGED, 1, &shcne);

	SetFocus(m_hActiveListView);

	m_uiTheming = std::make_unique<UiTheming>(this, m_tabContainer);

	COLORREF gripperBackgroundColor;

	if (DarkModeHelper::GetInstance().IsDarkModeEnabled())
	{
		gripperBackgroundColor = DarkModeHelper::BACKGROUND_COLOR;
	}
	else
	{
		gripperBackgroundColor = GetSysColor(COLOR_WINDOW);
	}

	CustomGripper::Initialize(m_hContainer, gripperBackgroundColor);

	InitializePlugins();

	SetTimer(m_hContainer, AUTOSAVE_TIMER_ID, AUTOSAVE_TIMEOUT, nullptr);

	m_InitializationFinished.set(true);
}

void Explorerplusplus::InitializeDisplayWindow()
{
	DWInitialSettings_t initialSettings;
	initialSettings.CentreColor = m_config->displayWindowCentreColor;
	initialSettings.SurroundColor = m_config->displayWindowSurroundColor;
	initialSettings.TextColor = m_config->displayWindowTextColor;
	initialSettings.hFont = m_config->displayWindowFont;
	initialSettings.hIcon = (HICON) LoadImage(GetModuleHandle(nullptr),
		MAKEINTRESOURCE(IDI_DISPLAYWINDOW), IMAGE_ICON, 0, 0, LR_CREATEDIBSECTION);

	m_hDisplayWindow = CreateDisplayWindow(m_hContainer, &initialSettings);

	ApplyDisplayWindowPosition();
}

wil::unique_hmenu Explorerplusplus::BuildViewsMenu()
{
	wil::unique_hmenu viewsMenu(CreatePopupMenu());
	AddViewModesToMenu(viewsMenu.get(), 0, TRUE);

	const Tab &tab = m_tabContainer->GetSelectedTab();
	ViewMode currentViewMode = tab.GetShellBrowser()->GetViewMode();

	CheckMenuRadioItem(viewsMenu.get(), IDM_VIEW_THUMBNAILS, IDM_VIEW_EXTRALARGEICONS,
		GetViewModeMenuId(currentViewMode), MF_BYCOMMAND);

	return viewsMenu;
}

void Explorerplusplus::AddViewModesToMenu(HMENU menu, UINT startPosition, BOOL byPosition)
{
	UINT position = startPosition;

	for (auto viewMode : VIEW_MODES)
	{
		std::wstring text =
			ResourceHelper::LoadString(m_resourceModule, GetViewModeMenuStringId(viewMode));

		MENUITEMINFO itemInfo;
		itemInfo.cbSize = sizeof(itemInfo);
		itemInfo.fMask = MIIM_ID | MIIM_STRING;
		itemInfo.wID = GetViewModeMenuId(viewMode);
		itemInfo.dwTypeData = text.data();
		InsertMenuItem(menu, position, byPosition, &itemInfo);

		if (byPosition)
		{
			position++;
		}
	}
}

bool Explorerplusplus::ShouldEnableDarkMode(Theme theme)
{
	return theme == +Theme::Dark
		|| (theme == +Theme::System && DarkModeHelper::GetInstance().ShouldAppsUseDarkMode());
}

void Explorerplusplus::SetUpDarkMode()
{
	auto &darkModeHelper = DarkModeHelper::GetInstance();
	darkModeHelper.EnableForApp();

	if (!darkModeHelper.IsDarkModeEnabled())
	{
		return;
	}

	darkModeHelper.AllowDarkModeForWindow(m_hContainer, true);

	BOOL dark = TRUE;
	DarkModeHelper::WINDOWCOMPOSITIONATTRIBDATA compositionData = {
		DarkModeHelper::WCA_USEDARKMODECOLORS, &dark, sizeof(dark)
	};
	darkModeHelper.SetWindowCompositionAttribute(m_hContainer, &compositionData);
}
