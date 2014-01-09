/*
 *  This file is part of WinSparkle (http://winsparkle.org)
 *
 *  Copyright (C) 2009-2013 Vaclav Slavik
 *  Copyright (C) 2007 Andy Matuschak
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "updatechecker.h"
#include "appcast.h"
#include "ui.h"
#include "error.h"
#include "settings.h"
#include "download.h"
#include "utils.h"

#include <ctime>
#include <algorithm>

using namespace std;

namespace winsparkle
{

/*--------------------------------------------------------------------------*
                             UpdateChecker::Run()
 *--------------------------------------------------------------------------*/

UpdateChecker::UpdateChecker(): Thread("WinSparkle updates check")
{
}


void UpdateChecker::Run()
{
    // no initialization to do, so signal readiness immediately
    SignalReady();

    try
    {
        const std::string url = Settings::GetAppcastURL();
        if ( url.empty() )
            throw std::runtime_error("Appcast URL not specified.");

        StringDownloadSink appcast_xml;
        DownloadFile(url, &appcast_xml, GetAppcastDownloadFlags());

        Appcast appcast;
        appcast.Load(appcast_xml.data);

        Settings::WriteConfigValue("LastCheckTime", time(NULL));

        const std::string currentVersion =
                WideToAnsi(Settings::GetAppBuildVersion());

        // Check if our version is out of date.
        if ( CompareVersions(currentVersion, appcast.Version) >= 0 )
        {
            // The same or newer version is already installed.
            UI::NotifyNoUpdates();
            return;
        }

        // Check if the user opted to ignore this particular version.
        if ( ShouldSkipUpdate(appcast) )
        {
            UI::NotifyNoUpdates();
            return;
        }

        UI::NotifyUpdateAvailable(appcast);
    }
    catch ( ... )
    {
        UI::NotifyUpdateError();
        throw;
    }
}

bool UpdateChecker::ShouldSkipUpdate(const Appcast& appcast) const
{
    std::string toSkip;
    if ( Settings::ReadConfigValue("SkipThisVersion", toSkip) )
    {
        return toSkip == appcast.Version;
    }
    else
    {
        return false;
    }
}


/*--------------------------------------------------------------------------*
                            ManualUpdateChecker
 *--------------------------------------------------------------------------*/

int ManualUpdateChecker::GetAppcastDownloadFlags() const
{
    // Manual check should always connect to the server and bypass any caching.
    // This is good for finding updates that are too new to propagate through
    // caches yet.
    return Download_NoCached;
}

bool ManualUpdateChecker::ShouldSkipUpdate(const Appcast&) const
{
    // If I chose "Skip version" it should not prompt me for automatic updates,
    // but if I explicitly open the dialog using
    // win_sparkle_check_update_with_ui() it should still show that version.
    // This is the semantics in Sparkle for Mac.
    return false;
}

} // namespace winsparkle
