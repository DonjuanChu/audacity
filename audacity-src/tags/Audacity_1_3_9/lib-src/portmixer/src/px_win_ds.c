/*
 * PortMixer
 * Windows DirectSound Implementation
 *
 * Copyright (c) 2002, 2006
 *
 * Written by Dominic Mazzoni and Augustus Saunders
 *        and Leland Lucius
 *
 * PortMixer is intended to work side-by-side with PortAudio,
 * the Portable Real-Time Audio Library by Ross Bencina and
 * Phil Burk.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#define COBJMACROS
#define DIRECTSOUND_VERSION 0x0300

#include <windows.h>
#include <dsound.h>
#include <dsconf.h>
#include <objbase.h>
#include <unknwn.h>

#include "portaudio.h"
#include "pa_win_ds.h"

#include "portmixer.h"
#include "px_mixer.h"
#include "px_win_common.h"

const GUID FAR DSPROPSETID_DirectSoundDevice = 
{0x84624f82, 0x25ec, 0x11d1, {0xa4, 0xd8, 0x0, 0xc0, 0x4f, 0xc2, 0x8a, 0xca}};

const GUID FAR CLSID_DirectSoundPrivate = 
{0x11ab3ec0, 0x25ec, 0x11d1, {0xa4, 0xd8, 0x0, 0xc0, 0x4f, 0xc2, 0x8a, 0xca}};

typedef HRESULT (CALLBACK *GCO) (REFCLSID, REFIID, LPVOID *);

#if defined(_DEBUG)
#include <stdio.h>
static void dprintf(const char *format, ...)
{
   char buf[4096];
   va_list args;
   int cnt;

   va_start(args, format);
   cnt = _vsnprintf(buf, sizeof(buf) - 1, format, args);
   va_end(args);

   if (cnt > 0) {
      buf[cnt] = '\0';
      OutputDebugString(buf);
   }
}
#endif

int OpenMixer_Win_DirectSound(px_mixer *Px, int index)
{
   DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_DATA desc;
   HMODULE hDsound = INVALID_HANDLE_VALUE;
   GCO DllGetClassObject;
   IClassFactory *pcf = NULL;
   IKsPropertySet *pps = NULL;
   HRESULT hr;
   ULONG bytes;
   LPGUID guidIn;
   LPGUID guidOut;
   UINT deviceIn = -1;
   UINT deviceOut = -1;

   guidIn = PaWinDS_GetStreamInputGUID(Px->pa_stream);
   guidOut = PaWinDS_GetStreamOutputGUID(Px->pa_stream);

   do {
      hDsound = LoadLibrary("dsound.dll");
      if (hDsound == NULL) {
         break;
      }
   
      DllGetClassObject = (GCO) GetProcAddress(hDsound, "DllGetClassObject");
      if (DllGetClassObject == NULL) {
         break;
      }
   
      hr = DllGetClassObject(&CLSID_DirectSoundPrivate,
                             &IID_IClassFactory,
                             (void **)(&pcf));
      if (hr || pcf == NULL) {
         break;
      }
   
      hr = IClassFactory_CreateInstance(pcf,
                                        NULL,
                                        &IID_IKsPropertySet,
                                        (void **)(&pps));
      if (hr || pps == NULL) {
         break;
      }
   
      if (guidIn) {
         memcpy(&desc.DeviceId, guidIn, sizeof(desc.DeviceId));
         desc.DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
         hr = IKsPropertySet_Get(pps,
                                 &DSPROPSETID_DirectSoundDevice,
                                 DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION,
                                 NULL,
                                 0,
                                 &desc,
                                 sizeof(desc),
                                 &bytes);
         if (FAILED(hr)) {
            break;
         }

         hr = mixerGetID((HMIXEROBJ) desc.WaveDeviceId,
                         &deviceIn,
                         MIXER_OBJECTF_WAVEIN);
         if (hr != MMSYSERR_NOERROR) {
           break;
         }
      }
   
      if (guidOut) {
         memcpy(&desc.DeviceId, guidOut, sizeof(desc.DeviceId));
         desc.DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
         hr = IKsPropertySet_Get(pps,
                                 &DSPROPSETID_DirectSoundDevice,
                                 DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION,
                                 NULL,
                                 0,
                                 &desc,
                                 sizeof(desc),
                                 &bytes);
         if (FAILED(hr)) {
            break;
         }

         hr = mixerGetID((HMIXEROBJ) desc.WaveDeviceId,
                         &deviceOut,
                         MIXER_OBJECTF_WAVEOUT);
         if (hr != MMSYSERR_NOERROR) {
           break;
         }
      }

      if (open_mixers(Px, deviceIn, deviceOut))
      {
         return TRUE;
      }
   } while( FALSE );

   if (pps) {
      IUnknown_Release(pps);
   }

   if (pcf) {
      IUnknown_Release(pcf);
   }

   if (hDsound != INVALID_HANDLE_VALUE) {
      FreeLibrary(hDsound);
   }

   return FALSE;
}
