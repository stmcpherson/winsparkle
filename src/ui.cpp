/*
 *  This file is part of WinSparkle (http://winsparkle.org)
 *
 *  Copyright (C) 2009-2010 Vaclav Slavik
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

// silence warnings in wxWidgets' CRT wrappers
#define _CRT_SECURE_NO_WARNINGS

#include "ui.h"
#include "settings.h"
#include "error.h"
#include "updatechecker.h"
#include "string_resources.h"

#define wxNO_NET_LIB
#define wxNO_XML_LIB
#define wxNO_XRC_LIB
#define wxNO_ADV_LIB
#define wxNO_HTML_LIB
#include <wx/setup.h>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/settings.h>
#include <wx/utils.h>
#include <wx/msw/ole/activex.h>

#include "resources.h"

#include <exdisp.h>
#include <mshtml.h>


#if !wxCHECK_VERSION(2,9,0)
#error "wxWidgets >= 2.9 is required to compile this code"
#endif

#include "updater.h"

namespace winsparkle
{

/*--------------------------------------------------------------------------*
                                  helpers
 *--------------------------------------------------------------------------*/

namespace
{


wxString _str(UINT strId) {
  WCHAR* buf = NULL;
  int    len = LoadStringW(UI::GetDllHINSTANCE(), strId, reinterpret_cast<LPWSTR>(&buf), 0);

  return len? wxString(buf, len) : wxString();
}

// Locks window updates to reduce flicker. Redoes layout in dtor.
struct LayoutChangesGuard
{
    LayoutChangesGuard(wxWindow *win) : m_win(win)
    {
        m_win->Freeze();
    }

    ~LayoutChangesGuard()
    {
        m_win->Layout();
        m_win->GetSizer()->SetSizeHints(m_win);
        m_win->Refresh();
        m_win->Thaw();
        m_win->Update();
    }

    wxWindow *m_win;
};


// shows/hides layout element
void DoShowElement(wxWindow *w, bool show)
{
    w->GetContainingSizer()->Show(w, show, true/*recursive*/);
}

void DoShowElement(wxSizer *s, bool show)
{
    s->ShowItems(show);
}

#define SHOW(c)    DoShowElement(c, true)
#define HIDE(c)    DoShowElement(c, false)

} // anonymous namespace


/*--------------------------------------------------------------------------*
                       Base class for WinSparkle dialogs
 *--------------------------------------------------------------------------*/

class WinSparkleDialog : public wxDialog
{
protected:
    WinSparkleDialog();

    void UpdateLayout();
    static void SetBoldFont(wxWindow *win);
    static void SetHeadingFont(wxWindow *win);

    // enable/disable resizing of the dialog
    void MakeResizable(bool resizable = true);

	static BOOL CALLBACK GetFirstIconProc(HMODULE hModule, LPCTSTR lpszType,
		LPTSTR lpszName, LONG_PTR lParam);

protected:
    // sizer for the main area of the dialog (to the right of the icon)
    wxSizer      *m_mainAreaSizer;

    static const int MESSAGE_AREA_WIDTH = 300;
};

WinSparkleDialog::WinSparkleDialog()
    : wxDialog(NULL, wxID_ANY, _str(IDS_TITLE),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetIcons(wxICON(UpdateAvailable));

    wxSizer *topSizer = new wxBoxSizer(wxHORIZONTAL);

    wxIcon bigIcon;
    Resources::LoadDialogIcon(bigIcon);

	topSizer->Add
              (
                  new wxStaticBitmap(this, wxID_ANY, bigIcon),
                  wxSizerFlags(0).Border(wxALL, 10)
              );

    m_mainAreaSizer = new wxBoxSizer(wxVERTICAL);
    topSizer->Add(m_mainAreaSizer, wxSizerFlags(1).Expand().Border(wxALL, 10));

    SetSizer(topSizer);

    MakeResizable(false);
}


