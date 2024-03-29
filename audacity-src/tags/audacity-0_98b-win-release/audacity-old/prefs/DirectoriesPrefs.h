/**********************************************************************

  Audacity: A Digital Audio Editor

  FileFormatPrefs.h

  Joshua Haberman

**********************************************************************/

#ifndef __AUDACITY_DIRECTORIES_PREFS__
#define __AUDACITY_DIRECTORIES_PREFS__

#include <wx/window.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/longlong.h>

#include "PrefsPanel.h"

class DirectoriesPrefs:public PrefsPanel {

 public:
   DirectoriesPrefs(wxWindow * parent);
   ~DirectoriesPrefs();
   bool Apply();
   void UpdateFreeSpace(wxCommandEvent &event);

   void OnChooseTempDir(wxCommandEvent &event);

 private:
   wxString FormatSize(wxLongLong size);
   wxStaticBox *mEnclosingBox;

   wxStaticText *mTempDirLabel;
   wxTextCtrl   *mTempDirText;
   wxStaticText *mFreeSpaceLabel;
   wxStaticText *mFreeSpace;

   wxString      mOldTempDir;
   wxString      mTempDir;

public:
   DECLARE_EVENT_TABLE();

};

#endif
