﻿// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "Explorer++.h"
#include "Config.h"
#include "FolderView.h"
#include "IDropFilesCallback.h"
#include "MainResource.h"
#include "MainToolbar.h"
#include "NewMenuClient.h"
#include "ResourceHelper.h"
#include "ServiceProvider.h"
#include "SetFileAttributesDialog.h"
#include "ShellBrowser/Columns.h"
#include "ShellBrowser/ShellBrowserImpl.h"
#include "ShellBrowser/ShellNavigationController.h"
#include "ShellBrowser/ViewModes.h"
#include "ShellView.h"
#include "SortMenuBuilder.h"
#include "TabContainer.h"
#include "ViewModeHelper.h"
#include "../Helper/BulkClipboardWriter.h"
#include "../Helper/ClipboardHelper.h"
#include "../Helper/DropHandler.h"
#include "../Helper/Helper.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/Macros.h"
#include "../Helper/MenuHelper.h"
#include "../Helper/ShellContextMenu.h"
#include "../Helper/ShellHelper.h"
#include "../Helper/WinRTBaseWrapper.h"
#include <wil/com.h>
#include "../Helper/DpiCompatibility.h"

const std::map<UINT, Icon> LIST_VIEW_RIGHT_CLICK_MENU_IMAGE_MAPPINGS = {
	{ IDM_EDIT_CUT, Icon::Cut },
	{ IDM_EDIT_COPY, Icon::Copy },
	{ IDM_EDIT_PASTE, Icon::Paste },
	{ IDM_EDIT_PASTESHORTCUT, Icon::PasteShortcut },
	{ IDM_FILE_DELETE, Icon::Delete },
	{ IDM_FILE_RENAME, Icon::Rename },
	{ IDM_FILE_PROPERTIES, Icon::Properties },
	{ IDM_VIEW_REFRESH, Icon::Refresh },
	{ IDM_FILE_OPENCOMMANDPROMPT, Icon::CommandLine },
	{ IDM_ACTIONS_NEWFOLDER, Icon::NewFolder },
};