void WinSparkleDialog::MakeResizable(bool resizable)
{
    bool is_resizable = (GetWindowStyleFlag() & wxRESIZE_BORDER) != 0;
    if ( is_resizable == resizable )
        return;

    ToggleWindowStyle(wxRESIZE_BORDER);
    Refresh(); // to paint the gripper
}


void WinSparkleDialog::UpdateLayout()
{
    Layout();
    GetSizer()->SetSizeHints(this);
}


void WinSparkleDialog::SetBoldFont(wxWindow *win)
{
    wxFont f = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    f.SetWeight(wxFONTWEIGHT_BOLD);

    win->SetFont(f);
}


void WinSparkleDialog::SetHeadingFont(wxWindow *win)
{
    wxFont f = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);

    int winver;
    wxGetOsVersion(&winver);
    if ( winver >= 6 ) // Windows Vista, 7 or newer
    {
        // 9pt is base font, 12pt is for "Main instructions". See
        // http://msdn.microsoft.com/en-us/library/aa511282%28v=MSDN.10%29.aspx
        f.SetPointSize(f.GetPointSize() + 3);
        win->SetForegroundColour(wxColour(0x00, 0x33, 0x99));
    }
    else // Windows XP/2000
    {
        f.SetWeight(wxFONTWEIGHT_BOLD);
        f.SetPointSize(f.GetPointSize() + 1);
    }

    win->SetFont(f);
}


/*--------------------------------------------------------------------------*
                      Window for communicating with the user
 *--------------------------------------------------------------------------*/

const int ID_SKIP_VERSION = wxNewId();
const int ID_REMIND_LATER = wxNewId();
const int ID_INSTALL = wxNewId();

class UpdateDialog : public WinSparkleDialog
{
public:
    UpdateDialog();

    // changes state into "checking for updates"
    void StateCheckingUpdates();
    // change state into "no updates found"
    void StateNoUpdateFound();
    // change state into "update error"
    void StateUpdateError();
    // change state into "a new version is available"
    void StateUpdateAvailable(const Appcast& info);
	// change state into "downloading update"
	void StateRunUpdate(const Appcast& info);

private:
    void EnablePulsing(bool enable);
    void OnTimer(wxTimerEvent& event);
    void OnCloseButton(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);

    void OnSkipVersion(wxCommandEvent&);
    void OnRemindLater(wxCommandEvent&);
    void OnInstall(wxCommandEvent&);

	void OnProgressUpdate(wxCommandEvent &ev);
	void OnUpdateComplete(wxCommandEvent &ev);
	void OnUpdateCancelled(wxCommandEvent &ev);

    void SetMessage(const wxString& text, int width = MESSAGE_AREA_WIDTH);
    void ShowReleaseNotes(const Appcast& info);

private:
    wxTimer       m_timer;
    wxSizer      *m_buttonSizer;
    wxStaticText *m_heading;
    wxStaticText *m_message;
    wxGauge      *m_progress;
    wxButton     *m_closeButton;
    wxSizer      *m_closeButtonSizer;
    wxSizer      *m_updateButtonsSizer;
    wxSizer      *m_releaseNotesSizer;
    wxPanel      *m_browserParent;

    wxAutoOleInterface<IWebBrowser2> m_webBrowser;

    // current appcast data (only valid after StateUpdateAvailable())
    Appcast       m_appcast;

	// current updater thread (only valid after StateRunUpdate())
	Updater		 *m_updater;
	bool		  m_runUpdateOnClose;
	wxString	  m_downloadedInstallerPath;

    static const int RELNOTES_WIDTH = 400;
    static const int RELNOTES_HEIGHT = 200;
};

