/**********************************************************************

  Audacity: A Digital Audio Editor

  AudioIOPrefs.h

  Joshua Haberman

**********************************************************************/

#ifndef __AUDACITY_AUDIO_IO_PREFS__
#define __AUDACITY_AUDIO_IO_PREFS__

#include <wx/window.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/statbox.h>
#include <wx/event.h>
#include <wx/button.h>
#include <wx/checkbox.h>

#include "PrefsPanel.h"

class AudioIOPrefs:public PrefsPanel {

 public:
   AudioIOPrefs(wxWindow * parent);
   ~AudioIOPrefs();
   bool Apply();

   void TestPlaybackDevice(wxCommandEvent & event);
   void SetPlaybackDeviceDefault(wxCommandEvent & event);
   void TestRecordingDevice(wxCommandEvent & event);
   void SetRecordingDeviceDefault(wxCommandEvent & event);

 private:
   wxTextCtrl *mPlaybackDeviceCtrl;
   wxButton *mPlaybackDeviceTest;
   wxButton *mPlaybackDeviceDefault;

   wxTextCtrl *mRecordingDeviceCtrl;
   wxButton *mRecordingDeviceTest;
   wxButton *mRecordingDeviceDefault;

   wxCheckBox *mRecordStereo;
   wxCheckBox *mDuplex;

 public:
    DECLARE_EVENT_TABLE()

};

#endif