LRESULT CALLBACK Explorerplusplus::ListViewProcStub(HWND hwnd, UINT uMsg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);

	auto *pexpp = reinterpret_cast<Explorerplusplus *>(dwRefData);

	return pexpp->ListViewSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK Explorerplusplus::ListViewSubclassProc(HWND ListView, UINT msg, WPARAM wParam,
	LPARAM lParam)
{
	switch (msg)
	{
	case WM_MENUSELECT:
		SendMessage(m_hContainer, WM_MENUSELECT, wParam, lParam);
		break;

	case WM_SETFOCUS:
		FocusChanged();
		break;

	case WM_CONTEXTMENU:
		if (reinterpret_cast<HWND>(wParam)
			== GetActivePane()
				   ->GetTabContainer()
				   ->GetSelectedTab()
				   .GetShellBrowser()
				   ->GetListView())
		{
			OnShowListViewContextMenu({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
			return 0;
		}
		break;

	case WM_NOTIFY:
		switch (((LPNMHDR) lParam)->code)
		{
		case HDN_BEGINDRAG:
			return FALSE;

		case HDN_ENDDRAG:
		{
			/* When the drag ends, the dragged item
			is shifted into position, and all later
			items are shifted down one. Therefore,
			take the item out of its position in the
			list, and move it into its new position. */
			NMHEADER *pnmHeader = nullptr;
			Column_t column;
			int i = 0;

			pnmHeader = (NMHEADER *) lParam;

			auto currentColumns = m_pActiveShellBrowser->GetCurrentColumns();

			i = 0;
			auto itr = currentColumns.begin();
			while (i < (pnmHeader->iItem + 1) && itr != currentColumns.end())
			{
				if (itr->checked)
				{
					i++;
				}

				itr++;
			}

			if (itr != currentColumns.begin())
				itr--;

			column = *itr;
			currentColumns.erase(itr);

			i = 0;
			itr = currentColumns.begin();
			while (i < (pnmHeader->pitem->iOrder + 1) && itr != currentColumns.end())
			{
				if (itr->checked)
				{
					i++;
				}

				itr++;
			}

			if (itr != currentColumns.begin())
				itr--;

			currentColumns.insert(itr, column);

			m_pActiveShellBrowser->SetCurrentColumns(currentColumns);

			Tab &tab = GetActivePane()->GetTabContainer()->GetSelectedTab();
			tab.GetShellBrowser()->GetNavigationController()->Refresh();

			return TRUE;
		}
		}
		break;
	}

	return DefSubclassProc(ListView, msg, wParam, lParam);
}

LRESULT Explorerplusplus::OnListViewKeyDown(LPARAM lParam)
{
	LV_KEYDOWN *keyDown = reinterpret_cast<LV_KEYDOWN *>(lParam);

	switch (keyDown->wVKey)
	{
	case VK_RETURN:
		if (IsKeyDown(VK_MENU))
		{
			m_pActiveShellBrowser->ShowPropertiesForSelectedFiles();
		}
		else
		{
			OpenAllSelectedItems(
				DetermineOpenDisposition(false, IsKeyDown(VK_CONTROL), IsKeyDown(VK_SHIFT)));
		}
		break;

	case 'V':
		if (IsKeyDown(VK_CONTROL) && !IsKeyDown(VK_SHIFT) && !IsKeyDown(VK_MENU))
		{
			OnListViewPaste();
		}
		break;

	case VK_INSERT:
		if (!IsKeyDown(VK_CONTROL) && IsKeyDown(VK_SHIFT) && !IsKeyDown(VK_MENU))
		{
			OnListViewPaste();
		}
		break;
	}

	return 0;
}

int Explorerplusplus::DetermineListViewObjectIndex(HWND hListView)
{
	for (auto &item : GetActivePane()->GetTabContainer()->GetAllTabs())
	{
		if (item.second->GetShellBrowser()->GetListView() == hListView)
		{
			return item.first;
		}
	}

	return -1;
}

void Explorerplusplus::OnShowListViewContextMenu(const POINT &ptScreen)
{
	POINT finalPoint = ptScreen;

	bool keyboardGenerated = false;

	if (ptScreen.x == -1 && ptScreen.y == -1)
	{
		keyboardGenerated = true;
	}

	Tab &tab = GetActivePane()->GetTabContainer()->GetSelectedTab();

	if (ListView_GetSelectedCount(tab.GetShellBrowser()->GetListView()) == 0)
	{
		if (keyboardGenerated)
		{
			finalPoint = { 0, 0 };
			ClientToScreen(tab.GetShellBrowser()->GetListView(), &finalPoint);
		}

		OnListViewBackgroundRClick(&finalPoint);
	}
	else
	{
		if (keyboardGenerated)
		{
			int targetItem = ListView_GetNextItem(tab.GetShellBrowser()->GetListView(), -1,
				LVNI_FOCUSED | LVNI_SELECTED);

			if (targetItem == -1)
			{
				auto lastSelectedItem =
					ListViewHelper::GetLastSelectedItemIndex(tab.GetShellBrowser()->GetListView());
				targetItem = lastSelectedItem.value();
			}

			RECT itemRect;
			ListView_GetItemRect(tab.GetShellBrowser()->GetListView(), targetItem, &itemRect,
				LVIR_ICON);

			finalPoint = { itemRect.left + (itemRect.right - itemRect.left) / 2,
				itemRect.top + (itemRect.bottom - itemRect.top) / 2 };
			ClientToScreen(tab.GetShellBrowser()->GetListView(), &finalPoint);
		}

		OnListViewItemRClick(&finalPoint);
	}
}

void Explorerplusplus::OnListViewBackgroundRClick(POINT *pCursorPos)
{
	// Shift key pressed, show shell context menu
	if (IsKeyDown(VK_SHIFT))
    {
		const auto &selectedTab = GetActivePane()->GetTabContainer()->GetSelectedTab();
		auto pidlDirectory = selectedTab.GetShellBrowser()->GetDirectoryIdl();

		ShellContextMenu shellContextMenu(pidlDirectory.get(), {}, this, m_pStatusBar);

		auto serviceProvider = winrt::make_self<ServiceProvider>();

		auto newMenuClient = winrt::make<NewMenuClient>(selectedTab.GetShellBrowser());
		serviceProvider->RegisterService(IID_INewMenuClient, newMenuClient.get());

		winrt::com_ptr<IFolderView2> folderView =
			winrt::make<FolderView>(selectedTab.GetShellBrowserWeak());
		serviceProvider->RegisterService(IID_IFolderView, folderView.get());

		auto shellView = winrt::make<ShellView>(selectedTab.GetShellBrowserWeak(), this, false);
		serviceProvider->RegisterService(SID_DefView, shellView.get());

		ShellContextMenu::Flags flags = ShellContextMenu::Flags::Standard;

		if (IsKeyDown(VK_SHIFT))
		{
			WI_SetFlag(flags, ShellContextMenu::Flags::ExtendedVerbs);
		}

		shellContextMenu.ShowMenu(selectedTab.GetShellBrowser()->GetListView(), pCursorPos,
			serviceProvider.get(), flags);
	}
	// Show custom context menu
	else
    {
		auto parentMenu = wil::unique_hmenu(LoadMenu(m_resourceInstance, MAKEINTRESOURCE(IDR_LIST_BACKGROUND_MENU)));
		HMENU menu = GetSubMenu(parentMenu.get(), 0);
		// Set nume icons
		std::vector<wil::unique_hbitmap> menuImages;
		AddImagesToListViewContextMenu(menu, menuImages);
		MenuHelper::EnableItem(menu, IDM_EDIT_PASTE, CanPaste(PasteType::Normal));
		MenuHelper::EnableItem(menu, IDM_EDIT_PASTESHORTCUT, CanPaste(PasteType::Shortcut));
		const UINT command = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_VERTICAL | TPM_RETURNCMD,
				pCursorPos->x, pCursorPos->y, 0, m_hActiveListView, nullptr);
		SendMessage(GetCoreInterface()->GetMainWindow(), WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
}

void Explorerplusplus::OnListViewItemRClick(POINT *pCursorPos)
{
	int nSelected = ListView_GetSelectedCount(m_hActiveListView);
	if (nSelected > 0)
    {
		std::vector<unique_pidl_child> pidlPtrs;
		std::vector<PCITEMID_CHILD> pidlItems;
		BOOL bSeenDirectory = FALSE;
		int iItem = -1;
		std::vector<PCITEMID_CHILD> pidFolderItems;

		while ((iItem = ListView_GetNextItem(m_hActiveListView, iItem, LVNI_SELECTED)) != -1)
		{
			auto pidlPtr = m_pActiveShellBrowser->GetItemChildIdl(iItem);

			pidlItems.push_back(pidlPtr.get());
			pidlPtrs.push_back(std::move(pidlPtr));
			DWORD dwAttributes = m_pActiveShellBrowser->GetItemFileFindData(iItem).dwFileAttributes;
            if ((dwAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY)
            {
				bSeenDirectory = TRUE;
				pidFolderItems.push_back(pidlItems.back());
			}
		}

		auto pidlDirectory = m_pActiveShellBrowser->GetDirectoryIdl();

		// Shift key pressed, show shell context menu
		if (IsKeyDown(VK_SHIFT))
		{
			ShellContextMenu::Flags flags = ShellContextMenu::Flags::Rename;

			if (IsKeyDown(VK_SHIFT))
			{
				WI_SetFlag(flags, ShellContextMenu::Flags::ExtendedVerbs);
			}

			ShellContextMenu shellContextMenu(pidlDirectory.get(), pidlItems, this, m_pStatusBar);
			shellContextMenu.ShowMenu(m_hActiveListView, pCursorPos, nullptr, flags);
		}
		// Show custom context menu
		else
		{
			auto parentMenu = wil::unique_hmenu(LoadMenu(m_resourceInstance, MAKEINTRESOURCE(IDR_LIST_ITEM_CONTEXT_MENU)));
			HMENU menu = GetSubMenu(parentMenu.get(), 0);

            // Set nume icons
			std::vector<wil::unique_hbitmap> menuImages;
			AddImagesToListViewContextMenu(menu, menuImages);

			MenuHelper::EnableItem(menu, IDM_EDIT_PASTE, CanPaste(PasteType::Normal));
			MenuHelper::EnableItem(menu, IDM_EDIT_PASTESHORTCUT, CanPaste(PasteType::Shortcut));
			MenuHelper::EnableItem(menu, IDM_EDIT_COPY, CanCopy());
			MenuHelper::EnableItem(menu, IDM_EDIT_CUT, CanCut());
			MenuHelper::EnableItem(menu, ID_OPEN_IN_NEW_TAB, bSeenDirectory);

			const UINT command = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_VERTICAL | TPM_RETURNCMD,
					pCursorPos->x, pCursorPos->y, 0, m_hActiveListView, nullptr);
			if (command == IDM_APP_OPEN)
			{
				OpenAllSelectedItems(DetermineOpenDisposition(false, IsKeyDown(VK_CONTROL), IsKeyDown(VK_SHIFT)));
			}
			else if (command == ID_OPEN_IN_NEW_TAB)
            {
				for (auto &folderItem : pidFolderItems)
				{
					std::vector<PidlChild> _pidlItems{ folderItem };
					HandleCustomMenuItem(pidlDirectory.get(), _pidlItems, OPEN_IN_NEW_TAB_MENU_ITEM_ID);
				}
            }
			else
			{
				SendMessage(GetCoreInterface()->GetMainWindow(), WM_COMMAND, MAKEWPARAM(command, 0), 0);
			}
		}
	}
}

void Explorerplusplus::OnListViewDoubleClick(const NMITEMACTIVATE *eventInfo)
{
	// Note that while it's stated in the documentation for both NM_CLICK and NM_DBLCLK that "The
	// iItem member of lParam is only valid if the icon or first-column label has been clicked.", it
	// appears that's not actually the case. From testing, iItem will be correctly populated even
	// when the click/double-click takes place elsewhere in a row. Therefore, it should be ok to use
	// that value in this function.
	if (eventInfo->iItem == -1)
	{
		return;
	}

	if (WI_IsFlagSet(eventInfo->uKeyFlags, LVKF_ALT))
	{
		auto pidlDirectory = m_pActiveShellBrowser->GetDirectoryIdl();
		auto pidl = m_pActiveShellBrowser->GetItemChildIdl(eventInfo->iItem);
		ShowMultipleFileProperties(pidlDirectory.get(), { pidl.get() }, m_hContainer);
	}
	else
	{
		OpenListViewItem(eventInfo->iItem,
			DetermineOpenDisposition(false, WI_IsFlagSet(eventInfo->uKeyFlags, LVKF_CONTROL),
				WI_IsFlagSet(eventInfo->uKeyFlags, LVKF_SHIFT)));
	}
}

void Explorerplusplus::OnListViewCopyItemPath() const
{
	if (ListView_GetSelectedCount(m_hActiveListView) == 0)
	{
		return;
	}

	std::wstring strItemPaths;
	int iItem = -1;

	while ((iItem = ListView_GetNextItem(m_hActiveListView, iItem, LVNI_SELECTED)) != -1)
	{
		std::wstring fullFilename = m_pActiveShellBrowser->GetItemFullName(iItem);

		strItemPaths += fullFilename + std::wstring(_T("\r\n"));
	}

	strItemPaths = strItemPaths.substr(0, strItemPaths.size() - 2);

	BulkClipboardWriter clipboardWriter;
	clipboardWriter.WriteText(strItemPaths);
}

void Explorerplusplus::OnListViewCopyUniversalPaths() const
{
	if (ListView_GetSelectedCount(m_hActiveListView) == 0)
	{
		return;
	}

	std::wstring strUniversalPaths;
	int iItem = -1;

	while ((iItem = ListView_GetNextItem(m_hActiveListView, iItem, LVNI_SELECTED)) != -1)
	{
		std::wstring fullFilename = m_pActiveShellBrowser->GetItemFullName(iItem);

		TCHAR szBuffer[1024];

		DWORD dwBufferSize = SIZEOF_ARRAY(szBuffer);
		auto *puni = reinterpret_cast<UNIVERSAL_NAME_INFO *>(&szBuffer);
		DWORD dwRet = WNetGetUniversalName(fullFilename.c_str(), UNIVERSAL_NAME_INFO_LEVEL,
			reinterpret_cast<LPVOID>(puni), &dwBufferSize);

		if (dwRet == NO_ERROR)
		{
			strUniversalPaths += puni->lpUniversalName + std::wstring(_T("\r\n"));
		}
		else
		{
			strUniversalPaths += fullFilename + std::wstring(_T("\r\n"));
		}
	}

	strUniversalPaths = strUniversalPaths.substr(0, strUniversalPaths.size() - 2);

	BulkClipboardWriter clipboardWriter;
	clipboardWriter.WriteText(strUniversalPaths);
}

void Explorerplusplus::OnListViewSetFileAttributes() const
{
	const Tab &selectedTab = GetActivePane()->GetTabContainer()->GetSelectedTab();
	selectedTab.GetShellBrowser()->SetFileAttributesForSelection();
}

void Explorerplusplus::OnListViewPaste()
{
	wil::com_ptr_nothrow<IDataObject> clipboardObject;
	HRESULT hr = OleGetClipboard(&clipboardObject);

	if (FAILED(hr))
	{
		return;
	}

	const auto &selectedTab = GetActivePane()->GetTabContainer()->GetSelectedTab();
	auto directory = selectedTab.GetShellBrowser()->GetDirectoryIdl();

	if (CanShellPasteDataObject(directory.get(), clipboardObject.get(), PasteType::Normal))
	{
		auto serviceProvider = winrt::make_self<ServiceProvider>();

		auto folderView = winrt::make<FolderView>(selectedTab.GetShellBrowserWeak());
		serviceProvider->RegisterService(IID_IFolderView, folderView.get());

		ExecuteActionFromContextMenu(directory.get(), {},
			selectedTab.GetShellBrowser()->GetListView(), L"paste", 0, serviceProvider.get());
	}
	else
	{
		TCHAR szDestination[MAX_PATH + 1];

		/* DO NOT use the internal current directory string.
		 Files are copied asynchronously, so a change of directory
		 will cause the destination directory to change in the
		 middle of the copy operation. */
		StringCchCopy(szDestination, SIZEOF_ARRAY(szDestination),
			selectedTab.GetShellBrowser()->GetDirectory().c_str());

		/* Also, the string must be double NULL terminated. */
		szDestination[lstrlen(szDestination) + 1] = '\0';

		DropHandler *pDropHandler = DropHandler::CreateNew();
		auto *dropFilesCallback = new DropFilesCallback(this);
		pDropHandler->CopyClipboardData(clipboardObject.get(), m_hContainer, szDestination,
			dropFilesCallback);
		pDropHandler->Release();
	}
}

void Explorerplusplus::AddImagesToListViewContextMenu(HMENU menu, std::vector<wil::unique_hbitmap> &menuImages)
{
	UINT dpi = DpiCompatibility::GetInstance().GetDpiForWindow(m_hActiveListView);
	for (const auto &mapping : LIST_VIEW_RIGHT_CLICK_MENU_IMAGE_MAPPINGS)
	{
		ResourceHelper::SetMenuItemImage(menu, mapping.first,
			GetCoreInterface()->GetIconResourceLoader(), mapping.second, dpi, menuImages);
	}
}

int Explorerplusplus::HighlightSimilarFiles(HWND ListView) const
{
	BOOL bSimilarTypes;
	int iSelected;
	int nItems;
	int nSimilar = 0;
	int i = 0;

	iSelected = ListView_GetNextItem(ListView, -1, LVNI_SELECTED);

	if (iSelected == -1)
		return -1;

	std::wstring testFile = m_pActiveShellBrowser->GetItemFullName(iSelected);

	nItems = ListView_GetItemCount(ListView);

	for (i = 0; i < nItems; i++)
	{
		std::wstring fullFileName = m_pActiveShellBrowser->GetItemFullName(i);

		bSimilarTypes = CompareFileTypes(fullFileName.c_str(), testFile.c_str());

		if (bSimilarTypes)
		{
			ListViewHelper::SelectItem(ListView, i, TRUE);
			nSimilar++;
		}
		else
		{
			ListViewHelper::SelectItem(ListView, i, FALSE);
		}
	}

	return nSimilar;
}

void Explorerplusplus::OpenAllSelectedItems(OpenFolderDisposition openFolderDisposition)
{
	BOOL bSeenDirectory = FALSE;
	DWORD dwAttributes;
	int iItem = -1;
	int iFolderItem = -1;

	while ((iItem = ListView_GetNextItem(m_hActiveListView, iItem, LVIS_SELECTED)) != -1)
	{
		dwAttributes = m_pActiveShellBrowser->GetItemFileFindData(iItem).dwFileAttributes;

		if ((dwAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY)
		{
			bSeenDirectory = TRUE;
			iFolderItem = iItem;
		}
		else
		{
			OpenListViewItem(iItem);
		}
	}

	if (bSeenDirectory)
	{
		OpenListViewItem(iFolderItem, openFolderDisposition);
	}
}

void Explorerplusplus::OpenListViewItem(int index, OpenFolderDisposition openFolderDisposition)
{
	auto pidlComplete = m_pActiveShellBrowser->GetItemCompleteIdl(index);
	OpenItem(pidlComplete.get(), openFolderDisposition);
}