UpdateDialog::UpdateDialog() : m_timer(this)
{
	m_updater = NULL;
	m_runUpdateOnClose = false;

    m_heading = new wxStaticText(this, wxID_ANY, "");
    SetHeadingFont(m_heading);
    m_mainAreaSizer->Add(m_heading, wxSizerFlags(0).Expand().Border(wxBOTTOM, 10));

    m_message = new wxStaticText(this, wxID_ANY, "",
                                 wxDefaultPosition, wxSize(MESSAGE_AREA_WIDTH, -1));
    m_mainAreaSizer->Add(m_message, wxSizerFlags(0).Expand());

    m_progress = new wxGauge(this, wxID_ANY, 100,
                             wxDefaultPosition, wxSize(MESSAGE_AREA_WIDTH, 16));
    m_mainAreaSizer->Add(m_progress, wxSizerFlags(0).Expand().Border(wxTOP|wxBOTTOM, 10));
    m_mainAreaSizer->AddStretchSpacer(1);

    m_releaseNotesSizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *notesLabel = new wxStaticText(this, wxID_ANY, _str(IDS_RELEASE_NOTES));
    SetBoldFont(notesLabel);
    m_releaseNotesSizer->Add(notesLabel, wxSizerFlags().Border(wxTOP, 10));

    m_browserParent = new wxPanel(this, wxID_ANY,
                                  wxDefaultPosition,
                                  wxSize(RELNOTES_WIDTH, RELNOTES_HEIGHT));
    m_browserParent->SetBackgroundColour(*wxWHITE);
    m_releaseNotesSizer->Add
                         (
                             m_browserParent,
                             wxSizerFlags(1).Expand().Border(wxTOP, 10)
                         );

    m_mainAreaSizer->Add
                 (
                     m_releaseNotesSizer,
                     // proportion=10000 to overcome stretch spacer above
                     wxSizerFlags(10000).Expand()
                 );

    m_buttonSizer = new wxBoxSizer(wxHORIZONTAL);

    m_updateButtonsSizer = new wxBoxSizer(wxHORIZONTAL);
    //m_updateButtonsSizer->Add
    //                      (
    //                        new wxButton(this, ID_SKIP_VERSION, _str(IDS_SKIP_THIS)),
    //                        wxSizerFlags().Border(wxRIGHT, 20)
    //                      );
    m_updateButtonsSizer->AddStretchSpacer(1);
    m_updateButtonsSizer->Add
                          (
                            new wxButton(this, ID_REMIND_LATER, _str(IDS_REMIND_LATER)),
                            wxSizerFlags().Border(wxRIGHT, 10)
                          );
    m_updateButtonsSizer->Add
                          (
                            new wxButton(this, ID_INSTALL, _str(IDS_INSTALL)),
                            wxSizerFlags()
                          );
    m_buttonSizer->Add(m_updateButtonsSizer, wxSizerFlags(1));

    m_closeButtonSizer = new wxBoxSizer(wxHORIZONTAL);
    m_closeButton = new wxButton(this, wxID_CANCEL);
    m_closeButtonSizer->AddStretchSpacer(1);
    m_closeButtonSizer->Add(m_closeButton, wxSizerFlags(0).Border(wxLEFT));
    m_buttonSizer->Add(m_closeButtonSizer, wxSizerFlags(1));

    m_mainAreaSizer->Add
                 (
                     m_buttonSizer,
                     wxSizerFlags(0).Expand().Border(wxTOP, 10)
                 );

    UpdateLayout();

    Bind(wxEVT_CLOSE_WINDOW, &UpdateDialog::OnClose, this);
    Bind(wxEVT_TIMER, &UpdateDialog::OnTimer, this);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &UpdateDialog::OnCloseButton, this, wxID_CANCEL);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &UpdateDialog::OnSkipVersion, this, ID_SKIP_VERSION);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &UpdateDialog::OnRemindLater, this, ID_REMIND_LATER);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &UpdateDialog::OnInstall, this, ID_INSTALL);

	Bind(wxEVT_COMMAND_THREAD, &UpdateDialog::OnProgressUpdate, this, Updater::PROGRESS_PERCENT_UPDATE);
	Bind(wxEVT_COMMAND_THREAD, &UpdateDialog::OnUpdateComplete, this, Updater::UPDATE_COMPLETE);
	Bind(wxEVT_COMMAND_THREAD, &UpdateDialog::OnUpdateCancelled, this, Updater::UPDATE_CANCELLED);
}

