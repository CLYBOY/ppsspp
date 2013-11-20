// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.


// Manages the PSP/GAME directory contents.
//
// Not concerned with full ISOs.

#include "net/http_client.h"

class GameManager {
public:
	bool IsGameInstalled(std::string name);

	// This starts off a background process.
	bool DownloadAndInstall(std::string storeZipUrl);
	void Uninstall(std::string name);

	// Call from time to time to check on completed downloads from the
	// main UI thread.
	void Update();

private:
	void InstallGame(std::string zipfile);

	http::Downloader downloader_;
	std::shared_ptr<http::Download> curDownload_;
};

extern GameManager g_GameManager;
