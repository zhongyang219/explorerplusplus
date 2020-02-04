// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "BookmarkClipboard.h"
#include "../Helper/BulkClipboardWriter.h"
#include "../Helper/StringHelper.h"
#include <boost/algorithm/string/join.hpp>

BookmarkClipboard::BookmarkClipboard()
{

}

UINT BookmarkClipboard::GetClipboardFormat()
{
	static UINT clipboardFormat = RegisterClipboardFormat(CLIPBOARD_FORMAT_STRING);
	return clipboardFormat;
}

BookmarkItems BookmarkClipboard::ReadBookmarks()
{
	Clipboard clipboard;
	auto data = clipboard.ReadCustomData(GetClipboardFormat());

	if (!data)
	{
		return {};
	}

	return BookmarkDataExchange::DeserializeBookmarkItems(*data);
}

bool BookmarkClipboard::WriteBookmarks(const OwnedRefBookmarkItems &bookmarkItems)
{
	BulkClipboardWriter clipboardWriter;
	std::vector<std::wstring> lines;

	for (auto &bookmarkItem : bookmarkItems)
	{
		if (bookmarkItem.get()->IsFolder())
		{
			lines.push_back(bookmarkItem.get()->GetName());
		}
		else
		{
			lines.push_back(bookmarkItem.get()->GetLocation());
		}
	}

	std::wstring text = boost::algorithm::join(lines, L"\n");
	clipboardWriter.WriteText(text);

	std::string data = BookmarkDataExchange::SerializeBookmarkItems(bookmarkItems);
	return clipboardWriter.WriteCustomData(GetClipboardFormat(), data);
}