void UpdateDialog::EnablePulsing(bool enable)
{
    if ( enable && !m_timer.IsRunning() )
        m_timer.Start(100);
    else if ( !enable && m_timer.IsRunning() )
        m_timer.Stop();
}

void UpdateDialog::OnProgressUpdate(wxCommandEvent &ev)
{
	m_progress->SetValue(ev.GetInt());
}

void UpdateDialog::OnUpdateCancelled(wxCommandEvent &ev)
{
	LayoutChangesGuard guard(this);

	m_updater = NULL;
	HIDE(m_progress);

	if (ev.GetInt() != 0)
	{
		// the user requested this cancellation.
		Close();
	}
	else
	{
		SetMessage(_str(IDS_UPDATE_CANCELLED));
		m_closeButton->SetLabelText(_str(IDS_CLOSE));
	}
}

void UpdateDialog::OnUpdateComplete(wxCommandEvent &ev)
{
	LayoutChangesGuard guard(this);

	m_updater = NULL;

	m_downloadedInstallerPath = ev.GetString();
	m_runUpdateOnClose = (m_downloadedInstallerPath.length() > 0);

	HIDE(m_progress);
	if (m_runUpdateOnClose)
	{
		SetMessage(_str(IDS_DOWNLOAD_OK_MSG));
		m_closeButton->SetLabelText(_str(IDS_CLOSE_INSTALL));
	}
}

void UpdateDialog::OnTimer(wxTimerEvent&)
{
    m_progress->Pulse();
}

void UpdateDialog::OnCloseButton(wxCommandEvent&)
{
	if (m_runUpdateOnClose)
	{
		Updater::RunUpdate(m_downloadedInstallerPath);
		Close();
	}
	else if (m_updater)
	{
		m_updater->RequestStop();
		m_closeButton->Disable();
	}
	else
		Close();
}


void UpdateDialog::OnClose(wxCloseEvent&)
{
    // We need to override this, because by default, wxDialog doesn't
    // destroy itself in Close().
    Destroy();
}


void UpdateDialog::OnSkipVersion(wxCommandEvent&)
{
    Settings::WriteConfigValue("SkipThisVersion", m_appcast.Version);
    Close();
}


void UpdateDialog::OnRemindLater(wxCommandEvent&)
{
    // Just abort the update. Next time it's scheduled to run,
    // the user will be prompted.
    Close();
}


void UpdateDialog::OnInstall(wxCommandEvent&)
{
    UI::RunUpdate(m_appcast);
    Close();
}


void UpdateDialog::SetMessage(const wxString& text, int width)
{
    m_message->SetLabel(text);
    m_message->Wrap(width);
}


void UpdateDialog::StateCheckingUpdates()
{
    LayoutChangesGuard guard(this);

    SetMessage(_str(IDS_CHECKING));

    m_closeButton->SetLabel(_str(IDS_CANCEL));
    EnablePulsing(true);

    HIDE(m_heading);
    SHOW(m_progress);
    SHOW(m_closeButtonSizer);
    HIDE(m_releaseNotesSizer);
    HIDE(m_updateButtonsSizer);
    MakeResizable(false);
}


void UpdateDialog::StateNoUpdateFound()
{
    LayoutChangesGuard guard(this);

    m_heading->SetLabel(_str(IDS_UP2DATE));

    wxString msg;
    try
    {
        msg = wxString::Format
              (
                  _str(IDS_NEWEST),
                  Settings::GetAppName(),
                  Settings::GetAppVersion()
              );
    }
    catch ( std::exception& )
    {
        // GetAppVersion() may fail
        msg = "Error: Updates checking not properly configured.";
    }

    SetMessage(msg);

    m_closeButton->SetLabel(_str(IDS_CLOSE));
    EnablePulsing(false);

    SHOW(m_heading);
    HIDE(m_progress);
    SHOW(m_closeButtonSizer);
    HIDE(m_releaseNotesSizer);
    HIDE(m_updateButtonsSizer);
    MakeResizable(false);
}


void UpdateDialog::StateUpdateError()
{
    LayoutChangesGuard guard(this);

    m_heading->SetLabel(_str(IDS_UPDATE_ERR));

    wxString msg = _str(IDS_UPDATE_ERR_MSG);
    SetMessage(msg);

    m_closeButton->SetLabel(_str(IDS_CANCEL));
    EnablePulsing(false);

    SHOW(m_heading);
    HIDE(m_progress);
    SHOW(m_closeButtonSizer);
    HIDE(m_releaseNotesSizer);
    HIDE(m_updateButtonsSizer);
    MakeResizable(false);
}

void UpdateDialog::StateRunUpdate(const Appcast& info)
{
	LayoutChangesGuard guard(this);
	const wxString appname = Settings::GetAppName();

	m_appcast = info;

	m_heading->SetLabel(_str(IDS_UPDATING));
	SetMessage(wxString::Format(_str(IDS_DOWNLOADING_UPDATE), appname));

	m_closeButton->SetLabel(_str(IDS_CANCEL));
	EnablePulsing(false);

	SHOW(m_heading);
	SHOW(m_progress);
	SHOW(m_closeButtonSizer);
	HIDE(m_releaseNotesSizer);
	HIDE(m_updateButtonsSizer);
	MakeResizable(false);

	m_progress->SetRange(100);

	m_updater = new Updater(m_appcast, this);
	if (m_updater)
	{
		if (wxTHREAD_NO_ERROR == m_updater->Create())
		{
			m_updater->Run();
		}
	}
}

void UpdateDialog::StateUpdateAvailable(const Appcast& info)
{
    m_appcast = info;

    const bool showRelnotes = !info.ReleaseNotesURL.empty() || !info.Description.empty();

    {
        LayoutChangesGuard guard(this);

        const wxString appname = Settings::GetAppName();

        m_heading->SetLabel(
            wxString::Format(_str(IDS_NEW_AVAILABLE), appname));

        SetMessage
        (
            wxString::Format
            (
                _str(IDS_ASK_DOWNLOAD),
                appname,
                info.DisplayVersion(Settings::GetAppVersion()).c_str(),
                Settings::GetAppVersion()
            ),
            showRelnotes ? RELNOTES_WIDTH : MESSAGE_AREA_WIDTH
        );

        EnablePulsing(false);

        SHOW(m_heading);
        HIDE(m_progress);
        HIDE(m_closeButtonSizer);
        SHOW(m_updateButtonsSizer);
        DoShowElement(m_releaseNotesSizer, showRelnotes);
        MakeResizable(showRelnotes);
    }

    // Only show the release notes now that the layout was updated, as it may
    // take some time to load the MSIE control:
    if ( showRelnotes )
        ShowReleaseNotes(info);
}


void UpdateDialog::ShowReleaseNotes(const Appcast& info)
{
    if ( !m_webBrowser.IsOk() )
    {
        // Load MSIE control

        wxBusyCursor busy;

        IWebBrowser2 *browser;
        HRESULT hr = CoCreateInstance
                     (
                         CLSID_WebBrowser,
                         NULL,
                         CLSCTX_INPROC_SERVER,
                         IID_IWebBrowser2,
                         (void**)&browser
                     );

        if ( FAILED(hr) )
        {
            // hide the notes again, we cannot show them
            LayoutChangesGuard guard(this);
            HIDE(m_releaseNotesSizer);
            MakeResizable(false);
            LogError("Failed to create WebBrowser ActiveX control.");
            return;
        }

        m_webBrowser = browser;

        new wxActiveXContainer(m_browserParent, IID_IWebBrowser2, browser);
    }

    if( !info.ReleaseNotesURL.empty() )
    {
        m_webBrowser->Navigate
                      (
                          wxBasicString(info.ReleaseNotesURL),
                          NULL,  // Flags
                          NULL,  // TargetFrameName
                          NULL,  // PostData
                          NULL   // Headers
                      );
    }
    else if ( !info.Description.empty() )
    {
        m_webBrowser->Navigate
                      (
                          wxBasicString("about:blank"),
                          NULL,  // Flags
                          NULL,  // TargetFrameName
                          NULL,  // PostData
                          NULL   // Headers
                      );

        HRESULT hr = E_FAIL;
        IHTMLDocument2 *doc;
        hr = m_webBrowser->get_Document((IDispatch **)&doc);
        if ( FAILED(hr) || !doc )
        {
            LogError("Failed to get HTML document");
            return;
        }

        // Creates a new one-dimensional array
        SAFEARRAY *psaStrings = SafeArrayCreateVector(VT_VARIANT, 0, 1);
        if ( psaStrings != NULL )
        {
            VARIANT *param;
            SafeArrayAccessData(psaStrings, (LPVOID*)&param);
            param->vt = VT_BSTR;
            param->bstrVal = wxBasicString(info.Description);
            SafeArrayUnaccessData(psaStrings);

            doc->write(psaStrings);

            SafeArrayDestroy(psaStrings);
            doc->Release();
        }
    }

    SetWindowStyleFlag(wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
}


/*--------------------------------------------------------------------------*
              Dialog that asks for permission to check for updates
 *--------------------------------------------------------------------------*/

class AskPermissionDialog : public WinSparkleDialog
{
public:
    AskPermissionDialog();
};


AskPermissionDialog::AskPermissionDialog()
{
    wxStaticText *heading =
            new wxStaticText(this, wxID_ANY, _str(IDS_ASK_AUTO_UPDATE));
    SetHeadingFont(heading);
    m_mainAreaSizer->Add(heading, wxSizerFlags(0).Expand().Border(wxBOTTOM, 10));

    wxStaticText *message =
            new wxStaticText
                (
                    this, wxID_ANY,
                    wxString::Format
                    (
                        _str(IDS_ASK_AUTO_UPDATE_MSG),
                        Settings::GetAppName()
                    ),
                    wxDefaultPosition, wxSize(MESSAGE_AREA_WIDTH, -1)
                );
    message->Wrap(MESSAGE_AREA_WIDTH);
    m_mainAreaSizer->Add(message, wxSizerFlags(0).Expand());

    m_mainAreaSizer->AddStretchSpacer(1);

    wxBoxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);

    buttonSizer->Add
                 (
                     new wxButton(this, wxID_OK, _str(IDS_CHECK_AUTOMATIC)),
                     wxSizerFlags().Border(wxRIGHT)
                 );
    buttonSizer->Add
                 (
                     new wxButton(this, wxID_CANCEL, _str(IDS_DONT_CHECK))
                 );

    m_mainAreaSizer->Add
                 (
                     buttonSizer,
                     wxSizerFlags(0).Right().Border(wxTOP, 10)
                 );

    UpdateLayout();
}


/*--------------------------------------------------------------------------*
                             Inter-thread messages
 *--------------------------------------------------------------------------*/

// Terminate the wx thread
const int MSG_TERMINATE = wxNewId();

// Show "Checking for updates..." window
const int MSG_SHOW_CHECKING_UPDATES = wxNewId();

// Notify the UI that there were no updates
const int MSG_NO_UPDATE_FOUND = wxNewId();

// Notify the UI that a new version is available
const int MSG_UPDATE_AVAILABLE = wxNewId();

// Notify the UI that a new version is available
const int MSG_UPDATE_ERROR = wxNewId();

// Tell the UI to ask for permission to check updates
const int MSG_ASK_FOR_PERMISSION = wxNewId();

// Tell the UI to download and open the update
const int MSG_RUN_UPDATE = wxNewId();

/*--------------------------------------------------------------------------*
                                Application
 *--------------------------------------------------------------------------*/

class App : public wxApp
{
public:
    App();

    // Sends a message with ID @a msg to the app.
    void SendMsg(int msg, void *data = NULL);

private:
    void InitWindow();
    void ShowWindow();

    void OnWindowClose(wxCloseEvent& event);
    void OnTerminate(wxThreadEvent& event);
    void OnShowCheckingUpdates(wxThreadEvent& event);
    void OnNoUpdateFound(wxThreadEvent& event);
    void OnUpdateAvailable(wxThreadEvent& event);
    void OnUpdateError(wxThreadEvent& event);
    void OnAskForPermission(wxThreadEvent& event);
	void OnRunUpdate(wxThreadEvent& event);

private:
    UpdateDialog *m_win;
};

IMPLEMENT_APP_NO_MAIN(App)

App::App()
{
    m_win = NULL;

    // Keep the wx "main" thread running even without windows. This greatly
    // simplifies threads handling, because we don't have to correctly
    // implement wx-thread restarting.
    //
    // Note that this only works if we don't explicitly call ExitMainLoop(),
    // except in reaction to win_sparkle_cleanup()'s message.
    // win_sparkle_cleanup() relies on the availability of wxApp instance and
    // if the event loop terminated, wxEntry() would return and wxApp instance
    // would be destroyed.
    //
    // Also note that this is efficient, because if there are no windows, the
    // thread will sleep waiting for a new event. We could safe some memory
    // by shutting the thread down when it's no longer needed, though.
    SetExitOnFrameDelete(false);

    // Bind events to their handlers:
    Bind(wxEVT_COMMAND_THREAD, &App::OnTerminate, this, MSG_TERMINATE);
    Bind(wxEVT_COMMAND_THREAD, &App::OnShowCheckingUpdates, this, MSG_SHOW_CHECKING_UPDATES);
    Bind(wxEVT_COMMAND_THREAD, &App::OnNoUpdateFound, this, MSG_NO_UPDATE_FOUND);
    Bind(wxEVT_COMMAND_THREAD, &App::OnUpdateAvailable, this, MSG_UPDATE_AVAILABLE);
    Bind(wxEVT_COMMAND_THREAD, &App::OnUpdateError, this, MSG_UPDATE_ERROR);
    Bind(wxEVT_COMMAND_THREAD, &App::OnAskForPermission, this, MSG_ASK_FOR_PERMISSION);
    Bind(wxEVT_COMMAND_THREAD, &App::OnRunUpdate, this, MSG_RUN_UPDATE);
}


void App::SendMsg(int msg, void *data)
{
    wxThreadEvent *event = new wxThreadEvent(wxEVT_COMMAND_THREAD, msg);
    if ( data )
        event->SetClientData(data);

    wxQueueEvent(this, event);
}


void App::InitWindow()
{
    if ( !m_win )
    {
        m_win = new UpdateDialog();
        m_win->Bind(wxEVT_CLOSE_WINDOW, &App::OnWindowClose, this);
    }
}


void App::ShowWindow()
{
    wxASSERT( m_win );

    m_win->Freeze();
    m_win->Show();
    m_win->Thaw();
    m_win->Raise();
}


void App::OnWindowClose(wxCloseEvent& event)
{
    m_win = NULL;
    event.Skip();
}


void App::OnTerminate(wxThreadEvent&)
{
    ExitMainLoop();
}


void App::OnShowCheckingUpdates(wxThreadEvent&)
{
    InitWindow();
    m_win->StateCheckingUpdates();
    ShowWindow();
}


void App::OnNoUpdateFound(wxThreadEvent&)
{
    if ( m_win )
        m_win->StateNoUpdateFound();
}


void App::OnUpdateError(wxThreadEvent&)
{
    if ( m_win )
        m_win->StateUpdateError();
}


void App::OnUpdateAvailable(wxThreadEvent& event)
{
    InitWindow();

    Appcast *appcast = static_cast<Appcast*>(event.GetClientData());

    m_win->StateUpdateAvailable(*appcast);

    delete appcast;

    ShowWindow();
}

void App::OnRunUpdate(wxThreadEvent& event)
{
	InitWindow();

	Appcast *appcast = static_cast<Appcast*>(event.GetClientData());

	m_win->StateRunUpdate(*appcast);

	delete appcast;

	ShowWindow();
}


void App::OnAskForPermission(wxThreadEvent& event)
{
    AskPermissionDialog dlg;
    bool shouldCheck = true; //(dlg.ShowModal() == wxID_OK);

    Settings::WriteConfigValue("CheckForUpdates", shouldCheck);

    if ( shouldCheck )
    {
        UpdateChecker *check = new UpdateChecker();
        check->Start();
    }
}


/*--------------------------------------------------------------------------*
                             winsparkle::UI class
 *--------------------------------------------------------------------------*/

// helper for accessing the UI thread
class UIThreadAccess
{
public:
    UIThreadAccess() : m_lock(ms_uiThreadCS) {}

    App& App()
    {
        StartIfNeeded();
        return wxGetApp();
    };

    bool IsRunning() const { return ms_uiThread != NULL; }

    // intentionally not static, to force locking before access
    UI* UIThread() { return ms_uiThread; }

    void ShutDownThread()
    {
        if ( ms_uiThread )
        {
            ms_uiThread->Join();
            delete ms_uiThread;
            ms_uiThread = NULL;
        }
    }

private:
    void StartIfNeeded()
    {
        // if the thread is not running yet, we have to start it
        if ( !ms_uiThread )
        {
            ms_uiThread = new UI();
            ms_uiThread->Start();
        }
    }

    CriticalSectionLocker m_lock;

    static UI *ms_uiThread;
    static CriticalSection ms_uiThreadCS;
};

UI *UIThreadAccess::ms_uiThread = NULL;
CriticalSection UIThreadAccess::ms_uiThreadCS;


HINSTANCE UI::ms_hInstance = NULL;


UI::UI() : Thread("WinSparkle UI thread")
{
}


void UI::Run()
{
    // Note: The thread that called UI::Get() holds gs_uiThreadCS
    //       at this point and won't release it until we signal it.

    // We need to pass correct HINSTANCE to wxEntry() and the right value is
    // HINSTANCE of this DLL, not of the main .exe.

    if ( !ms_hInstance )
        return; // DllMain() not called? -- FIXME: throw

    // IMPLEMENT_WXWIN_MAIN does this as the first thing
    wxDISABLE_DEBUG_SUPPORT();

    // We do this before wxEntry() explicitly, even though wxEntry() would
    // do it too, so that we know when wx is initialized and can signal
    // run_wx_gui_from_dll() about it *before* starting the event loop.
    wxInitializer wxinit;
    if ( !wxinit.IsOk() )
        return; // failed to init wx -- FIXME: throw

    // Signal run_wx_gui_from_dll() that it can continue
    SignalReady();

    // Run the app:
    wxEntry(ms_hInstance);
}


/*static*/
void UI::ShutDown()
{
    UIThreadAccess uit;

    if ( !uit.IsRunning() )
        return;

    uit.App().SendMsg(MSG_TERMINATE);
    uit.ShutDownThread();
}


/*static*/
void UI::NotifyNoUpdates()
{
    UIThreadAccess uit;

    if ( !uit.IsRunning() )
        return;

    uit.App().SendMsg(MSG_NO_UPDATE_FOUND);
}


/*static*/
void UI::NotifyUpdateAvailable(const Appcast& info)
{
    UIThreadAccess uit;
    uit.App().SendMsg(MSG_UPDATE_AVAILABLE, new Appcast(info));
}


/*static*/
void UI::RunUpdate(const Appcast& info)
{
	UIThreadAccess uit;
	uit.App().SendMsg(MSG_RUN_UPDATE, new Appcast(info));
}


/*static*/
void UI::NotifyUpdateError()
{
    UIThreadAccess uit;

    if ( !uit.IsRunning() )
        return;

    uit.App().SendMsg(MSG_UPDATE_ERROR);
}


/*static*/
void UI::ShowCheckingUpdates()
{
    UIThreadAccess uit;
    uit.App().SendMsg(MSG_SHOW_CHECKING_UPDATES);
}


/*static*/
void UI::AskForPermission()
{
    UIThreadAccess uit;
    uit.App().SendMsg(MSG_ASK_FOR_PERMISSION);
}


} // namespace winsparkle
