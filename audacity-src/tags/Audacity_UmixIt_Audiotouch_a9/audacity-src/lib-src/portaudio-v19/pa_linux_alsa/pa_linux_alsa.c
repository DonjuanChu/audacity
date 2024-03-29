/*
 * $Id: pa_linux_alsa.c,v 1.1.2.2 2006-04-08 16:12:26 richardash1981 Exp $
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * ALSA implementation by Joshua Haberman and Arve Knudsen
 *
 * Copyright (c) 2002 Joshua Haberman <joshua@haberman.com>
 * Copyright (c) 2005-2006 Arve Knudsen <aknuds-1@broadpark.no>
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
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
 */

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#undef ALSA_PCM_NEW_HW_PARAMS_API
#undef ALSA_PCM_NEW_SW_PARAMS_API

#include <sys/poll.h>
#include <string.h> /* strlen() */
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <signal.h> /* For sig_atomic_t */

#include "portaudio.h"
#include "pa_util.h"
#include "../pa_unix/pa_unix_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_linux_alsa.h"

/* Check return value of ALSA function, and map it to PaError */
#define ENSURE_(expr, code) \
    do { \
        if( UNLIKELY( (aErr_ = (expr)) < 0 ) ) \
        { \
            /* PaUtil_SetLastHostErrorInfo should only be used in the main thread */ \
            if( (code) == paUnanticipatedHostError && pthread_self() != callbackThread_ ) \
            { \
                PaUtil_SetLastHostErrorInfo( paALSA, aErr_, snd_strerror( aErr_ ) ); \
            } \
            PaUtil_DebugPrint( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" ); \
            if( (code) == paUnanticipatedHostError ) \
                PA_DEBUG(( "Host error description: %s\n", snd_strerror( aErr_ ) )); \
            result = (code); \
            goto error; \
        } \
    } while( 0 );

#define ENSURE_SYSTEM_(expr, success) \
    do { \
        if( UNLIKELY( (aErr_ = (expr)) != success ) ) \
        { \
            /* PaUtil_SetLastHostErrorInfo should only be used in the main thread */ \
            if( pthread_self() != callbackThread_ ) \
            { \
                PaUtil_SetLastHostErrorInfo( paALSA, aErr_, strerror( aErr_ ) ); \
            } \
            PaUtil_DebugPrint( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" ); \
            result = paUnanticipatedHostError; \
            goto error; \
        } \
    } while( 0 );

#define ASSERT_CALL_(expr, success) \
    aErr_ = (expr); \
    assert( success == aErr_ );

static int aErr_;               /* Used with ENSURE_ */
static pthread_t callbackThread_;

typedef enum
{
    StreamDirection_In,
    StreamDirection_Out
} StreamDirection;

/* Threading utility struct */
typedef struct PaAlsaThreading
{
    pthread_t watchdogThread;
    pthread_t callbackThread;
    int watchdogRunning;
    int rtSched;
    int rtPrio;
    int useWatchdog;
    unsigned long throttledSleepTime;
    volatile PaTime callbackTime;
    volatile PaTime callbackCpuTime;
    PaUtilCpuLoadMeasurer *cpuLoadMeasurer;
} PaAlsaThreading;

typedef struct
{
    PaSampleFormat hostSampleFormat;
    unsigned long framesPerBuffer;
    int numUserChannels, numHostChannels;
    int userInterleaved, hostInterleaved;

    snd_pcm_t *pcm;
    snd_pcm_uframes_t bufferSize;
    snd_pcm_format_t nativeFormat;
    unsigned int nfds;
    int ready;  /* Marked ready from poll */
    void **userBuffers;
    snd_pcm_uframes_t offset;
    StreamDirection streamDir;

    snd_pcm_channel_area_t *channelAreas;  /* Needed for channel adaption */
} PaAlsaStreamComponent;

/* Implementation specific stream structure */
typedef struct PaAlsaStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;
    PaAlsaThreading threading;

    unsigned long framesPerUserBuffer, maxFramesPerHostBuffer;

    int primeBuffers;
    int callbackMode;              /* bool: are we running in callback mode? */
    int pcmsSynced;	            /* Have we successfully synced pcms */

    /* the callback thread uses these to poll the sound device(s), waiting
     * for data to be ready/available */
    struct pollfd* pfds;
    int pollTimeout;

    /* Used in communication between threads */
    volatile sig_atomic_t callback_finished; /* bool: are we in the "callback finished" state? */
    volatile sig_atomic_t callbackAbort;    /* Drop frames? */
    volatile sig_atomic_t callbackStop;     /* Signal a stop */
    volatile sig_atomic_t isActive;         /* Is stream in active state? (Between StartStream and StopStream || !paContinue) */
    pthread_mutex_t stateMtx;               /* Used to synchronize access to stream state */
    pthread_mutex_t startMtx;               /* Used to synchronize stream start in callback mode */
    pthread_cond_t startCond;               /* Wait untill audio is started in callback thread */

    int neverDropInput;

    PaTime underrun;
    PaTime overrun;

    PaAlsaStreamComponent capture, playback;
}
PaAlsaStream;

/* PaAlsaHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct PaAlsaHostApiRepresentation
{
    PaUtilHostApiRepresentation baseHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    PaHostApiIndex hostApiIndex;
}
PaAlsaHostApiRepresentation;

typedef struct PaAlsaDeviceInfo
{
    PaDeviceInfo baseDeviceInfo;
    char *alsaName;
    int isPlug;
    int minInputChannels;
    int minOutputChannels;
}
PaAlsaDeviceInfo;

/* Threading utilities */

static void InitializeThreading( PaAlsaThreading *th, PaUtilCpuLoadMeasurer *clm )
{
    th->watchdogRunning = 0;
    th->rtSched = 0;
    th->callbackTime = 0;
    th->callbackCpuTime = 0;
    th->useWatchdog = 1;
    th->throttledSleepTime = 0;
    th->cpuLoadMeasurer = clm;

    th->rtPrio = (sched_get_priority_max( SCHED_FIFO ) - sched_get_priority_min( SCHED_FIFO )) / 2
            + sched_get_priority_min( SCHED_FIFO );
}

static PaError KillCallbackThread( PaAlsaThreading *th, int wait, PaError *exitResult, PaError *watchdogExitResult )
{
    PaError result = paNoError;
    void *pret;

    if( exitResult )
        *exitResult = paNoError;
    if( watchdogExitResult )
        *watchdogExitResult = paNoError;

    if( th->watchdogRunning )
    {
        pthread_cancel( th->watchdogThread );
        ENSURE_SYSTEM_( pthread_join( th->watchdogThread, &pret ), 0 );

        if( pret && pret != PTHREAD_CANCELED )
        {
            if( watchdogExitResult )
                *watchdogExitResult = *(PaError *) pret;
            free( pret );
        }
    }

    /* Only kill the thread if it isn't in the process of stopping (flushing adaptation buffers) */
    /* TODO: Make join time out */
    if( !wait )
    {
        PA_DEBUG(( "%s: Canceling thread %d\n", __FUNCTION__, th->callbackThread ));
        pthread_cancel( th->callbackThread );   /* XXX: Safe to call this if the thread has exited on its own? */
    }
    PA_DEBUG(( "%s: Joining thread %d\n", __FUNCTION__, th->callbackThread ));
    ENSURE_SYSTEM_( pthread_join( th->callbackThread, &pret ), 0 );

    if( pret && pret != PTHREAD_CANCELED )
    {
        if( exitResult )
            *exitResult = *(PaError *) pret;
        free( pret );
    }

error:
    return result;
}

/** Lock a pthread_mutex_t.
 *
 * @concern ThreadCancellation We're disabling thread cancellation while the thread is holding a lock, so mutexes are 
 * properly unlocked at termination time.
 */
static PaError LockMutex( pthread_mutex_t *mtx )
{
    PaError result = paNoError;
    int oldState;
    
    ENSURE_SYSTEM_( pthread_setcancelstate( PTHREAD_CANCEL_DISABLE, &oldState ), 0 );
    ENSURE_SYSTEM_( pthread_mutex_lock( mtx ), 0 );

error:
    return result;
}

/** Unlock a pthread_mutex_t.
 *
 * @concern ThreadCancellation Thread cancellation is enabled again after the mutex is properly unlocked.
 */
static PaError UnlockMutex( pthread_mutex_t *mtx )
{
    PaError result = paNoError;
    int oldState;

    ENSURE_SYSTEM_( pthread_mutex_unlock( mtx ), 0 );
    ENSURE_SYSTEM_( pthread_setcancelstate( PTHREAD_CANCEL_ENABLE, &oldState ), 0 );

error:
    return result;
}

static void OnWatchdogExit( void *userData )
{
    PaAlsaThreading *th = (PaAlsaThreading *) userData;
    struct sched_param spm = { 0 };
    assert( th );

    ASSERT_CALL_( pthread_setschedparam( th->callbackThread, SCHED_OTHER, &spm ), 0 );    /* Lower before exiting */
    PA_DEBUG(( "Watchdog exiting\n" ));
}

static PaError BoostPriority( PaAlsaThreading *th )
{
    PaError result = paNoError;
    struct sched_param spm = { 0 };
    spm.sched_priority = th->rtPrio;

    assert( th );

    if( pthread_setschedparam( th->callbackThread, SCHED_FIFO, &spm ) != 0 )
    {
        PA_UNLESS( errno == EPERM, paInternalError );  /* Lack permission to raise priority */
        PA_DEBUG(( "Failed bumping priority\n" ));
        result = 0;
    }
    else
        result = 1; /* Success */
error:
    return result;
}

static void *WatchdogFunc( void *userData )
{
    PaError result = paNoError, *pres = NULL;
    int err;
    PaAlsaThreading *th = (PaAlsaThreading *) userData;
    unsigned intervalMsec = 500;
    const PaTime maxSeconds = 3.;   /* Max seconds between callbacks */
    PaTime timeThen = PaUtil_GetTime(), timeNow, timeElapsed, cpuTimeThen, cpuTimeNow, cpuTimeElapsed;
    double cpuLoad, avgCpuLoad = 0.;
    int throttled = 0;

    assert( th );

    /* Execute OnWatchdogExit when exiting */
    pthread_cleanup_push( &OnWatchdogExit, th );

    /* Boost priority of callback thread */
    PA_ENSURE( result = BoostPriority( th ) );
    if( !result )
    {
        /* Boost failed, might as well exit */
        pthread_exit( NULL );
    }

    cpuTimeThen = th->callbackCpuTime;
    {
        int policy;
        struct sched_param spm = { 0 };
        pthread_getschedparam( pthread_self(), &policy, &spm );
        PA_DEBUG(( "%s: Watchdog priority is %d\n", __FUNCTION__, spm.sched_priority ));
    }

    while( 1 )
    {
        double lowpassCoeff = 0.9, lowpassCoeff1 = 0.99999 - lowpassCoeff;
        
        /* Test before and after in case whatever underlying sleep call isn't interrupted by pthread_cancel */
        pthread_testcancel();
        Pa_Sleep( intervalMsec );
        pthread_testcancel();

        if( PaUtil_GetTime() - th->callbackTime > maxSeconds )
        {
            PA_DEBUG(( "Watchdog: Terminating callback thread\n" ));
            /* Tell thread to terminate */
            err = pthread_kill( th->callbackThread, SIGKILL );
            pthread_exit( NULL );
        }

        PA_DEBUG(( "%s: PortAudio reports CPU load: %g\n", __FUNCTION__, PaUtil_GetCpuLoad( th->cpuLoadMeasurer ) ));

        /* Check if we should throttle, or unthrottle :P */
        cpuTimeNow = th->callbackCpuTime;
        cpuTimeElapsed = cpuTimeNow - cpuTimeThen;
        cpuTimeThen = cpuTimeNow;

        timeNow = PaUtil_GetTime();
        timeElapsed = timeNow - timeThen;
        timeThen = timeNow;
        cpuLoad = cpuTimeElapsed / timeElapsed;
        avgCpuLoad = avgCpuLoad * lowpassCoeff + cpuLoad * lowpassCoeff1;
        /*
        if( throttled )
            PA_DEBUG(( "Watchdog: CPU load: %g, %g\n", avgCpuLoad, cpuTimeElapsed ));
            */
        if( PaUtil_GetCpuLoad( th->cpuLoadMeasurer ) > .925 )
        {
            static int policy;
            static struct sched_param spm = { 0 };
            static const struct sched_param defaultSpm = { 0 };
            PA_DEBUG(( "%s: Throttling audio thread, priority %d\n", __FUNCTION__, spm.sched_priority ));

            pthread_getschedparam( th->callbackThread, &policy, &spm );
            if( !pthread_setschedparam( th->callbackThread, SCHED_OTHER, &defaultSpm ) )
            {
                throttled = 1;
            }
            else
                PA_DEBUG(( "Watchdog: Couldn't lower priority of audio thread: %s\n", strerror( errno ) ));

            /* Give other processes a go, before raising priority again */
            PA_DEBUG(( "%s: Watchdog sleeping for %lu msecs before unthrottling\n", __FUNCTION__, th->throttledSleepTime ));
            Pa_Sleep( th->throttledSleepTime );

            /* Reset callback priority */
            if( pthread_setschedparam( th->callbackThread, SCHED_FIFO, &spm ) != 0 )
            {
                PA_DEBUG(( "%s: Couldn't raise priority of audio thread: %s\n", __FUNCTION__, strerror( errno ) ));
            }

            if( PaUtil_GetCpuLoad( th->cpuLoadMeasurer ) >= .99 )
                intervalMsec = 50;
            else
                intervalMsec = 100;

            /*
            lowpassCoeff = .97;
            lowpassCoeff1 = .99999 - lowpassCoeff;
            */
        }
        else if( throttled && avgCpuLoad < .8 )
        {
            intervalMsec = 500;
            throttled = 0;

            /*
            lowpassCoeff = .9;
            lowpassCoeff1 = .99999 - lowpassCoeff;
            */
        }
    }

    pthread_cleanup_pop( 1 );   /* Execute cleanup on exit */

error:
    /* Shouldn't get here in the normal case */

    /* Pass on error code */
    pres = malloc( sizeof (PaError) );
    *pres = result;
    
    pthread_exit( pres );
}

static PaError CreateCallbackThread( PaAlsaThreading *th, void *(*callbackThreadFunc)( void * ), PaStream *s )
{
    PaError result = paNoError;
    pthread_attr_t attr;
    int started = 0;

#if defined _POSIX_MEMLOCK && (_POSIX_MEMLOCK != -1)
    if( th->rtSched )
    {
        if( mlockall( MCL_CURRENT | MCL_FUTURE ) < 0 )
        {
            int savedErrno = errno;             /* In case errno gets overwritten */
            assert( savedErrno != EINVAL );     /* Most likely a programmer error */
            PA_UNLESS( (savedErrno == EPERM), paInternalError );
            PA_DEBUG(( "%s: Failed locking memory\n", __FUNCTION__ ));
        }
        else
            PA_DEBUG(( "%s: Successfully locked memory\n", __FUNCTION__ ));
    }
#endif

    PA_UNLESS( !pthread_attr_init( &attr ), paInternalError );
    /* Priority relative to other processes */
    PA_UNLESS( !pthread_attr_setscope( &attr, PTHREAD_SCOPE_SYSTEM ), paInternalError );   

    PA_UNLESS( !pthread_create( &th->callbackThread, &attr, callbackThreadFunc, s ), paInternalError );
    started = 1;

    if( th->rtSched )
    {
        if( th->useWatchdog )
        {
            int err;
            struct sched_param wdSpm = { 0 };
            /* Launch watchdog, watchdog sets callback thread priority */
            int prio = PA_MIN( th->rtPrio + 4, sched_get_priority_max( SCHED_FIFO ) );
            wdSpm.sched_priority = prio;

            PA_UNLESS( !pthread_attr_init( &attr ), paInternalError );
            PA_UNLESS( !pthread_attr_setinheritsched( &attr, PTHREAD_EXPLICIT_SCHED ), paInternalError );
            PA_UNLESS( !pthread_attr_setscope( &attr, PTHREAD_SCOPE_SYSTEM ), paInternalError );
            PA_UNLESS( !pthread_attr_setschedpolicy( &attr, SCHED_FIFO ), paInternalError );
            PA_UNLESS( !pthread_attr_setschedparam( &attr, &wdSpm ), paInternalError );
            if( (err = pthread_create( &th->watchdogThread, &attr, &WatchdogFunc, th )) )
            {
                PA_UNLESS( err == EPERM, paInternalError );
                /* Permission error, go on without realtime privileges */
                PA_DEBUG(( "Failed bumping priority\n" ));
            }
            else
            {
                int policy;
                th->watchdogRunning = 1;
                ENSURE_SYSTEM_( pthread_getschedparam( th->watchdogThread, &policy, &wdSpm ), 0 );
                /* Check if priority is right, policy could potentially differ from SCHED_FIFO (but that's alright) */
                if( wdSpm.sched_priority != prio )
                {
                    PA_DEBUG(( "Watchdog priority not set correctly (%d)\n", wdSpm.sched_priority ));
                    PA_ENSURE( paInternalError );
                }
            }
        }
        else
            PA_ENSURE( BoostPriority( th ) );
    }

end:
    return result;
error:
    if( started )
        KillCallbackThread( th, 0, NULL, NULL );

    goto end;
}

static void CallbackUpdate( PaAlsaThreading *th )
{
    th->callbackTime = PaUtil_GetTime();
    th->callbackCpuTime = PaUtil_GetCpuLoad( th->cpuLoadMeasurer );
}

/* prototypes for functions declared in this file */

static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError BuildDeviceList( PaAlsaHostApiRepresentation *hostApi );
static int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate );
static int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate );

/* Callback prototypes */
static void *CallbackThreadFunc( void *userData );

/* Blocking prototypes */
static signed long GetStreamReadAvailable( PaStream* s );
static signed long GetStreamWriteAvailable( PaStream* s );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );


static const PaAlsaDeviceInfo *GetDeviceInfo( const PaUtilHostApiRepresentation *hostApi, int device )
{
    return (const PaAlsaDeviceInfo *)hostApi->deviceInfos[device];
}

static void AlsaErrorHandler(const char *file, int line, const char *function, int err, const char *fmt, ...)
{
}

PaError PaAlsa_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaAlsaHostApiRepresentation *alsaHostApi = NULL;

    PA_UNLESS( alsaHostApi = (PaAlsaHostApiRepresentation*) PaUtil_AllocateMemory(
                sizeof(PaAlsaHostApiRepresentation) ), paInsufficientMemory );
    PA_UNLESS( alsaHostApi->allocations = PaUtil_CreateAllocationGroup(), paInsufficientMemory );
    alsaHostApi->hostApiIndex = hostApiIndex;

    *hostApi = (PaUtilHostApiRepresentation*)alsaHostApi;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paALSA;
    (*hostApi)->info.name = "ALSA";

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    ENSURE_( snd_lib_error_set_handler(AlsaErrorHandler), paUnanticipatedHostError );

    PA_ENSURE( BuildDeviceList( alsaHostApi ) );

    PaUtil_InitializeStreamInterface( &alsaHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &alsaHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

    return result;

error:
    if( alsaHostApi )
    {
        if( alsaHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( alsaHostApi->allocations );
            PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
        }

        PaUtil_FreeMemory( alsaHostApi );
    }

    return result;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaAlsaHostApiRepresentation *alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;

    assert( hostApi );

    if( alsaHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( alsaHostApi->allocations );
        PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
    }

    PaUtil_FreeMemory( alsaHostApi );
    snd_config_update_free_global();
}

/** Determine max channels and default latencies.
 *
 * This function provides functionality to grope an opened (might be opened for capture or playback) pcm device for 
 * traits like max channels, suitable default latencies and default sample rate. Upon error, max channels is set to zero,
 * and a suitable result returned. The device is closed before returning.
 */
static PaError GropeDevice( snd_pcm_t* pcm, int isPlug, StreamDirection mode, int openBlocking,
        PaAlsaDeviceInfo* devInfo, int* canMmap )
{
    PaError result = paNoError;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_uframes_t lowLatency = 512, highLatency = 2048;
    unsigned int minChans, maxChans;
    int* minChannels, * maxChannels;
    double * defaultLowLatency, * defaultHighLatency, * defaultSampleRate =
        &devInfo->baseDeviceInfo.defaultSampleRate;
    double defaultSr = *defaultSampleRate;

    assert( pcm );

    if( StreamDirection_In == mode )
    {
        minChannels = &devInfo->minInputChannels;
        maxChannels = &devInfo->baseDeviceInfo.maxInputChannels;
        defaultLowLatency = &devInfo->baseDeviceInfo.defaultLowInputLatency;
        defaultHighLatency = &devInfo->baseDeviceInfo.defaultHighInputLatency;
    }
    else
    {
        minChannels = &devInfo->minOutputChannels;
        maxChannels = &devInfo->baseDeviceInfo.maxOutputChannels;
        defaultLowLatency = &devInfo->baseDeviceInfo.defaultLowOutputLatency;
        defaultHighLatency = &devInfo->baseDeviceInfo.defaultHighOutputLatency;
    }

    ENSURE_( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

    snd_pcm_hw_params_alloca( &hwParams );
    snd_pcm_hw_params_any( pcm, hwParams );

    *canMmap = snd_pcm_hw_params_test_access( pcm, hwParams, SND_PCM_ACCESS_MMAP_INTERLEAVED ) >= 0 ||
            snd_pcm_hw_params_test_access( pcm, hwParams, SND_PCM_ACCESS_MMAP_NONINTERLEAVED ) >= 0;

    if( defaultSr >= 0 )
    {
        /* Could be that the device opened in one mode supports samplerates that the other mode wont have,
         * so try again .. */
        if( SetApproximateSampleRate( pcm, hwParams, defaultSr ) < 0 )
        {
            defaultSr = -1.;
            PA_DEBUG(( "%s: Original default samplerate failed, trying again ..\n", __FUNCTION__ ));
        }
    }

    if( defaultSr < 0. )           /* Default sample rate not set */
    {
        unsigned int sampleRate = 44100;        /* Will contain approximate rate returned by alsa-lib */
        if( snd_pcm_hw_params_set_rate_near( pcm, hwParams, &sampleRate, NULL ) < 0)
        {
            result = paUnanticipatedHostError;
            goto error;
        }
        ENSURE_( GetExactSampleRate( hwParams, &defaultSr ), paUnanticipatedHostError );
    }
    
    ENSURE_( snd_pcm_hw_params_get_channels_min( hwParams, &minChans ), paUnanticipatedHostError );
    ENSURE_( snd_pcm_hw_params_get_channels_max( hwParams, &maxChans ), paUnanticipatedHostError );
    assert( maxChans <= INT_MAX );
    assert( maxChans > 0 );    /* Weird linking issue could cause wrong version of ALSA symbols to be called,
                                   resulting in zeroed values */

    /* XXX: Limit to sensible number (ALSA plugins accept a crazy amount of channels)? */
    if( isPlug && maxChans > 128 )
    {
        maxChans = 128;
        PA_DEBUG(( "%s: Limiting number of plugin channels to %u\n", __FUNCTION__, maxChans ));
    }

    /* TWEAKME:
     *
     * Giving values for default min and max latency is not
     * straightforward.  Here are our objectives:
     *
     *         * for low latency, we want to give the lowest value
     *         that will work reliably.  This varies based on the
     *         sound card, kernel, CPU, etc.  I think it is better
     *         to give sub-optimal latency than to give a number
     *         too low and cause dropouts.  My conservative
     *         estimate at this point is to base it on 4096-sample
     *         latency at 44.1 kHz, which gives a latency of 23ms.
     *         * for high latency we want to give a large enough
     *         value that dropouts are basically impossible.  This
     *         doesn't really require as much tweaking, since
     *         providing too large a number will just cause us to
     *         select the nearest setting that will work at stream
     *         config time.
     */
    ENSURE_( snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &lowLatency ), paUnanticipatedHostError );

    /* Have to reset hwParams, to set new buffer size */
    ENSURE_( snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError ); 
    ENSURE_( snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &highLatency ), paUnanticipatedHostError );

    *minChannels = (int)minChans;
    *maxChannels = (int)maxChans;
    *defaultSampleRate = defaultSr;
    *defaultLowLatency = (double) lowLatency / *defaultSampleRate;
    *defaultHighLatency = (double) highLatency / *defaultSampleRate;

end:
    snd_pcm_close( pcm );
    return result;

error:
    goto end;
}

/* Initialize device info with invalid values (maxInputChannels and maxOutputChannels are set to zero since these indicate
 * wether input/output is available) */
static void InitializeDeviceInfo( PaDeviceInfo *deviceInfo )
{
    deviceInfo->structVersion = -1;
    deviceInfo->name = NULL;
    deviceInfo->hostApi = -1;
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 0;
    deviceInfo->defaultLowInputLatency = -1.;
    deviceInfo->defaultLowOutputLatency = -1.;
    deviceInfo->defaultHighInputLatency = -1.;
    deviceInfo->defaultHighOutputLatency = -1.;
    deviceInfo->defaultSampleRate = -1.;
}

/* Helper struct */
typedef struct
{
    char *alsaName;
    char *name;
    int isPlug;
    int hasPlayback;
    int hasCapture;
} DeviceNames;

static PaError PaAlsa_StrDup( PaAlsaHostApiRepresentation *alsaApi,
        char **dst,
        const char *src)
{
    PaError result = paNoError;
    int len = strlen( src ) + 1;

    /* PA_DEBUG(("PaStrDup %s %d\n", src, len)); */

    PA_UNLESS( *dst = (char *)PaUtil_GroupAllocateMemory( alsaApi->allocations, len ),
            paInsufficientMemory );
    strncpy( *dst, src, len );

error:
    return result;
}

/* Disregard some standard plugins
 */
static int IgnorePlugin( const char *pluginId )
{
    /* XXX: dmix and default ignored because after opening and closing, they seem to keep hogging resources.
     */
    static const char *ignoredPlugins[] = {"hw", "plughw", "plug", "dsnoop", "tee",
        "file", "null", "shm", "cards", "dmix", "default", NULL};
    int i = 0;
    while( ignoredPlugins[i] )
    {
        if( !strcmp( pluginId, ignoredPlugins[i] ) )
        {
            return 1;
        }
        ++i;
    }

    return 0;
}

/* Build PaDeviceInfo list, ignore devices for which we cannot determine capabilities (possibly busy, sigh) */
static PaError BuildDeviceList( PaAlsaHostApiRepresentation *alsaApi )
{
    PaUtilHostApiRepresentation *baseApi = &alsaApi->baseHostApiRep;
    PaAlsaDeviceInfo *deviceInfoArray;
    int cardIdx = -1, devIdx = 0;
    snd_ctl_card_info_t *cardInfo;
    PaError result = paNoError;
    size_t numDeviceNames = 0, maxDeviceNames = 1, i;
    DeviceNames *deviceNames = NULL;
    snd_config_t *topNode = NULL;
    snd_pcm_info_t *pcmInfo;
    int res;
    int blocking = SND_PCM_NONBLOCK;
    char alsaCardName[50];
    if( getenv( "PA_ALSA_INITIALIZE_BLOCK" ) && atoi( getenv( "PA_ALSA_INITIALIZE_BLOCK" ) ) )
        blocking = 0;

    /* These two will be set to the first working input and output device, respectively */
    baseApi->info.defaultInputDevice = paNoDevice;
    baseApi->info.defaultOutputDevice = paNoDevice;

    /* count the devices by enumerating all the card numbers */

    /* snd_card_next() modifies the integer passed to it to be:
     *      the index of the first card if the parameter is -1
     *      the index of the next card if the parameter is the index of a card
     *      -1 if there are no more cards
     *
     * The function itself returns 0 if it succeeded. */
    cardIdx = -1;
    snd_ctl_card_info_alloca( &cardInfo );
    snd_pcm_info_alloca( &pcmInfo );
    while( snd_card_next( &cardIdx ) == 0 && cardIdx >= 0 )
    {
        char *cardName;
        int devIdx = -1;
        snd_ctl_t *ctl;
        char buf[50];

        snprintf( alsaCardName, sizeof (alsaCardName), "hw:%d", cardIdx );

        /* Acquire name of card */
        if( snd_ctl_open( &ctl, alsaCardName, 0 ) < 0 )
        {
            /* Unable to open card :( */
            continue;
        }
        snd_ctl_card_info( ctl, cardInfo );

        PA_ENSURE( PaAlsa_StrDup( alsaApi, &cardName, snd_ctl_card_info_get_name( cardInfo )) );

        while( snd_ctl_pcm_next_device( ctl, &devIdx ) == 0 && devIdx >= 0 )
        {
            char *alsaDeviceName, *deviceName;
            size_t len;
            int hasPlayback = 0, hasCapture = 0;
            snprintf( buf, sizeof (buf), "%s:%d,%d", "hw", cardIdx, devIdx );

            /* Obtain info about this particular device */
            snd_pcm_info_set_device( pcmInfo, devIdx );
            snd_pcm_info_set_subdevice( pcmInfo, 0 );
            snd_pcm_info_set_stream( pcmInfo, SND_PCM_STREAM_CAPTURE );
            if( snd_ctl_pcm_info( ctl, pcmInfo ) >= 0 )
            {
                hasCapture = 1;
            }
            
            snd_pcm_info_set_stream( pcmInfo, SND_PCM_STREAM_PLAYBACK );
            if( snd_ctl_pcm_info( ctl, pcmInfo ) >= 0 )
            {
                hasPlayback = 1;
            }

            if( !hasPlayback && !hasCapture )
            {
                continue;   /* Error */
            }

            /* The length of the string written by snprintf plus terminating 0 */
            len = snprintf( NULL, 0, "%s: %s (%s)", cardName, snd_pcm_info_get_name( pcmInfo ), buf ) + 1;
            PA_UNLESS( deviceName = (char *)PaUtil_GroupAllocateMemory( alsaApi->allocations, len ),
                    paInsufficientMemory );
            snprintf( deviceName, len, "%s: %s (%s)", cardName,
                    snd_pcm_info_get_name( pcmInfo ), buf );

            ++numDeviceNames;
            if( !deviceNames || numDeviceNames > maxDeviceNames )
            {
                maxDeviceNames *= 2;
                PA_UNLESS( deviceNames = (DeviceNames *) realloc( deviceNames, maxDeviceNames * sizeof (DeviceNames) ),
                        paInsufficientMemory );
            }

            PA_ENSURE( PaAlsa_StrDup( alsaApi, &alsaDeviceName, buf ) );

            deviceNames[ numDeviceNames - 1 ].alsaName = alsaDeviceName;
            deviceNames[ numDeviceNames - 1 ].name = deviceName;
            deviceNames[ numDeviceNames - 1 ].isPlug = 0;
            deviceNames[ numDeviceNames - 1 ].hasPlayback = hasPlayback;
            deviceNames[ numDeviceNames - 1 ].hasCapture = hasCapture;
        }
        snd_ctl_close( ctl );
    }

    /* Iterate over plugin devices */
    if( NULL == snd_config )
    {
        /* snd_config_update is called implicitly by some functions, if this hasn't happened snd_config will be NULL (bleh) */
        ENSURE_( snd_config_update(), paUnanticipatedHostError );
        PA_DEBUG(( "Updating snd_config\n" ));
    }
    assert( snd_config );
    if( (res = snd_config_search( snd_config, "pcm", &topNode )) >= 0 )
    {
        snd_config_iterator_t i, next;

        snd_config_for_each( i, next, topNode )
        {
            const char *tpStr = "unknown", *idStr = NULL;
            int err = 0;

            char *alsaDeviceName, *deviceName;
            snd_config_t *n = snd_config_iterator_entry( i ), * tp = NULL;;

            if( (err = snd_config_search( n, "type", &tp )) < 0 )
            {
                if( -ENOENT != err )
                {
                    ENSURE_(err, paUnanticipatedHostError);
                }
            }
            else 
            {
                ENSURE_( snd_config_get_string( tp, &tpStr ), paUnanticipatedHostError );
            }
            ENSURE_( snd_config_get_id( n, &idStr ), paUnanticipatedHostError );
            if( IgnorePlugin( idStr ) )
            {
                PA_DEBUG(( "%s: Ignoring ALSA plugin device %s of type %s\n", __FUNCTION__, idStr, tpStr ));
                continue;
            }
            PA_DEBUG(( "%s: Found plugin %s of type %s\n", __FUNCTION__, idStr, tpStr ));

            PA_UNLESS( alsaDeviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                            strlen(idStr) + 6 ), paInsufficientMemory );
            strcpy( alsaDeviceName, idStr );
            PA_UNLESS( deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                            strlen(idStr) + 1 ), paInsufficientMemory );
            strcpy( deviceName, idStr );

            ++numDeviceNames;
            if( !deviceNames || numDeviceNames > maxDeviceNames )
            {
                maxDeviceNames *= 2;
                PA_UNLESS( deviceNames = (DeviceNames *) realloc( deviceNames, maxDeviceNames * sizeof (DeviceNames) ),
                        paInsufficientMemory );
            }

            deviceNames[numDeviceNames - 1].alsaName = alsaDeviceName;
            deviceNames[numDeviceNames - 1].name = deviceName;
            deviceNames[numDeviceNames - 1].isPlug = 1;
            deviceNames[numDeviceNames - 1].hasPlayback = 1;
            deviceNames[numDeviceNames - 1].hasCapture = 1;
        }
    }
    else
        PA_DEBUG(( "%s: Iterating over ALSA plugins failed: %s\n", __FUNCTION__, snd_strerror( res ) ));

    /* allocate deviceInfo memory based on the number of devices */
    PA_UNLESS( baseApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo*) * (numDeviceNames) ), paInsufficientMemory );

    /* allocate all device info structs in a contiguous block */
    PA_UNLESS( deviceInfoArray = (PaAlsaDeviceInfo*)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaAlsaDeviceInfo) * numDeviceNames ), paInsufficientMemory );

    /* Loop over list of cards, filling in info, if a device is deemed unavailable (can't get name),
     * it's ignored.
     */
    /* while( snd_card_next( &cardIdx ) == 0 && cardIdx >= 0 ) */
    for( i = 0, devIdx = 0; i < numDeviceNames; ++i )
    {
        snd_pcm_t *pcm;
        PaAlsaDeviceInfo *deviceInfo = &deviceInfoArray[devIdx];
        PaDeviceInfo *baseDeviceInfo = &deviceInfo->baseDeviceInfo;
        int canMmap = -1;

        /* Zero fields */
        InitializeDeviceInfo( baseDeviceInfo );

        /* to determine device capabilities, we must open the device and query the
         * hardware parameter configuration space */

        /* Query capture */
        if( deviceNames[i].hasCapture &&
                snd_pcm_open( &pcm, deviceNames[i].alsaName, SND_PCM_STREAM_CAPTURE, blocking ) >= 0 )
        {
            if( GropeDevice( pcm, deviceNames[i].isPlug, StreamDirection_In, blocking, deviceInfo,
                        &canMmap ) != paNoError )
            {
                /* Error */
                PA_DEBUG(("%s: Failed groping %s for capture\n", __FUNCTION__, deviceNames[i].alsaName));
                continue;
            }
        }

        /* Query playback */
        if( deviceNames[i].hasPlayback &&
                snd_pcm_open( &pcm, deviceNames[i].alsaName, SND_PCM_STREAM_PLAYBACK, blocking ) >= 0 )
        {
            if( GropeDevice( pcm, deviceNames[i].isPlug, StreamDirection_Out, blocking, deviceInfo,
                        &canMmap ) != paNoError )
            {
                /* Error */
                PA_DEBUG(("%s: Failed groping %s for playback\n", __FUNCTION__, deviceNames[i].alsaName));
                continue;
            }
        }

        if( 0 == canMmap )
        {
            PA_DEBUG(("%s: Device %s doesn't support mmap\n", __FUNCTION__, deviceNames[i].alsaName));
            continue;
        }

        baseDeviceInfo->structVersion = 2;
        baseDeviceInfo->hostApi = alsaApi->hostApiIndex;
        baseDeviceInfo->name = deviceNames[i].name;
        deviceInfo->alsaName = deviceNames[i].alsaName;
        deviceInfo->isPlug = deviceNames[i].isPlug;

        /* A: Storing pointer to PaAlsaDeviceInfo object as pointer to PaDeviceInfo object.
         * Should now be safe to add device info, unless the device supports neither capture nor playback
         */
        if( baseDeviceInfo->maxInputChannels > 0 || baseDeviceInfo->maxOutputChannels > 0 )
        {
            if( baseApi->info.defaultInputDevice == paNoDevice && baseDeviceInfo->maxInputChannels > 0 )
                baseApi->info.defaultInputDevice = devIdx;
            if(  baseApi->info.defaultOutputDevice == paNoDevice && baseDeviceInfo->maxOutputChannels > 0 )
                baseApi->info.defaultOutputDevice = devIdx;
            PA_DEBUG(("%s: Adding device %s\n", __FUNCTION__, deviceNames[i].name));
            baseApi->deviceInfos[devIdx++] = (PaDeviceInfo *) deviceInfo;
        }
    }
    free( deviceNames );

    baseApi->info.deviceCount = devIdx;   /* Number of successfully queried devices */

end:
    return result;

error:
    /* No particular action */
    goto end;
}

/* Check against known device capabilities */
static PaError ValidateParameters( const PaStreamParameters *parameters, PaUtilHostApiRepresentation *hostApi, StreamDirection mode )
{
    PaError result = paNoError;
    int maxChans;
    const PaAlsaDeviceInfo *deviceInfo = NULL;
    assert( parameters );

    if( parameters->device != paUseHostApiSpecificDeviceSpecification )
    {
        assert( parameters->device < hostApi->info.deviceCount );
        PA_UNLESS( parameters->hostApiSpecificStreamInfo == NULL, paBadIODeviceCombination );
        deviceInfo = GetDeviceInfo( hostApi, parameters->device );
    }
    else
    {
        const PaAlsaStreamInfo *streamInfo = parameters->hostApiSpecificStreamInfo;

        PA_UNLESS( parameters->device == paUseHostApiSpecificDeviceSpecification, paInvalidDevice );
        PA_UNLESS( streamInfo->size == sizeof (PaAlsaStreamInfo) && streamInfo->version == 1,
                paIncompatibleHostApiSpecificStreamInfo );
        PA_UNLESS( streamInfo->deviceString != NULL, paInvalidDevice );

        /* Skip further checking */
        return paNoError;
    }

    assert( deviceInfo );
    assert( parameters->hostApiSpecificStreamInfo == NULL );
    maxChans = (StreamDirection_In == mode ? deviceInfo->baseDeviceInfo.maxInputChannels :
        deviceInfo->baseDeviceInfo.maxOutputChannels);
    PA_UNLESS( parameters->channelCount <= maxChans, paInvalidChannelCount );

error:
    return result;
}

/* Given an open stream, what sample formats are available? */
static PaSampleFormat GetAvailableFormats( snd_pcm_t *pcm )
{
    PaSampleFormat available = 0;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_hw_params_alloca( &hwParams );

    snd_pcm_hw_params_any( pcm, hwParams );

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT ) >= 0)
        available |= paFloat32;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S32 ) >= 0)
        available |= paInt32;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24 ) >= 0)
        available |= paInt24;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16 ) >= 0)
        available |= paInt16;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U8 ) >= 0)
        available |= paUInt8;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S8 ) >= 0)
        available |= paInt8;

    return available;
}

static snd_pcm_format_t Pa2AlsaFormat( PaSampleFormat paFormat )
{
    switch( paFormat )
    {
        case paFloat32:
            return SND_PCM_FORMAT_FLOAT;

        case paInt16:
            return SND_PCM_FORMAT_S16;

        case paInt24:
            return SND_PCM_FORMAT_S24;

        case paInt32:
            return SND_PCM_FORMAT_S32;

        case paInt8:
            return SND_PCM_FORMAT_S8;

        case paUInt8:
            return SND_PCM_FORMAT_U8;

        default:
            return SND_PCM_FORMAT_UNKNOWN;
    }
}

/** Open an ALSA pcm handle.
 * 
 * The device to be open can be specified in a custom PaAlsaStreamInfo struct, or it will be a device number. In case of a
 * device number, it maybe specified through an env variable (PA_ALSA_PLUGHW) that we should open the corresponding plugin
 * device.
 */
static PaError AlsaOpen( const PaUtilHostApiRepresentation *hostApi, const PaStreamParameters *params, StreamDirection
        streamDir, snd_pcm_t **pcm )
{
    PaError result = paNoError;
    int ret;
    const char *deviceName = alloca( 50 );
    const PaAlsaDeviceInfo *deviceInfo = NULL;
    PaAlsaStreamInfo *streamInfo = (PaAlsaStreamInfo *)params->hostApiSpecificStreamInfo;

    if( !streamInfo )
    {
        int usePlug = 0;
        deviceInfo = GetDeviceInfo( hostApi, params->device );
        
        /* If device name starts with hw: and PA_ALSA_PLUGHW is 1, we open the plughw device instead */
        if( !strncmp( "hw:", deviceInfo->alsaName, 3 ) && getenv( "PA_ALSA_PLUGHW" ) )
            usePlug = atoi( getenv( "PA_ALSA_PLUGHW" ) );
        if( usePlug )
            snprintf( (char *) deviceName, 50, "plug%s", deviceInfo->alsaName );
        else
            deviceName = deviceInfo->alsaName;
    }
    else
        deviceName = streamInfo->deviceString;

    PA_DEBUG(( "%s: Opening device %s\n", __FUNCTION__, deviceName ));
    if( (ret = snd_pcm_open( pcm, deviceName, streamDir == StreamDirection_In ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK,
                    SND_PCM_NONBLOCK )) < 0 )
    {
        /* Not to be closed */
        *pcm = NULL;
        ENSURE_( ret, ret == -EBUSY ? paDeviceUnavailable : paBadIODeviceCombination );
    }
    ENSURE_( snd_pcm_nonblock( *pcm, 0 ), paUnanticipatedHostError );

end:
    return result;

error:
    goto end;
}

static PaError TestParameters( const PaUtilHostApiRepresentation *hostApi, const PaStreamParameters *parameters,
        double sampleRate, StreamDirection streamDir )
{
    PaError result = paNoError;
    snd_pcm_t *pcm = NULL;
    PaSampleFormat availableFormats;
    /* We are able to adapt to a number of channels less than what the device supports */
    unsigned int numHostChannels;
    PaSampleFormat hostFormat;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_hw_params_alloca( &hwParams );
    
    if( !parameters->hostApiSpecificStreamInfo )
    {
        const PaAlsaDeviceInfo *devInfo = GetDeviceInfo( hostApi, parameters->device );
        numHostChannels = PA_MAX( parameters->channelCount, StreamDirection_In == streamDir ?
                devInfo->minInputChannels : devInfo->minOutputChannels );
    }
    else
        numHostChannels = parameters->channelCount;

    PA_ENSURE( AlsaOpen( hostApi, parameters, streamDir, &pcm ) );

    snd_pcm_hw_params_any( pcm, hwParams );

    if( SetApproximateSampleRate( pcm, hwParams, sampleRate ) < 0 )
    {
        result = paInvalidSampleRate;
        goto error;
    }

    if( snd_pcm_hw_params_set_channels( pcm, hwParams, numHostChannels ) < 0 )
    {
        result = paInvalidChannelCount;
        goto error;
    }

    /* See if we can find a best possible match */
    availableFormats = GetAvailableFormats( pcm );
    PA_ENSURE( hostFormat = PaUtil_SelectClosestAvailableFormat( availableFormats, parameters->sampleFormat ) );
    ENSURE_( snd_pcm_hw_params_set_format( pcm, hwParams, Pa2AlsaFormat( hostFormat ) ), paUnanticipatedHostError );

    {
        /* It happens that this call fails because the device is busy */
        int ret = 0;
        if( (ret = snd_pcm_hw_params( pcm, hwParams )) < 0)
        {
            ENSURE_( ret, ret == -EBUSY ? paDeviceUnavailable : paUnanticipatedHostError );
        }
    }

end:
    if( pcm )
    {
        snd_pcm_close( pcm );
    }
    return result;

error:
    goto end;
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount = 0, outputChannelCount = 0;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaError result = paFormatIsSupported;

    if( inputParameters )
    {
        PA_ENSURE( ValidateParameters( inputParameters, hostApi, StreamDirection_In ) );

        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }

    if( outputParameters )
    {
        PA_ENSURE( ValidateParameters( outputParameters, hostApi, StreamDirection_Out ) );

        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
    }

    if( inputChannelCount )
    {
        if( (result = TestParameters( hostApi, inputParameters, sampleRate, StreamDirection_In ))
                != paNoError )
            goto error;
    }
    if ( outputChannelCount )
    {
        if( (result = TestParameters( hostApi, outputParameters, sampleRate, StreamDirection_Out ))
                != paNoError )
            goto error;
    }

    return paFormatIsSupported;

error:
    return result;
}

static PaError PaAlsaStreamComponent_Initialize( PaAlsaStreamComponent *self, PaAlsaHostApiRepresentation *alsaApi,
        const PaStreamParameters *params, StreamDirection streamDir, int callbackMode )
{
    PaError result = paNoError;
    PaSampleFormat userSampleFormat = params->sampleFormat, hostSampleFormat;
    assert( params->channelCount > 0 );

    /* Make sure things have an initial value */
    memset( self, 0, sizeof (PaAlsaStreamComponent) );

    if( NULL == params->hostApiSpecificStreamInfo )
    {
        const PaAlsaDeviceInfo *devInfo = GetDeviceInfo( &alsaApi->baseHostApiRep, params->device );
        self->numHostChannels = PA_MAX( params->channelCount, StreamDirection_In == streamDir ? devInfo->minInputChannels
                : devInfo->minOutputChannels );
    }
    else
    {
        /* We're blissfully unaware of the minimum channelCount */
        self->numHostChannels = params->channelCount;
    }

    PA_ENSURE( AlsaOpen( &alsaApi->baseHostApiRep, params, streamDir, &self->pcm ) );
    self->nfds = snd_pcm_poll_descriptors_count( self->pcm );
    hostSampleFormat = PaUtil_SelectClosestAvailableFormat( GetAvailableFormats( self->pcm ), userSampleFormat );

    self->hostSampleFormat = hostSampleFormat;
    self->nativeFormat = Pa2AlsaFormat( hostSampleFormat );
    self->hostInterleaved = self->userInterleaved = !(userSampleFormat & paNonInterleaved);
    self->numUserChannels = params->channelCount;
    self->streamDir = streamDir;

    if( !callbackMode && !self->userInterleaved )
    {
        /* Pre-allocate non-interleaved user provided buffers */
        PA_UNLESS( self->userBuffers = PaUtil_AllocateMemory( sizeof (void *) * self->numUserChannels ),
                paInsufficientMemory );
    }

error:
    return result;
}

static void PaAlsaStreamComponent_Terminate( PaAlsaStreamComponent *self )
{
    snd_pcm_close( self->pcm );
    if( self->userBuffers )
        PaUtil_FreeMemory( self->userBuffers );
}

int nearbyint_(float value) {
    if(  value - (int)value > .5 )
        return (int)ceil( value );
    return (int)floor( value );
}

/** Initiate configuration, preparing for determining a period size suitable for both capture and playback components.
 *
 */
static PaError PaAlsaStreamComponent_InitialConfigure( PaAlsaStreamComponent *self, const PaStreamParameters *params,
        int primeBuffers, snd_pcm_hw_params_t *hwParams, double *sampleRate )
{
    /* Configuration consists of setting all of ALSA's parameters.
     * These parameters come in two flavors: hardware parameters
     * and software paramters.  Hardware parameters will affect
     * the way the device is initialized, software parameters
     * affect the way ALSA interacts with me, the user-level client.
     */

    PaError result = paNoError;
    snd_pcm_access_t accessMode, alternateAccessMode;
    int dir = 0;
    snd_pcm_t *pcm = self->pcm;
    double sr = *sampleRate;
    unsigned int minPeriods = 2;

    /* self->framesPerBuffer = framesPerHostBuffer; */

    /* ... fill up the configuration space with all possibile
     * combinations of parameters this device will accept */
    ENSURE_( snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError );

    ENSURE_( snd_pcm_hw_params_set_periods_integer( pcm, hwParams ), paUnanticipatedHostError );
    /* I think there should be at least 2 periods (even though ALSA doesn't appear to enforce this) */
    dir = 0;
    ENSURE_( snd_pcm_hw_params_set_periods_min( pcm, hwParams, &minPeriods, &dir ), paUnanticipatedHostError );

    if( self->userInterleaved )
    {
        accessMode = SND_PCM_ACCESS_MMAP_INTERLEAVED;
        alternateAccessMode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
    }
    else
    {
        accessMode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
        alternateAccessMode = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    }
    /* If requested access mode fails, try alternate mode */
    if( snd_pcm_hw_params_set_access( pcm, hwParams, accessMode ) < 0 )
    {
        int err = 0;
        if( (err = snd_pcm_hw_params_set_access( pcm, hwParams, alternateAccessMode )) < 0)
        {
            result = paUnanticipatedHostError;
            if( -EINVAL == err )
            {
                PaUtil_SetLastHostErrorInfo( paALSA, err, "PA ALSA requires that a device supports mmap access" );
            }
            else
            {
                PaUtil_SetLastHostErrorInfo( paALSA, err, snd_strerror( err ) );
            }
            goto error;
        }
        /* Flip mode */
        self->hostInterleaved = !self->userInterleaved;
    }

    ENSURE_( snd_pcm_hw_params_set_format( pcm, hwParams, self->nativeFormat ), paUnanticipatedHostError );

    ENSURE_( SetApproximateSampleRate( pcm, hwParams, sr ), paInvalidSampleRate );
    ENSURE_( GetExactSampleRate( hwParams, &sr ), paUnanticipatedHostError );
    /* reject if there's no sample rate within 1% of the one requested */
    if( (fabs( *sampleRate - sr ) / *sampleRate) > 0.01 )
    {
        PA_DEBUG(("%s: Wanted %f, closest sample rate was %d\n", __FUNCTION__, sampleRate, sr ));                 
        PA_ENSURE( paInvalidSampleRate );
    }

    ENSURE_( snd_pcm_hw_params_set_channels( pcm, hwParams, self->numHostChannels ), paInvalidChannelCount );

    *sampleRate = sr;

end:
    return result;

error:
    /* No particular action */
    goto end;
}

/** Finish the configuration of the component's ALSA device.
 *
 * As part of this method, the component's bufferSize attribute will be set.
 * @param latency: The latency for this component.
 */
static PaError PaAlsaStreamComponent_FinishConfigure( PaAlsaStreamComponent *self, snd_pcm_hw_params_t* hwParams,
        const PaStreamParameters *params, int primeBuffers, double sampleRate, PaTime* latency )
{
    PaError result = paNoError;
    snd_pcm_sw_params_t* swParams;
    snd_pcm_uframes_t bufSz = 0;
    *latency = -1.;

    snd_pcm_sw_params_alloca( &swParams );

    bufSz = (params->suggestedLatency * sampleRate) + self->framesPerBuffer;    /* One period does not count as latency */
    ENSURE_( snd_pcm_hw_params_set_buffer_size_near( self->pcm, hwParams, &bufSz ), paUnanticipatedHostError );

    /* Set the parameters! */
    ENSURE_( snd_pcm_hw_params( self->pcm, hwParams ), paUnanticipatedHostError );
    ENSURE_( snd_pcm_hw_params_get_buffer_size( hwParams, &self->bufferSize ), paUnanticipatedHostError );
    /* Latency in seconds, one period is not counted as latency */
    *latency = (self->bufferSize - self->framesPerBuffer) / sampleRate;

    /* Now software parameters... */
    ENSURE_( snd_pcm_sw_params_current( self->pcm, swParams ), paUnanticipatedHostError );

    ENSURE_( snd_pcm_sw_params_set_start_threshold( self->pcm, swParams, self->framesPerBuffer ), paUnanticipatedHostError );
    ENSURE_( snd_pcm_sw_params_set_stop_threshold( self->pcm, swParams, self->bufferSize ), paUnanticipatedHostError );

    /* Silence buffer in the case of underrun */
    if( !primeBuffers ) /* XXX: Make sense? */
    {
        snd_pcm_uframes_t boundary;
        ENSURE_( snd_pcm_sw_params_get_boundary( swParams, &boundary ), paUnanticipatedHostError );
        ENSURE_( snd_pcm_sw_params_set_silence_threshold( self->pcm, swParams, 0 ), paUnanticipatedHostError );
        ENSURE_( snd_pcm_sw_params_set_silence_size( self->pcm, swParams, boundary ), paUnanticipatedHostError );
    }
        
    ENSURE_( snd_pcm_sw_params_set_avail_min( self->pcm, swParams, self->framesPerBuffer ), paUnanticipatedHostError );
    ENSURE_( snd_pcm_sw_params_set_xfer_align( self->pcm, swParams, 1 ), paUnanticipatedHostError );
    ENSURE_( snd_pcm_sw_params_set_tstamp_mode( self->pcm, swParams, SND_PCM_TSTAMP_MMAP ), paUnanticipatedHostError );

    /* Set the parameters! */
    ENSURE_( snd_pcm_sw_params( self->pcm, swParams ), paUnanticipatedHostError );

error:
    return result;
}

static PaError PaAlsaStream_Initialize( PaAlsaStream *self, PaAlsaHostApiRepresentation *alsaApi, const PaStreamParameters *inParams,
        const PaStreamParameters *outParams, double sampleRate, unsigned long framesPerUserBuffer, PaStreamCallback callback,
        PaStreamFlags streamFlags, void *userData )
{
    PaError result = paNoError;
    assert( self );

    memset( self, 0, sizeof (PaAlsaStream) );

    if( NULL != callback )
    {
        PaUtil_InitializeStreamRepresentation( &self->streamRepresentation,
                                               &alsaApi->callbackStreamInterface,
                                               callback, userData );
        self->callbackMode = 1;
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &self->streamRepresentation,
                                               &alsaApi->blockingStreamInterface,
                                               NULL, userData );
    }

    self->framesPerUserBuffer = framesPerUserBuffer;
    self->neverDropInput = streamFlags & paNeverDropInput;
    /* XXX: Ignore paPrimeOutputBuffersUsingStreamCallback untill buffer priming is fully supported in pa_process.c */
    /*
    if( outParams & streamFlags & paPrimeOutputBuffersUsingStreamCallback )
        self->primeBuffers = 1;
        */
    memset( &self->capture, 0, sizeof (PaAlsaStreamComponent) );
    memset( &self->playback, 0, sizeof (PaAlsaStreamComponent) );
    if( inParams )
        PA_ENSURE( PaAlsaStreamComponent_Initialize( &self->capture, alsaApi, inParams, StreamDirection_In, NULL != callback ) );
    if( outParams )
        PA_ENSURE( PaAlsaStreamComponent_Initialize( &self->playback, alsaApi, outParams, StreamDirection_Out, NULL != callback ) );

    assert( self->capture.nfds || self->playback.nfds );

    PA_UNLESS( self->pfds = (struct pollfd*)PaUtil_AllocateMemory( (self->capture.nfds +
                    self->playback.nfds) * sizeof (struct pollfd) ), paInsufficientMemory );

    PaUtil_InitializeCpuLoadMeasurer( &self->cpuLoadMeasurer, sampleRate );
    InitializeThreading( &self->threading, &self->cpuLoadMeasurer );
    ASSERT_CALL_( pthread_mutex_init( &self->stateMtx, NULL ), 0 );
    ASSERT_CALL_( pthread_mutex_init( &self->startMtx, NULL ), 0 );
    ASSERT_CALL_( pthread_cond_init( &self->startCond, NULL ), 0 );

error:
    return result;
}

/** Free resources associated with stream, and eventually stream itself.
 *
 * Frees allocated memory, and terminates individual StreamComponents.
 */
static void PaAlsaStream_Terminate( PaAlsaStream *self )
{
    assert( self );

    if( self->capture.pcm )
    {
        PaAlsaStreamComponent_Terminate( &self->capture );
    }
    if( self->playback.pcm )
    {
        PaAlsaStreamComponent_Terminate( &self->playback );
    }

    PaUtil_FreeMemory( self->pfds );
    ASSERT_CALL_( pthread_mutex_destroy( &self->stateMtx ), 0 );
    ASSERT_CALL_( pthread_mutex_destroy( &self->startMtx ), 0 );
    ASSERT_CALL_( pthread_cond_destroy( &self->startCond ), 0 );

    PaUtil_FreeMemory( self );
}

/** Calculate polling timeout
 *
 * @param frames Time to wait
 * @return Polling timeout in milliseconds
 */
static int CalculatePollTimeout( const PaAlsaStream *stream, unsigned long frames )
{
    assert( stream->streamRepresentation.streamInfo.sampleRate > 0.0 );
    /* Period in msecs, rounded up */
    return (int)ceil( 1000 * frames / stream->streamRepresentation.streamInfo.sampleRate );
}

/** Determine size per host buffer.
 *
 * During this method call, the component's framesPerBuffer attribute gets computed, and the corresponding period size
 * gets configured for the device.
 * @param accurate: If the configured period size is non-integer, this will be set to 0.
 */
static PaError PaAlsaStreamComponent_DetermineFramesPerBuffer( PaAlsaStreamComponent* self, const PaStreamParameters* params,
        unsigned long framesPerUserBuffer, double sampleRate, snd_pcm_hw_params_t* hwParams, int* accurate )
{
    PaError result = paNoError;
    unsigned long bufferSize = params->suggestedLatency * sampleRate, framesPerHostBuffer;
    int dir = 0;
    
    {
        snd_pcm_uframes_t tmp;
        snd_pcm_hw_params_get_buffer_size_min( hwParams, &tmp );
        bufferSize = PA_MAX( bufferSize, tmp );
        snd_pcm_hw_params_get_buffer_size_max( hwParams, &tmp );
        bufferSize = PA_MIN( bufferSize, tmp );
    }

    assert( bufferSize > 0 );

    if( framesPerUserBuffer != paFramesPerBufferUnspecified )
    {
        /* Preferably the host buffer size should be a multiple of the user buffer size */

        if( bufferSize > framesPerUserBuffer )
        {
            snd_pcm_uframes_t remainder = bufferSize % framesPerUserBuffer;
            if( remainder > framesPerUserBuffer / 2. )
                bufferSize += framesPerUserBuffer - remainder;
            else
                bufferSize -= remainder;

            assert( bufferSize % framesPerUserBuffer == 0 );
        }
        else if( framesPerUserBuffer % bufferSize != 0 )
        {
            /*  Find a good compromise between user specified latency and buffer size */
            if( bufferSize > framesPerUserBuffer * .75 )
            {
                bufferSize = framesPerUserBuffer;
            }
            else
            {
                snd_pcm_uframes_t newSz = framesPerUserBuffer;
                while( newSz / 2 >= bufferSize )
                {
                    if( framesPerUserBuffer % (newSz / 2) != 0 )
                    {
                        /* No use dividing any further */
                        break;
                    }
                    newSz /= 2;
                }
                bufferSize = newSz;
            }

            assert( framesPerUserBuffer % bufferSize == 0 );
        }
    }

    /* Using 5 as a base number of periods, we try to approximate the suggested latency (+1 period),
       finding a combination of period/buffer size which best fits these constraints */
    {
        unsigned numPeriods = 4, maxPeriods = 0;
        /* It may be that the device only supports 2 periods for instance */
        dir = 0;
        ENSURE_( snd_pcm_hw_params_get_periods_max( hwParams, &maxPeriods, &dir ), paUnanticipatedHostError );
        assert( maxPeriods > 1 );
        /* One period is not counted as latency */
        maxPeriods -= 1;
        numPeriods = PA_MIN( maxPeriods, numPeriods );

        if( framesPerUserBuffer != paFramesPerBufferUnspecified )
        {
            framesPerHostBuffer = framesPerUserBuffer;
            if( framesPerHostBuffer < bufferSize )
            {
                while( bufferSize / framesPerHostBuffer > numPeriods )
                {
                    framesPerHostBuffer *= 2;
                }
            }
            else
            {
                while( bufferSize / framesPerHostBuffer < numPeriods )
                {
                    if( framesPerUserBuffer % (framesPerHostBuffer / 2) != 0 )
                    {
                        /* Can't be divided any further */
                        break;
                    }
                    framesPerHostBuffer /= 2;
                }
            }

            if( framesPerHostBuffer < framesPerUserBuffer )
            {
                assert( framesPerUserBuffer % framesPerHostBuffer == 0 );
                if( snd_pcm_hw_params_test_period_size( self->pcm, hwParams, framesPerHostBuffer, 0 ) < 0 )
                {
                    if( snd_pcm_hw_params_test_period_size( self->pcm, hwParams, framesPerHostBuffer * 2, 0 ) == 0 )
                        framesPerHostBuffer *= 2;
                    else if( snd_pcm_hw_params_test_period_size( self->pcm, hwParams, framesPerHostBuffer / 2, 0 ) == 0 )
                        framesPerHostBuffer /= 2;
                }
            }
            else
            {
                assert( framesPerHostBuffer % framesPerUserBuffer == 0 );
                if( snd_pcm_hw_params_test_period_size( self->pcm, hwParams, framesPerHostBuffer, 0 ) < 0 )
                {
                    if( snd_pcm_hw_params_test_period_size( self->pcm, hwParams, framesPerHostBuffer + framesPerUserBuffer, 0 ) == 0 )
                        framesPerHostBuffer += framesPerUserBuffer;
                    else if( snd_pcm_hw_params_test_period_size( self->pcm, hwParams, framesPerHostBuffer - framesPerUserBuffer, 0 ) == 0 )
                        framesPerHostBuffer -= framesPerUserBuffer;
                }
            }
        }
        else
        {
            framesPerHostBuffer = bufferSize / numPeriods;
        }
    }

    assert( framesPerHostBuffer > 0 );
    {
        snd_pcm_uframes_t min = 0, max = 0;
        ENSURE_( snd_pcm_hw_params_get_period_size_min( hwParams, &min, NULL ), paUnanticipatedHostError );
        ENSURE_( snd_pcm_hw_params_get_period_size_max( hwParams, &max, NULL ), paUnanticipatedHostError );

        if( framesPerHostBuffer < min )
        {
            framesPerHostBuffer = min;
            PA_DEBUG(( "%s: The determined period size (%lu) is less than minimum (%lu)\n", __FUNCTION__,
                        framesPerHostBuffer, min ));
        }
        else if( framesPerHostBuffer > max )
        {
            framesPerHostBuffer = max;
            PA_DEBUG(( "%s: The determined period size (%lu) is greater than maximum (%lu)\n", __FUNCTION__,
                        framesPerHostBuffer, max ));
        }

        assert( framesPerHostBuffer >= min && framesPerHostBuffer <= max );
        dir = 0;
        ENSURE_( snd_pcm_hw_params_set_period_size_near( self->pcm, hwParams, &framesPerHostBuffer, &dir ),
                paUnanticipatedHostError );
        if( dir != 0 )
        {
            PA_DEBUG(( "%s: The configured period size is non-integer.\n", __FUNCTION__, dir ));
            *accurate = 0;
        }
    }
    self->framesPerBuffer = framesPerHostBuffer;

error:
    return result;
}

/* We need to determine how many frames per host buffer (period) to use.  Our
 * goals are to provide the best possible performance, but also to
 * honor the requested latency settings as closely as we can. Therefore this
 * decision is based on:
 *
 *   - the period sizes that playback and/or capture support.  The
 *     host buffer size has to be one of these.
 *   - the number of periods that playback and/or capture support.
 *
 * We want to make period_size*(num_periods-1) to be as close as possible
 * to latency*rate for both playback and capture.
 *
 * This method will determine suitable period sizes for capture and playback handles, and report the maximum number of
 * frames per host buffer. The latter is relevant, in case we should be so unfortunate that the period size differs
 * between capture and playback. If this should happen, the stream's hostBufferSizeMode attribute will be set to
 * paUtilBoundedHostBufferSize, because the best we can do is limit the size of individual host buffers to the upper
 * bound. The size of host buffers scheduled for processing should only matter if the user has specified a buffer size,
 * but when he/she does we must strive for an optimal configuration. By default we'll opt for a fixed host buffer size,
 * which should be fine if the period size is the same for capture and playback. In general, if there is a specified user
 * buffer size, this method tries it best to determine a period size which is a multiple of the user buffer size.
 *
 * The framesPerBuffer attributes of the individual capture and playback components of the stream are set to corresponding
 * values determined here. Since these should be reported as 
 *
 * This is one of those blocks of code that will just take a lot of
 * refinement to be any good.
 *
 * In the full-duplex case it is possible that the routine was unable
 * to find a number of frames per buffer acceptable to both devices
 * TODO: Implement an algorithm to find the value closest to acceptance
 * by both devices, to minimize difference between period sizes?
 *
 * @param determinedFramesPerHostBuffer: The determined host buffer size.
 */
static PaError PaAlsaStream_DetermineFramesPerBuffer( PaAlsaStream* self, double sampleRate, const PaStreamParameters* inputParameters,
        const PaStreamParameters* outputParameters, unsigned long framesPerUserBuffer, snd_pcm_hw_params_t* hwParamsCapture,
        snd_pcm_hw_params_t* hwParamsPlayback, PaUtilHostBufferSizeMode* hostBufferSizeMode )
{
    PaError result = paNoError;
    unsigned long framesPerHostBuffer = 0;
    int dir = 0;
    int accurate = 1;

    if( self->capture.pcm && self->playback.pcm )
    {
        if( framesPerUserBuffer == paFramesPerBufferUnspecified )
        {
            snd_pcm_uframes_t desiredLatency, e, minPeriodSize, maxPeriodSize, optimalPeriodSize, periodSize,
                              minCapture, minPlayback, maxCapture, maxPlayback;

            /* Come up with a common desired latency */

            dir = 0;
            ENSURE_( snd_pcm_hw_params_get_period_size_min( hwParamsCapture, &minCapture, &dir ), paUnanticipatedHostError );
            dir = 0;
            ENSURE_( snd_pcm_hw_params_get_period_size_min( hwParamsPlayback, &minPlayback, &dir ), paUnanticipatedHostError );
            dir = 0;
            ENSURE_( snd_pcm_hw_params_get_period_size_max( hwParamsCapture, &maxCapture, &dir ), paUnanticipatedHostError );
            dir = 0;
            ENSURE_( snd_pcm_hw_params_get_period_size_max( hwParamsPlayback, &maxPlayback, &dir ), paUnanticipatedHostError );
            minPeriodSize = PA_MAX( minPlayback, minCapture );
            maxPeriodSize = PA_MIN( maxPlayback, maxCapture );
            PA_UNLESS( minPeriodSize <= maxPeriodSize, paBadIODeviceCombination );

            desiredLatency = (snd_pcm_uframes_t)(PA_MIN( outputParameters->suggestedLatency, inputParameters->suggestedLatency )
                    * sampleRate);
            /* Clamp desiredLatency */
            {
                snd_pcm_uframes_t maxBufferSize;
                snd_pcm_uframes_t maxBufferSizeCapture, maxBufferSizePlayback;
                ENSURE_( snd_pcm_hw_params_get_buffer_size_max( hwParamsCapture, &maxBufferSizeCapture ), paUnanticipatedHostError );
                ENSURE_( snd_pcm_hw_params_get_buffer_size_max( hwParamsPlayback, &maxBufferSizePlayback ), paUnanticipatedHostError );
                maxBufferSize = PA_MIN( maxBufferSizeCapture, maxBufferSizePlayback );

                desiredLatency = PA_MIN( desiredLatency, maxBufferSize );
            }

            /* Find the closest power of 2 */
            e = ilogb( minPeriodSize );
            if( minPeriodSize & (minPeriodSize - 1) )
                e += 1;
            periodSize = (snd_pcm_uframes_t)pow( 2, e );

            while( periodSize <= maxPeriodSize )
            {
                if( snd_pcm_hw_params_test_period_size( self->playback.pcm, hwParamsPlayback, periodSize, 0 ) >= 0 &&
                        snd_pcm_hw_params_test_period_size( self->capture.pcm, hwParamsCapture, periodSize, 0 ) >= 0 )
                    break;  /* Ok! */

                periodSize *= 2;
            }

            /* 4 periods considered optimal */
            optimalPeriodSize = PA_MAX( desiredLatency / 4, minPeriodSize );
            optimalPeriodSize = PA_MIN( optimalPeriodSize, maxPeriodSize );

            /* Find the closest power of 2 */
            e = ilogb( optimalPeriodSize );
            if( optimalPeriodSize & (optimalPeriodSize - 1) )
                e += 1;
            optimalPeriodSize = (snd_pcm_uframes_t)pow( 2, e );

            while( optimalPeriodSize >= periodSize )
            {
                if( snd_pcm_hw_params_test_period_size( self->capture.pcm, hwParamsCapture, optimalPeriodSize, 0 ) < 0 )
                    continue;
                if( snd_pcm_hw_params_test_period_size( self->playback.pcm, hwParamsPlayback, optimalPeriodSize, 0 ) >= 0 )
                    break;
                optimalPeriodSize /= 2;
            }
            if( optimalPeriodSize > periodSize )
                periodSize = optimalPeriodSize;

            if( periodSize <= maxPeriodSize )
            {
                /* Looks good, the periodSize _should_ be acceptable by both devices */
                ENSURE_( snd_pcm_hw_params_set_period_size( self->capture.pcm, hwParamsCapture, periodSize, 0 ),
                        paUnanticipatedHostError );
                ENSURE_( snd_pcm_hw_params_set_period_size( self->playback.pcm, hwParamsPlayback, periodSize, 0 ),
                        paUnanticipatedHostError );
                self->capture.framesPerBuffer = self->playback.framesPerBuffer = periodSize;
                framesPerHostBuffer = periodSize;
            }
            else
            {
                /* Unable to find a common period size, oh well */
                optimalPeriodSize = PA_MAX( desiredLatency / 4, minPeriodSize );
                optimalPeriodSize = PA_MIN( optimalPeriodSize, maxPeriodSize );

                self->capture.framesPerBuffer = optimalPeriodSize;
                dir = 0;
                ENSURE_( snd_pcm_hw_params_set_period_size_near( self->capture.pcm, hwParamsCapture, &self->capture.framesPerBuffer, &dir ),
                        paUnanticipatedHostError );
                self->playback.framesPerBuffer = optimalPeriodSize;
                dir = 0;
                ENSURE_( snd_pcm_hw_params_set_period_size_near( self->playback.pcm, hwParamsPlayback, &self->playback.framesPerBuffer, &dir ),
                        paUnanticipatedHostError );
                framesPerHostBuffer = PA_MAX( self->capture.framesPerBuffer, self->playback.framesPerBuffer );
                *hostBufferSizeMode = paUtilBoundedHostBufferSize;
            }
        }
        else
        {
            /* We choose the simple route and determine a suitable number of frames per buffer for one component of
             * the stream, then we hope that this will work for the other component too (it should!).
             */

            unsigned maxPeriods = 0;
            PaAlsaStreamComponent* first = &self->capture, * second = &self->playback;
            const PaStreamParameters* firstStreamParams = inputParameters;
            snd_pcm_hw_params_t* firstHwParams = hwParamsCapture, * secondHwParams = hwParamsPlayback;

            dir = 0;
            ENSURE_( snd_pcm_hw_params_get_periods_max( hwParamsPlayback, &maxPeriods, &dir ), paUnanticipatedHostError );
            if( maxPeriods < 4 )
            {
                /* The playback component is trickier to get right, try that first */
                first = &self->playback;
                second = &self->capture;
                firstStreamParams = outputParameters;
                firstHwParams = hwParamsPlayback;
                secondHwParams = hwParamsCapture;
            }

            PA_ENSURE( PaAlsaStreamComponent_DetermineFramesPerBuffer( first, firstStreamParams, framesPerUserBuffer,
                        sampleRate, firstHwParams, &accurate ) );

            second->framesPerBuffer = first->framesPerBuffer;
            dir = 0;
            ENSURE_( snd_pcm_hw_params_set_period_size_near( second->pcm, secondHwParams, &second->framesPerBuffer, &dir ),
                    paUnanticipatedHostError );
            if( self->capture.framesPerBuffer == self->playback.framesPerBuffer )
            {
                framesPerHostBuffer = self->capture.framesPerBuffer;
            }
            else
            {
                framesPerHostBuffer = PA_MAX( self->capture.framesPerBuffer, self->playback.framesPerBuffer );
                *hostBufferSizeMode = paUtilBoundedHostBufferSize;
            }
        }
    }
    else    /* half-duplex is a slightly simpler case */
    {
        if( self->capture.pcm )
        {
            PA_ENSURE( PaAlsaStreamComponent_DetermineFramesPerBuffer( &self->capture, inputParameters, framesPerUserBuffer,
                        sampleRate, hwParamsCapture, &accurate) );
            framesPerHostBuffer = self->capture.framesPerBuffer;
        }
        else
        {
            assert( self->playback.pcm );
            PA_ENSURE( PaAlsaStreamComponent_DetermineFramesPerBuffer( &self->playback, outputParameters, framesPerUserBuffer,
                        sampleRate, hwParamsPlayback, &accurate ) );
            framesPerHostBuffer = self->playback.framesPerBuffer;
        }
    }

    PA_UNLESS( framesPerHostBuffer != 0, paInternalError );
    self->maxFramesPerHostBuffer = framesPerHostBuffer;

    if( !accurate )
    {
        /* Don't know the exact size per host buffer */
        *hostBufferSizeMode = paUtilBoundedHostBufferSize;
        /* Raise upper bound */
        ++self->maxFramesPerHostBuffer;
    }

error:
    return result;
}

/** Set up ALSA stream parameters.
 *
 */
static PaError PaAlsaStream_Configure( PaAlsaStream *self, const PaStreamParameters *inParams, const PaStreamParameters*
        outParams, double sampleRate, unsigned long framesPerUserBuffer, double* inputLatency, double* outputLatency,
        PaUtilHostBufferSizeMode* hostBufferSizeMode )
{
    PaError result = paNoError;
    double realSr = sampleRate;
    snd_pcm_hw_params_t* hwParamsCapture, * hwParamsPlayback;

    snd_pcm_hw_params_alloca( &hwParamsCapture );
    snd_pcm_hw_params_alloca( &hwParamsPlayback );

    if( self->capture.pcm )
        PA_ENSURE( PaAlsaStreamComponent_InitialConfigure( &self->capture, inParams, self->primeBuffers, hwParamsCapture,
                    &realSr ) );
    if( self->playback.pcm )
        PA_ENSURE( PaAlsaStreamComponent_InitialConfigure( &self->playback, outParams, self->primeBuffers, hwParamsPlayback,
                    &realSr ) );

    PA_ENSURE( PaAlsaStream_DetermineFramesPerBuffer( self, realSr, inParams, outParams, framesPerUserBuffer,
                hwParamsCapture, hwParamsPlayback, hostBufferSizeMode ) );

    if( self->capture.pcm )
    {
        assert( self->capture.framesPerBuffer != 0 );
        PA_ENSURE( PaAlsaStreamComponent_FinishConfigure( &self->capture, hwParamsCapture, inParams, self->primeBuffers, realSr,
                    inputLatency ) );
        PA_DEBUG(( "%s: Capture period size: %lu, latency: %f\n", __FUNCTION__, self->capture.framesPerBuffer, *inputLatency ));
    }
    if( self->playback.pcm )
    {
        assert( self->playback.framesPerBuffer != 0 );
        PA_ENSURE( PaAlsaStreamComponent_FinishConfigure( &self->playback, hwParamsPlayback, outParams, self->primeBuffers, realSr,
                    outputLatency ) );
        PA_DEBUG(( "%s: Playback period size: %lu, latency: %f\n", __FUNCTION__, self->playback.framesPerBuffer, *outputLatency ));
    }

    /* Should be exact now */
    self->streamRepresentation.streamInfo.sampleRate = realSr;

    /* this will cause the two streams to automatically start/stop/prepare in sync.
     * We only need to execute these operations on one of the pair.
     * A: We don't want to do this on a blocking stream.
     */
    if( self->callbackMode && self->capture.pcm && self->playback.pcm )
    {
        int err = snd_pcm_link( self->capture.pcm, self->playback.pcm );
        if( err == 0 )
            self->pcmsSynced = 1;
        else
            PA_DEBUG(( "%s: Unable to sync pcms: %s\n", __FUNCTION__, snd_strerror( err ) ));
    }

    {
        unsigned long minFramesPerHostBuffer = PA_MIN( self->capture.pcm ? self->capture.framesPerBuffer : ULONG_MAX,
            self->playback.pcm ? self->playback.framesPerBuffer : ULONG_MAX );
        self->pollTimeout = CalculatePollTimeout( self, minFramesPerHostBuffer );    /* Period in msecs, rounded up */

        /* Time before watchdog unthrottles realtime thread == 1/4 of period time in msecs */
        self->threading.throttledSleepTime = (unsigned long) (minFramesPerHostBuffer / sampleRate / 4 * 1000);
    }

    if( self->callbackMode )
    {
        /* If the user expects a certain number of frames per callback we will either have to rely on block adaption
         * (framesPerHostBuffer is not an integer multiple of framesPerBuffer) or we can simply align the number
         * of host buffer frames with what the user specified */
        if( self->framesPerUserBuffer != paFramesPerBufferUnspecified )
        {
            /* self->alignFrames = 1; */

            /* Unless the ratio between number of host and user buffer frames is an integer we will have to rely
             * on block adaption */
        /*
            if( framesPerHostBuffer % framesPerBuffer != 0 || (self->capture.pcm && self->playback.pcm &&
                        self->capture.framesPerBuffer != self->playback.framesPerBuffer) )
                self->useBlockAdaption = 1;
            else
                self->alignFrames = 1;
        */
        }
    }

error:
    return result;
}

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback* callback,
                           void *userData )
{
    PaError result = paNoError;
    PaAlsaHostApiRepresentation *alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;
    PaAlsaStream *stream = NULL;
    PaSampleFormat hostInputSampleFormat = 0, hostOutputSampleFormat = 0;
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;
    int numInputChannels = 0, numOutputChannels = 0;
    PaTime inputLatency, outputLatency;
    /* Operate with fixed host buffer size by default, since other modes will invariably lead to block adaption */
    /* XXX: Use Bounded by default? Output tends to get stuttery with Fixed ... */
    PaUtilHostBufferSizeMode hostBufferSizeMode = paUtilFixedHostBufferSize;

    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag;

    if( inputParameters )
    {
        PA_ENSURE( ValidateParameters( inputParameters, hostApi, StreamDirection_In ) );

        numInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }
    if( outputParameters )
    {
        PA_ENSURE( ValidateParameters( outputParameters, hostApi, StreamDirection_Out ) );

        numOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
    }

    /* XXX: Why do we support this anyway? */
    if( framesPerBuffer == paFramesPerBufferUnspecified && getenv( "PA_ALSA_PERIODSIZE" ) != NULL )
    {
        PA_DEBUG(( "%s: Getting framesPerBuffer from environment\n", __FUNCTION__ ));
        framesPerBuffer = atoi( getenv("PA_ALSA_PERIODSIZE") );
    }

    PA_UNLESS( stream = (PaAlsaStream*)PaUtil_AllocateMemory( sizeof(PaAlsaStream) ), paInsufficientMemory );
    PA_ENSURE( PaAlsaStream_Initialize( stream, alsaHostApi, inputParameters, outputParameters, sampleRate,
                framesPerBuffer, callback, streamFlags, userData ) );

    PA_ENSURE( PaAlsaStream_Configure( stream, inputParameters, outputParameters, sampleRate, framesPerBuffer,
                &inputLatency, &outputLatency, &hostBufferSizeMode ) );
    hostInputSampleFormat = stream->capture.hostSampleFormat;
    hostOutputSampleFormat = stream->playback.hostSampleFormat;

    PA_ENSURE( PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
                    numInputChannels, inputSampleFormat, hostInputSampleFormat,
                    numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
                    sampleRate, streamFlags, framesPerBuffer, stream->maxFramesPerHostBuffer,
                    hostBufferSizeMode, callback, userData ) );

    /* Ok, buffer processor is initialized, now we can deduce it's latency */
    if( numInputChannels > 0 )
        stream->streamRepresentation.streamInfo.inputLatency = inputLatency + PaUtil_GetBufferProcessorInputLatency(
                &stream->bufferProcessor );
    if( numOutputChannels > 0 )
        stream->streamRepresentation.streamInfo.outputLatency = outputLatency + PaUtil_GetBufferProcessorOutputLatency(
                &stream->bufferProcessor );

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
    {
        PA_DEBUG(( "%s: Stream in error, terminating\n", __FUNCTION__ ));
        PaAlsaStream_Terminate( stream );
    }

    return result;
}

static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    PaAlsaStream_Terminate( stream );

    return result;
}

static void SilenceBuffer( PaAlsaStream *stream )
{
    const snd_pcm_channel_area_t *areas;
    snd_pcm_uframes_t frames = (snd_pcm_uframes_t)snd_pcm_avail_update( stream->playback.pcm ), offset;

    snd_pcm_mmap_begin( stream->playback.pcm, &areas, &offset, &frames );
    snd_pcm_areas_silence( areas, offset, stream->playback.numHostChannels, frames, stream->playback.nativeFormat );
    snd_pcm_mmap_commit( stream->playback.pcm, offset, frames );
}

/** Start/prepare pcm(s) for streaming.
 *
 * Depending on wether the stream is in callback or blocking mode, we will respectively start or simply
 * prepare the playback pcm. If the buffer has _not_ been primed, we will in callback mode prepare and
 * silence the buffer before starting playback. In blocking mode we simply prepare, as the playback will
 * be started automatically as the user writes to output. 
 *
 * The capture pcm, however, will simply be prepared and started.
 *
 * PaAlsaStream::startMtx makes sure access is synchronized (useful in callback mode)
 */
static PaError AlsaStart( PaAlsaStream *stream, int priming )
{
    PaError result = paNoError;

    if( stream->playback.pcm )
    {
        if( stream->callbackMode )
        {
            if( !priming )
            {
                /* Buffer isn't primed, so prepare and silence */
                ENSURE_( snd_pcm_prepare( stream->playback.pcm ), paUnanticipatedHostError );
                SilenceBuffer( stream );
            }
            ENSURE_( snd_pcm_start( stream->playback.pcm ), paUnanticipatedHostError );
        }
        else
            ENSURE_( snd_pcm_prepare( stream->playback.pcm ), paUnanticipatedHostError );
    }
    if( stream->capture.pcm && !stream->pcmsSynced )
    {
        ENSURE_( snd_pcm_prepare( stream->capture.pcm ), paUnanticipatedHostError );
        /* For a blocking stream we want to start capture as well, since nothing will happen otherwise */
        ENSURE_( snd_pcm_start( stream->capture.pcm ), paUnanticipatedHostError );
    }

end:
    return result;
error:
    goto end;
}

/** Utility function for determining if pcms are in running state.
 *
 */
static int IsRunning( PaAlsaStream *stream )
{
    int result = 0;

    LockMutex( &stream->stateMtx );
    if( stream->capture.pcm )
    {
        snd_pcm_state_t capture_state = snd_pcm_state( stream->capture.pcm );

        if( capture_state == SND_PCM_STATE_RUNNING || capture_state == SND_PCM_STATE_XRUN
                || capture_state == SND_PCM_STATE_DRAINING )
        {
            result = 1;
            goto end;
        }
    }

    if( stream->playback.pcm )
    {
        snd_pcm_state_t playback_state = snd_pcm_state( stream->playback.pcm );

        if( playback_state == SND_PCM_STATE_RUNNING || playback_state == SND_PCM_STATE_XRUN
                || playback_state == SND_PCM_STATE_DRAINING )
        {
            result = 1;
            goto end;
        }
    }

end:
    ASSERT_CALL_( UnlockMutex( &stream->stateMtx ), paNoError );

    return result;
}

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    int streamStarted = 0;  /* So we can know wether we need to take the stream down */

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    /* Set now, so we can test for activity further down */
    stream->isActive = 1;

    if( stream->callbackMode )
    {
        int res = 0;
        PaTime pt = PaUtil_GetTime();
        struct timespec ts;

        PA_ENSURE( CreateCallbackThread( &stream->threading, &CallbackThreadFunc, stream ) );
        streamStarted = 1;

        /* Wait for stream to be started */
        ts.tv_sec = (time_t) floor( pt + 1 );
        ts.tv_nsec = (long) ((pt - floor( pt )) * 1000000000);

        /* Since we'll be holding a lock on the startMtx (when not waiting on the condition), IsRunning won't be checking
         * stream state at the same time as the callback thread affects it. We also check IsStreamActive, in the unlikely
         * case the callback thread exits in the meantime (the stream will be considered inactive after the thread exits) */
        PA_ENSURE( LockMutex( &stream->startMtx ) );

        /* Due to possible spurious wakeups, we enclose in a loop */
        while( !IsRunning( stream ) && IsStreamActive( s ) && !res )
        {
            res = pthread_cond_timedwait( &stream->startCond, &stream->startMtx, &ts );
        }
        PA_ENSURE( UnlockMutex( &stream->startMtx ) );

        PA_UNLESS( !res || res == ETIMEDOUT, paInternalError );
        PA_DEBUG(( "%s: Waited for %g seconds for stream to start\n", __FUNCTION__, PaUtil_GetTime() - pt ));

        if( res == ETIMEDOUT )
        {
            PA_ENSURE( paTimedOut );
        }
    }
    else
    {
        PA_ENSURE( AlsaStart( stream, 0 ) );
        streamStarted = 1;
    }

end:
    return result;
error:
    if( streamStarted )
        AbortStream( stream );
    stream->isActive = 0;
    
    goto end;
}

static PaError AlsaStop( PaAlsaStream *stream, int abort )
{
    PaError result = paNoError;

    if( abort )
    {
        if( stream->playback.pcm )
        {
            ENSURE_( snd_pcm_drop( stream->playback.pcm ), paUnanticipatedHostError );
        }
        if( stream->capture.pcm && !stream->pcmsSynced )
        {
            ENSURE_( snd_pcm_drop( stream->capture.pcm ), paUnanticipatedHostError );
        }

        PA_DEBUG(( "%s: Dropped frames\n", __FUNCTION__ ));
    }
    else
    {
        if( stream->playback.pcm )
        {
            ENSURE_( snd_pcm_nonblock( stream->playback.pcm, 0 ), paUnanticipatedHostError );
            if( snd_pcm_drain( stream->playback.pcm ) < 0 )
            {
                PA_DEBUG(( "%s: Draining playback handle failed!\n", __FUNCTION__ ));
            }
        }
        if( stream->capture.pcm && !stream->pcmsSynced )
        {
            /* We don't need to retrieve any remaining frames */
            if( snd_pcm_drop( stream->capture.pcm ) < 0 )
            {
                PA_DEBUG(( "%s: Draining capture handle failed!\n", __FUNCTION__ ));
            }
        }
    }

end:
    return result;
error:
    goto end;
}

/** Stop or abort stream.
 *
 * If a stream is in callback mode we will have to inspect wether the background thread has
 * finished, or we will have to take it out. In either case we join the thread before
 * returning. In blocking mode, we simply tell ALSA to stop abruptly (abort) or finish
 * buffers (drain)
 *
 * Stream will be considered inactive (!PaAlsaStream::isActive) after a call to this function
 */
static PaError RealStop( PaAlsaStream *stream, int abort )
{
    PaError result = paNoError;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */
    if( stream->callbackMode )
    {
        PaError threadRes, watchdogRes;
        stream->callbackAbort = abort;

        if( !abort )
        {
            PA_DEBUG(( "Stopping callback\n" ));
            stream->callbackStop = 1;
        }
        PA_ENSURE( KillCallbackThread( &stream->threading, !abort, &threadRes, &watchdogRes ) );
        if( threadRes != paNoError )
            PA_DEBUG(( "Callback thread returned: %d\n", threadRes ));
        if( watchdogRes != paNoError )
            PA_DEBUG(( "Watchdog thread returned: %d\n", watchdogRes ));

        stream->callbackStop = 0;   /* The deed is done */
        stream->callback_finished = 0;
    }
    else
    {
        PA_ENSURE( AlsaStop( stream, abort ) );
    }

    stream->isActive = 0;

end:
    return result;

error:
    goto end;
}

static PaError StopStream( PaStream *s )
{
    return RealStop( (PaAlsaStream *) s, 0 );
}

static PaError AbortStream( PaStream *s )
{
    return RealStop( (PaAlsaStream * ) s, 1 );
}

/** The stream is considered stopped before StartStream, or AFTER a call to Abort/StopStream (callback
 * returning !paContinue is not considered)
 *
 */
static PaError IsStreamStopped( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream *)s;

    /* callback_finished indicates we need to join callback thread (ie. in Abort/StopStream) */
    return !IsStreamActive( s ) && !stream->callback_finished;
}

static PaError IsStreamActive( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;
    return stream->isActive;
}

static PaTime GetStreamTime( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    snd_timestamp_t timestamp;
    snd_pcm_status_t* status;
    snd_pcm_status_alloca( &status );

    /* TODO: what if we have both?  does it really matter? */

    /* TODO: if running in callback mode, this will mean
     * libasound routines are being called from multiple threads.
     * need to verify that libasound is thread-safe. */

    if( stream->capture.pcm )
    {
        snd_pcm_status( stream->capture.pcm, status );
    }
    else if( stream->playback.pcm )
    {
        snd_pcm_status( stream->playback.pcm, status );
    }

    snd_pcm_status_get_tstamp( status, &timestamp );
    return timestamp.tv_sec + (PaTime)timestamp.tv_usec / 1e6;
}

static double GetStreamCpuLoad( PaStream* s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}

static int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate )
{
    unsigned long approx = (unsigned long) sampleRate;
    int dir = 0;
    double fraction = sampleRate - approx;

    assert( pcm && hwParams );

    if( fraction > 0.0 )
    {
        if( fraction > 0.5 )
        {
            ++approx;
            dir = -1;
        }
        else
            dir = 1;
    }

    return snd_pcm_hw_params_set_rate( pcm, hwParams, approx, dir );
}

/* Return exact sample rate in param sampleRate */
static int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate )
{
    unsigned int num, den;
    int err; 

    assert( hwParams );

    err = snd_pcm_hw_params_get_rate_numden( hwParams, &num, &den );
    *sampleRate = (double) num / den;

    return err;
}

/* Utility functions for blocking/callback interfaces */

/* Atomic restart of stream (we don't want the intermediate state visible) */
static PaError AlsaRestart( PaAlsaStream *stream )
{
    PaError result = paNoError;

    PA_ENSURE( LockMutex( &stream->stateMtx ) );
    PA_ENSURE( AlsaStop( stream, 0 ) );
    PA_ENSURE( AlsaStart( stream, 0 ) );

    PA_DEBUG(( "%s: Restarted audio\n", __FUNCTION__ ));

error:
    PA_ENSURE( UnlockMutex( &stream->stateMtx ) );

    return result;
}

/** Recover from xrun state.
 *
 */
static PaError PaAlsaStream_HandleXrun( PaAlsaStream *self )
{
    PaError result = paNoError;
    snd_pcm_status_t *st;
    PaTime now = PaUtil_GetTime();
    snd_timestamp_t t;

    snd_pcm_status_alloca( &st );

    if( self->playback.pcm )
    {
        snd_pcm_status( self->playback.pcm, st );
        if( snd_pcm_status_get_state( st ) == SND_PCM_STATE_XRUN )
        {
            snd_pcm_status_get_trigger_tstamp( st, &t );
            self->underrun = now * 1000 - ((PaTime) t.tv_sec * 1000 + (PaTime) t.tv_usec / 1000);
        }
    }
    if( self->capture.pcm )
    {
        snd_pcm_status( self->capture.pcm, st );
        if( snd_pcm_status_get_state( st ) == SND_PCM_STATE_XRUN )
        {
            snd_pcm_status_get_trigger_tstamp( st, &t );
            self->overrun = now * 1000 - ((PaTime) t.tv_sec * 1000 + (PaTime) t.tv_usec / 1000);
        }
    }

    PA_ENSURE( AlsaRestart( self ) );

end:
    return result;
error:
    goto end;
}

/** Decide if we should continue polling for specified direction, eventually adjust the poll timeout.
 * 
 */
static PaError ContinuePoll( const PaAlsaStream *stream, StreamDirection streamDir, int *pollTimeout, int *continuePoll )
{
    PaError result = paNoError;
    snd_pcm_sframes_t delay, margin;
    int err;
    const PaAlsaStreamComponent *component = NULL, *otherComponent = NULL;

    *continuePoll = 1;

    if( StreamDirection_In == streamDir )
    {
        component = &stream->capture;
        otherComponent = &stream->playback;
    }
    else
    {
        component = &stream->playback;
        otherComponent = &stream->capture;
    }

    /* ALSA docs say that negative delay should indicate xrun, but in my experience snd_pcm_delay returns -EPIPE */
    if( (err = snd_pcm_delay( otherComponent->pcm, &delay )) < 0 )
    {
        if( err == -EPIPE )
        {
            /* Xrun */
            *continuePoll = 0;
            goto error;
        }

        ENSURE_( err, paUnanticipatedHostError );
    }

    if( StreamDirection_Out == streamDir )
    {
        /* Number of eligible frames before capture overrun */
        delay = otherComponent->bufferSize - delay;
    }
    margin = delay - otherComponent->framesPerBuffer / 2;

    if( margin < 0 )
    {
        PA_DEBUG(( "%s: Stopping poll for %s\n", __FUNCTION__, StreamDirection_In == streamDir ? "capture" : "playback" ));
        *continuePoll = 0;
    }
    else if( margin < otherComponent->framesPerBuffer )
    {
        *pollTimeout = CalculatePollTimeout( stream, margin );
        PA_DEBUG(( "%s: Trying to poll again for %s frames, pollTimeout: %d\n",
                    __FUNCTION__, StreamDirection_In == streamDir ? "capture" : "playback", *pollTimeout ));
    }

error:
    return result;
}

/* Callback interface */

static void OnExit( void *data )
{
    PaAlsaStream *stream = (PaAlsaStream *) data;

    assert( data );

    PaUtil_ResetCpuLoadMeasurer( &stream->cpuLoadMeasurer );

    stream->callback_finished = 1;  /* Let the outside world know stream was stopped in callback */
    PA_DEBUG(( "%s: Stopping ALSA handles\n", __FUNCTION__ ));
    AlsaStop( stream, stream->callbackAbort );
    stream->callbackAbort = 0;      /* Clear state */
    
    PA_DEBUG(( "%s: Stoppage\n", __FUNCTION__ ));

    /* Eventually notify user all buffers have played */
    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    stream->isActive = 0;
}

static void CalculateTimeInfo( PaAlsaStream *stream, PaStreamCallbackTimeInfo *timeInfo )
{
    snd_pcm_status_t *capture_status, *playback_status;
    snd_timestamp_t capture_timestamp, playback_timestamp;
    PaTime capture_time = 0., playback_time = 0.;

    snd_pcm_status_alloca( &capture_status );
    snd_pcm_status_alloca( &playback_status );

    if( stream->capture.pcm )
    {
        snd_pcm_sframes_t capture_delay;

        snd_pcm_status( stream->capture.pcm, capture_status );
        snd_pcm_status_get_tstamp( capture_status, &capture_timestamp );

        capture_time = capture_timestamp.tv_sec +
            ((PaTime)capture_timestamp.tv_usec / 1000000.0);
        timeInfo->currentTime = capture_time;

        capture_delay = snd_pcm_status_get_delay( capture_status );
        timeInfo->inputBufferAdcTime = timeInfo->currentTime -
            (PaTime)capture_delay / stream->streamRepresentation.streamInfo.sampleRate;
    }
    if( stream->playback.pcm )
    {
        snd_pcm_sframes_t playback_delay;

        snd_pcm_status( stream->playback.pcm, playback_status );
        snd_pcm_status_get_tstamp( playback_status, &playback_timestamp );

        playback_time = playback_timestamp.tv_sec +
            ((PaTime)playback_timestamp.tv_usec / 1000000.0);

        if( stream->capture.pcm ) /* Full duplex */
        {
            /* Hmm, we have both a playback and a capture timestamp.
             * Hopefully they are the same... */
            if( fabs( capture_time - playback_time ) > 0.01 )
                PA_DEBUG(("Capture time and playback time differ by %f\n", fabs(capture_time-playback_time)));
        }
        else
            timeInfo->currentTime = playback_time;

        playback_delay = snd_pcm_status_get_delay( playback_status );
        timeInfo->outputBufferDacTime = timeInfo->currentTime +
            (PaTime)playback_delay / stream->streamRepresentation.streamInfo.sampleRate;
    }
}

/** Called after buffer processing is finished.
 *
 * A number of mmapped frames is committed, it is possible that an xrun has occurred in the meantime.
 *
 * @param numFrames The number of frames that has been processed
 * @param xrun Return whether an xrun has occurred
 */
static PaError PaAlsaStreamComponent_EndProcessing( PaAlsaStreamComponent *self, unsigned long numFrames, int *xrun )
{
    PaError result = paNoError;
    int res;

    /* @concern FullDuplex It is possible that only one direction is marked ready after polling, and processed
     * afterwards
     */
    if( !self->ready )
        goto end;

    res = snd_pcm_mmap_commit( self->pcm, self->offset, numFrames );
    if( res == -EPIPE || res == -ESTRPIPE )
    {
        *xrun = 1;
    }
    else
    {
        ENSURE_( res, paUnanticipatedHostError );
    }

end:
error:
    return result;
}

/* Extract buffer from channel area */
static unsigned char *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset )
{
    return (unsigned char *) area->addr + (area->first + offset * area->step) / 8;
}

/** Do necessary adaption between user and host channels.
 *
    @concern ChannelAdaption Adapting between user and host channels can involve silencing unused channels and
    duplicating mono information if host outputs come in pairs.
 */
static PaError PaAlsaStreamComponent_DoChannelAdaption( PaAlsaStreamComponent *self, PaUtilBufferProcessor *bp, int numFrames )
{
    PaError result = paNoError;
    unsigned char *p;
    int i;
    int unusedChans = self->numHostChannels - self->numUserChannels;
    unsigned char *src, *dst;
    int convertMono = (self->numHostChannels % 2) == 0 && (self->numUserChannels % 2) != 0;

    assert( StreamDirection_Out == self->streamDir );

    if( self->hostInterleaved )
    {
        int swidth = snd_pcm_format_size( self->nativeFormat, 1 );
        unsigned char *buffer = ExtractAddress( self->channelAreas, self->offset );

        /* Start after the last user channel */
        p = buffer + self->numUserChannels * swidth;

        if( convertMono )
        {
            /* Convert the last user channel into stereo pair */
            src = buffer + (self->numUserChannels - 1) * swidth;
            for( i = 0; i < numFrames; ++i )
            {
                dst = src + swidth;
                memcpy( dst, src, swidth );
                src += self->numHostChannels * swidth;
            }

            /* Don't touch the channel we just wrote to */
            p += swidth;
            --unusedChans;
        }

        if( unusedChans > 0 )
        {
            /* Silence unused output channels */
            for( i = 0; i < numFrames; ++i )
            {
                memset( p, 0, swidth * unusedChans );
                p += self->numHostChannels * swidth;
            }
        }
    }
    else
    {
        /* We extract the last user channel */
        if( convertMono )
        {
            ENSURE_( snd_pcm_area_copy( self->channelAreas + self->numUserChannels, self->offset, self->channelAreas +
                    (self->numUserChannels - 1), self->offset, numFrames, self->nativeFormat ), paUnanticipatedHostError );
            --unusedChans;
        }
        if( unusedChans > 0 )
        {
            snd_pcm_areas_silence( self->channelAreas + (self->numHostChannels - unusedChans), self->offset, unusedChans, numFrames,
                    self->nativeFormat );
        }
    }

error:
    return result;
}

static PaError PaAlsaStream_EndProcessing( PaAlsaStream *self, unsigned long numFrames, int *xrunOccurred )
{
    PaError result = paNoError;
    int xrun = 0;

    if( self->capture.pcm )
    {
        PA_ENSURE( PaAlsaStreamComponent_EndProcessing( &self->capture, numFrames, &xrun ) );
    }
    if( self->playback.pcm )
    {
        if( self->playback.numHostChannels > self->playback.numUserChannels )
            PA_ENSURE( PaAlsaStreamComponent_DoChannelAdaption( &self->playback, &self->bufferProcessor, numFrames ) );
        PA_ENSURE( PaAlsaStreamComponent_EndProcessing( &self->playback, numFrames, &xrun ) );
    }

error:
    *xrunOccurred = xrun;
    return result;
}

/** Update the number of available frames.
 *
 */
static PaError PaAlsaStreamComponent_GetAvailableFrames( PaAlsaStreamComponent *self, unsigned long *numFrames, int *xrunOccurred )
{
    PaError result = paNoError;
    snd_pcm_sframes_t framesAvail = snd_pcm_avail_update( self->pcm );
    *xrunOccurred = 0;

    if( -EPIPE == framesAvail )
    {
        *xrunOccurred = 1;
        framesAvail = 0;
    }
    else
        ENSURE_( framesAvail, paUnanticipatedHostError );

    *numFrames = framesAvail;

error:
    return result;
}

/** Fill in pollfd objects.
 */
static PaError PaAlsaStreamComponent_BeginPolling( PaAlsaStreamComponent* self, struct pollfd* pfds )
{
    PaError result = paNoError;
    int ret = snd_pcm_poll_descriptors( self->pcm, pfds, self->nfds );
    (void)ret;  /* Prevent unused variable warning if asserts are turned off */
    assert( ret == self->nfds );

    self->ready = 0;

    return result;
}

/** Examine results from poll().
 *
 * @param pfds pollfds to inspect
 * @param shouldPoll Should we continue to poll
 * @param xrun Has an xrun occurred
 */
static PaError PaAlsaStreamComponent_EndPolling( PaAlsaStreamComponent* self, struct pollfd* pfds, int* shouldPoll, int* xrun )
{
    PaError result = paNoError;
    unsigned short revents;

    ENSURE_( snd_pcm_poll_descriptors_revents( self->pcm, pfds, self->nfds, &revents ), paUnanticipatedHostError );
    if( revents != 0 )
    {
        if( revents & POLLERR )
        {
            *xrun = 1;
        }
        else
            self->ready = 1;

        *shouldPoll = 0;
    }

error:
    return result;
}

/** Return the number of available frames for this stream.
 *
 * @concern FullDuplex The minimum available for the two directions is calculated, it might be desirable to ignore
 * one direction however (not marked ready from poll), so this is controlled by queryCapture and queryPlayback.
 *
 * @param queryCapture Check available for capture
 * @param queryPlayback Check available for playback
 * @param available The returned number of frames
 * @param xrunOccurred Return whether an xrun has occurred
 */
static PaError PaAlsaStream_GetAvailableFrames( PaAlsaStream *self, int queryCapture, int queryPlayback, unsigned long
        *available, int *xrunOccurred )
{
    PaError result = paNoError;
    unsigned long captureFrames, playbackFrames;
    *xrunOccurred = 0;

    assert( queryCapture || queryPlayback );

    if( queryCapture )
    {
        assert( self->capture.pcm );
        PA_ENSURE( PaAlsaStreamComponent_GetAvailableFrames( &self->capture, &captureFrames, xrunOccurred ) );
        if( *xrunOccurred )
            goto end;
    }
    if( queryPlayback )
    {
        assert( self->playback.pcm );
        PA_ENSURE( PaAlsaStreamComponent_GetAvailableFrames( &self->playback, &playbackFrames, xrunOccurred ) );
        if( *xrunOccurred )
            goto end;
    }

    if( queryCapture && queryPlayback )
    {
        *available = PA_MIN( captureFrames, playbackFrames );
        /*PA_DEBUG(("capture: %lu, playback: %lu, combined: %lu\n", captureFrames, playbackFrames, *available));*/
    }
    else if( queryCapture )
    {
        *available = captureFrames;
    }
    else
    {
        *available = playbackFrames;
    }

end:
error:
    return result;
}

/** Wait for and report available buffer space from ALSA.
 *
 * Unless ALSA reports a minimum of frames available for I/O, we poll the ALSA filedescriptors for more.
 * Both of these operations can uncover xrun conditions.
 *
 * @concern Xruns Both polling and querying available frames can report an xrun condition.
 *
 * @param framesAvail Return the number of available frames
 * @param xrunOccurred Return whether an xrun has occurred
 */ 
static PaError PaAlsaStream_WaitForFrames( PaAlsaStream *self, unsigned long *framesAvail, int *xrunOccurred )
{
    PaError result = paNoError;
    int pollPlayback = self->playback.pcm != NULL, pollCapture = self->capture.pcm != NULL;
    int pollTimeout = self->pollTimeout;
    int xrun = 0;

    assert( self );
    assert( framesAvail );

    if( !self->callbackMode )
    {
        /* In blocking mode we will only wait if necessary */
        PA_ENSURE( PaAlsaStream_GetAvailableFrames( self, self->capture.pcm != NULL, self->playback.pcm != NULL,
                    framesAvail, &xrun ) );
        if( xrun )
        {
            goto end;
        }

        if( *framesAvail > 0 )
        {
            /* Mark pcms ready from poll */
            if( self->capture.pcm )
                self->capture.ready = 1;
            if( self->playback.pcm )
                self->playback.ready = 1;

            goto end;
        }
    }

    while( pollPlayback || pollCapture )
    {
        int totalFds = 0;
        struct pollfd *capturePfds = NULL, *playbackPfds = NULL;

        pthread_testcancel();

        if( pollCapture )
        {
            capturePfds = self->pfds;
            PA_ENSURE( PaAlsaStreamComponent_BeginPolling( &self->capture, capturePfds ) );
            totalFds += self->capture.nfds;
        }
        if( pollPlayback )
        {
            playbackPfds = self->pfds + (self->capture.pcm ? self->capture.nfds : 0);
            PA_ENSURE( PaAlsaStreamComponent_BeginPolling( &self->playback, playbackPfds ) );
            totalFds += self->playback.nfds;
        }
        
        if( poll( self->pfds, totalFds, pollTimeout ) < 0 )
        {
            /*  XXX: Depend on preprocessor condition? */
            if( errno == EINTR )
            {
                /* gdb */
                continue;
            }

            /* TODO: Add macro for checking system calls */
            PA_ENSURE( paInternalError );
        }

        /* check the return status of our pfds */
        if( pollCapture )
        {
            PA_ENSURE( PaAlsaStreamComponent_EndPolling( &self->capture, capturePfds, &pollCapture, &xrun ) );
        }
        if( pollPlayback )
        {
            PA_ENSURE( PaAlsaStreamComponent_EndPolling( &self->playback, playbackPfds, &pollPlayback, &xrun ) );
        }
        if( xrun )
        {
            break;
        }

        /* @concern FullDuplex If only one of two pcms is ready we may want to compromise between the two.
         * If there is less than half a period's worth of samples left of frames in the other pcm's buffer we will
         * stop polling.
         */
        if( self->capture.pcm && self->playback.pcm )
        {
            if( pollCapture && !pollPlayback )
            {
                PA_ENSURE( ContinuePoll( self, StreamDirection_In, &pollTimeout, &pollCapture ) );
            }
            else if( pollPlayback && !pollCapture )
            {
                PA_ENSURE( ContinuePoll( self, StreamDirection_Out, &pollTimeout, &pollPlayback ) );
            }
        }
    }

    if( !xrun )
    {
        /* Get the number of available frames for the pcms that are marked ready.
         * @concern FullDuplex If only one direction is marked ready (from poll), the number of frames available for
         * the other direction is returned. Output is normally preferred over capture however, so capture frames may be
         * discarded to avoid overrun unless paNeverDropInput is specified.
         */
        int captureReady = self->capture.pcm ? self->capture.ready : 0,
            playbackReady = self->playback.pcm ? self->playback.ready : 0;
        PA_ENSURE( PaAlsaStream_GetAvailableFrames( self, captureReady, playbackReady, framesAvail, &xrun ) );

        if( self->capture.pcm && self->playback.pcm )
        {
            if( !self->playback.ready && !self->neverDropInput )
            {
                /* Drop input, a period's worth */
                assert( self->capture.ready );
                PaAlsaStreamComponent_EndProcessing( &self->capture, PA_MIN( self->capture.framesPerBuffer,
                            *framesAvail ), &xrun );
                *framesAvail = 0;
                self->capture.ready = 0;
            }
        }
        else if( self->capture.pcm )
            assert( self->capture.ready );
        else
            assert( self->playback.ready );
    }

end:
error:
    if( xrun )
    {
        /* Recover from the xrun state */
        PA_ENSURE( PaAlsaStream_HandleXrun( self ) );
        *framesAvail = 0;
    }
    else
    {
        if( 0 != *framesAvail )
        {
            /* If we're reporting frames eligible for processing, one of the handles better be ready */
            PA_UNLESS( self->capture.ready || self->playback.ready, paInternalError );
        }
    }
    *xrunOccurred = xrun;

    return result;
}

/** Register per-channel ALSA buffer information with buffer processor.
 *
 * Mmapped buffer space is acquired from ALSA, and registered with the buffer processor. Differences between the
 * number of host and user channels is taken into account.
 * 
 * @param numFrames On entrance the number of requested frames, on exit the number of contiguously accessible frames.
 */
static PaError PaAlsaStreamComponent_RegisterChannels( PaAlsaStreamComponent* self, PaUtilBufferProcessor* bp,
        unsigned long* numFrames, int* xrun )
{
    PaError result = paNoError;
    const snd_pcm_channel_area_t *areas, *area;
    void (*setChannel)(PaUtilBufferProcessor *, unsigned int, void *, unsigned int) =
        StreamDirection_In == self->streamDir ? PaUtil_SetInputChannel : PaUtil_SetOutputChannel;
    unsigned char *buffer, *p;
    int i;
    unsigned long framesAvail;

    /* This _must_ be called before mmap_begin */
    PA_ENSURE( PaAlsaStreamComponent_GetAvailableFrames( self, &framesAvail, xrun ) );
    if( *xrun )
    {
        *numFrames = 0;
        goto end;
    }

    ENSURE_( snd_pcm_mmap_begin( self->pcm, &areas, &self->offset, numFrames ), paUnanticipatedHostError );

    if( self->hostInterleaved )
    {
        int swidth = snd_pcm_format_size( self->nativeFormat, 1 );

        p = buffer = ExtractAddress( areas, self->offset );
        for( i = 0; i < self->numUserChannels; ++i )
        {
            /* We're setting the channels up to userChannels, but the stride will be hostChannels samples */
            setChannel( bp, i, p, self->numHostChannels );
            p += swidth;
        }
    }
    else
    {
        for( i = 0; i < self->numUserChannels; ++i )
        {
            area = areas + i;
            buffer = ExtractAddress( area, self->offset );
            setChannel( bp, i, buffer, 1 );
        }
    }

    /* @concern ChannelAdaption Buffer address is recorded so we can do some channel adaption later */
    self->channelAreas = (snd_pcm_channel_area_t *)areas;

end:
error:
    return result;
}

/** Initiate buffer processing.
 *
 * ALSA buffers are registered with the PA buffer processor and the buffer size (in frames) set.
 *
 * @concern FullDuplex If both directions are being processed, the minimum amount of frames for the two directions is
 * calculated.
 *
 * @param numFrames On entrance the number of available frames, on exit the number of received frames
 * @param xrunOccurred Return whether an xrun has occurred
 */
static PaError PaAlsaStream_SetUpBuffers( PaAlsaStream* self, unsigned long* numFrames, int* xrunOccurred )
{
    PaError result = paNoError;
    unsigned long captureFrames = ULONG_MAX, playbackFrames = ULONG_MAX, commonFrames = 0;
    int xrun = 0;

    if( *xrunOccurred )
    {
        *numFrames = 0;
        return result;
    }
    /* If we got here at least one of the pcm's should be marked ready */
    PA_UNLESS( self->capture.ready || self->playback.ready, paInternalError );

    /* Extract per-channel ALSA buffer pointers and register them with the buffer processor.
     * It is possible that a direction is not marked ready however, because it is out of sync with the other.
     */
    if( self->capture.pcm && self->capture.ready )
    {
        captureFrames = *numFrames;
        PA_ENSURE( PaAlsaStreamComponent_RegisterChannels( &self->capture, &self->bufferProcessor, &captureFrames, 
                    &xrun ) );
    }
    if( self->playback.pcm && self->playback.ready )
    {
        playbackFrames = *numFrames;
        PA_ENSURE( PaAlsaStreamComponent_RegisterChannels( &self->playback, &self->bufferProcessor, &playbackFrames, 
                    &xrun ) );
    }
    if( xrun )
    {
        /* Nothing more to do */
        assert( 0 == commonFrames );
        goto end;
    }

    commonFrames = PA_MIN( captureFrames, playbackFrames );
    /* assert( commonFrames <= *numFrames ); */
    if( commonFrames > *numFrames )
    {
        /* Hmmm ... how come there are more frames available than we requested!? Blah. */
        PA_DEBUG(( "%s: Common available frames are reported to be more than number requested: %lu, %lu, callbackMode: %d\n", __FUNCTION__,
                    commonFrames, *numFrames, self->callbackMode ));
        if( self->capture.pcm )
        {
            PA_DEBUG(( "%s: captureFrames: %lu, capture.ready: %d\n", __FUNCTION__, captureFrames, self->capture.ready ));
        }
        if( self->playback.pcm )
        {
            PA_DEBUG(( "%s: playbackFrames: %lu, playback.ready: %d\n", __FUNCTION__, playbackFrames, self->playback.ready ));
        }
        
        commonFrames = 0;
        goto end;
    }

    /* Inform PortAudio of the number of frames we got.
     * @concern FullDuplex We might be experiencing underflow in either end; if its an input underflow, we go on
     * with output. If its output underflow however, depending on the paNeverDropInput flag, we may want to simply
     * discard the excess input or call the callback with paOutputOverflow flagged.
     */
    if( self->capture.pcm )
    {
        if( self->capture.ready )
        {
            PaUtil_SetInputFrameCount( &self->bufferProcessor, commonFrames );
        }
        else
        {
            /* We have input underflow */
            PaUtil_SetNoInput( &self->bufferProcessor );
        }
    }
    if( self->playback.pcm )
    {
        if( self->playback.ready )
        {
            PaUtil_SetOutputFrameCount( &self->bufferProcessor, commonFrames );
        }
        else
        {
            /* We have output underflow, but keeping input data (paNeverDropInput) */
            assert( self->neverDropInput );
            assert( self->capture.pcm != NULL );
            PA_DEBUG(( "%s: Setting output buffers to NULL\n", __FUNCTION__ ));
            PaUtil_SetNoOutput( &self->bufferProcessor );
        }
    }
    
end:
    *numFrames = commonFrames;
error:
    if( xrun )
    {
        PA_ENSURE( PaAlsaStream_HandleXrun( self ) );
        *numFrames = 0;
    }
    *xrunOccurred = xrun;

    return result;
}

/** Callback thread's function.
 *
 * Roughly, the workflow can be described in the following way: The number of available frames that can be processed
 * directly is obtained from ALSA, we then request as much directly accessible memory as possible within this amount
 * from ALSA. The buffer memory is registered with the PA buffer processor and processing is carried out with
 * PaUtil_EndBufferProcessing. Finally, the number of processed frames is reported to ALSA. The processing can
 * happen in several iterations untill we have consumed the known number of available frames (or an xrun is detected).
 */
static void *CallbackThreadFunc( void *userData )
{
    PaError result = paNoError, *pres = NULL;
    PaAlsaStream *stream = (PaAlsaStream*) userData;
    PaStreamCallbackTimeInfo timeInfo = {0, 0, 0};
    snd_pcm_sframes_t startThreshold = 0;
    int callbackResult = paContinue;
    PaStreamCallbackFlags cbFlags = 0;  /* We might want to keep state across iterations */
    int streamStarted = 0;

    assert( stream );

    callbackThread_ = pthread_self();
    /* Execute OnExit when exiting */
    pthread_cleanup_push( &OnExit, stream );

    /* Not implemented */
    assert( !stream->primeBuffers );

    /* @concern StreamStart If the output is being primed the output pcm needs to be prepared, otherwise the
     * stream is started immediately. The latter involves signaling the waiting main thread.
     */
    if( stream->primeBuffers )
    {
        snd_pcm_sframes_t avail;
        
        if( stream->playback.pcm )
            ENSURE_( snd_pcm_prepare( stream->playback.pcm ), paUnanticipatedHostError );
        if( stream->capture.pcm && !stream->pcmsSynced )
            ENSURE_( snd_pcm_prepare( stream->capture.pcm ), paUnanticipatedHostError );

        /* We can't be certain that the whole ring buffer is available for priming, but there should be
         * at least one period */
        avail = snd_pcm_avail_update( stream->playback.pcm );
        startThreshold = avail - (avail % stream->playback.framesPerBuffer);
        assert( startThreshold >= stream->playback.framesPerBuffer );
    }
    else
    {
        PA_ENSURE( LockMutex( &stream->startMtx ) );
        /* Buffer will be zeroed */
        PA_ENSURE( AlsaStart( stream, 0 ) );
        ENSURE_SYSTEM_( pthread_cond_signal( &stream->startCond ), 0 );
        PA_ENSURE( UnlockMutex( &stream->startMtx ) );

        streamStarted = 1;
    }

    while( 1 )
    {
        unsigned long framesAvail, framesGot;
        int xrun = 0;

        pthread_testcancel();

        /* @concern StreamStop if the main thread has requested a stop and the stream has not been effectively
         * stopped we signal this condition by modifying callbackResult (we'll want to flush buffered output).
         */
        if( stream->callbackStop && paContinue == callbackResult )
        {
            PA_DEBUG(( "Setting callbackResult to paComplete\n" ));
            callbackResult = paComplete;
        }

        if( paContinue != callbackResult )
        {
            stream->callbackAbort = (paAbort == callbackResult);
            if( stream->callbackAbort ||
                    /** @concern BlockAdaption: Go on if adaption buffers are empty */
                    PaUtil_IsBufferProcessorOutputEmpty( &stream->bufferProcessor ) ) 
                goto end;

            PA_DEBUG(( "%s: Flushing buffer processor\n", __FUNCTION__ ));
            /* There is still buffered output that needs to be processed */
        }

        /* Wait for data to become available, this comes down to polling the ALSA file descriptors untill we have
         * a number of available frames.
         */
        PA_ENSURE( PaAlsaStream_WaitForFrames( stream, &framesAvail, &xrun ) );
        if( xrun )
        {
            assert( 0 == framesAvail );
            continue;

            /* XXX: Report xruns to the user? A situation is conceivable where the callback is never invoked due
             * to constant xruns, it might be desirable to notify the user of this.
             */
        }

        /* Consume buffer space. Once we have a number of frames available for consumption we must retrieve the
         * mmapped buffers from ALSA, this is contiguously accessible memory however, so we may receive smaller
         * portions at a time than is available as a whole. Therefore we should be prepared to process several
         * chunks successively. The buffers are passed to the PA buffer processor.
         */
        while( framesAvail > 0 )
        {
            xrun = 0;

            pthread_testcancel();

            /** @concern Xruns Under/overflows are to be reported to the callback */
            if( stream->underrun > 0.0 )
            {
                cbFlags |= paOutputUnderflow;
                stream->underrun = 0.0;
            }
            if( stream->overrun > 0.0 )
            {
                cbFlags |= paInputOverflow;
                stream->overrun = 0.0;
            }
            if( stream->capture.pcm && stream->playback.pcm )
            {
                /** @concern FullDuplex It's possible that only one direction is being processed to avoid an
                 * under- or overflow, this should be reported correspondingly */
                if( !stream->capture.ready )
                {
                    cbFlags |= paInputUnderflow;
                    PA_DEBUG(( "%s: Input underflow\n", __FUNCTION__ ));
                }
                else if( !stream->playback.ready )
                {
                    cbFlags |= paOutputOverflow;
                    PA_DEBUG(( "%s: Output overflow\n", __FUNCTION__ ));
                }
            }

            CallbackUpdate( &stream->threading );
            CalculateTimeInfo( stream, &timeInfo );
            PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, cbFlags );
            cbFlags = 0;

            /* CPU load measurement should include processing activivity external to the stream callback */
            PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

            framesGot = framesAvail;
            if( paUtilFixedHostBufferSize == stream->bufferProcessor.hostBufferSizeMode )
            {
                /* We've committed to a fixed host buffer size, stick to that */
                framesGot = framesGot >= stream->maxFramesPerHostBuffer ? stream->maxFramesPerHostBuffer : 0;
            }
            else
            {
                /* We've committed to an upper bound on the size of host buffers */
                assert( paUtilBoundedHostBufferSize == stream->bufferProcessor.hostBufferSizeMode );
                framesGot = PA_MIN( framesGot, stream->maxFramesPerHostBuffer );
            }
            PA_ENSURE( PaAlsaStream_SetUpBuffers( stream, &framesGot, &xrun ) );
            /* Check the host buffer size against the buffer processor configuration */
            framesAvail -= framesGot;

            if( framesGot > 0 )
            {
                assert( !xrun );
                PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );
                PA_ENSURE( PaAlsaStream_EndProcessing( stream, framesGot, &xrun ) );
            }
            PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesGot );

            if( 0 == framesGot )
            {
                /* Go back to polling for more frames */
                break;

            }

            if( paContinue != callbackResult )
                break;
        }
    }

    /* Match pthread_cleanup_push */
    pthread_cleanup_pop( 1 );

end:
    PA_DEBUG(( "%s: Thread %d exiting\n ", __FUNCTION__, pthread_self() ));
    pthread_exit( pres );

error:
    /* Pass on error code */
    pres = malloc( sizeof (PaError) );
    *pres = result;
    
    goto end;
}

/* Blocking interface */

static PaError ReadStream( PaStream* s, void *buffer, unsigned long frames )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    unsigned long framesGot, framesAvail;
    void *userBuffer;
    snd_pcm_t *save = stream->playback.pcm;

    assert( stream );

    PA_UNLESS( stream->capture.pcm, paCanNotReadFromAnOutputOnlyStream );

    /* Disregard playback */
    stream->playback.pcm = NULL;

    if( stream->overrun > 0. )
    {
        result = paInputOverflowed;
        stream->overrun = 0.0;
    }

    if( stream->capture.userInterleaved )
    {
        userBuffer = buffer;
    }
    else
    {
        /* Copy channels into local array */
        userBuffer = stream->capture.userBuffers;
        memcpy( userBuffer, buffer, sizeof (void *) * stream->capture.numUserChannels );
    }

    /* Start stream if in prepared state */
    if( snd_pcm_state( stream->capture.pcm ) == SND_PCM_STATE_PREPARED )
    {
        ENSURE_( snd_pcm_start( stream->capture.pcm ), paUnanticipatedHostError );
    }

    while( frames > 0 )
    {
        int xrun = 0;
        PA_ENSURE( PaAlsaStream_WaitForFrames( stream, &framesAvail, &xrun ) );
        framesGot = PA_MIN( framesAvail, frames );

        PA_ENSURE( PaAlsaStream_SetUpBuffers( stream, &framesGot, &xrun ) );
        if( framesGot > 0 )
        {
            framesGot = PaUtil_CopyInput( &stream->bufferProcessor, &userBuffer, framesGot );
            PA_ENSURE( PaAlsaStream_EndProcessing( stream, framesGot, &xrun ) );
            frames -= framesGot;
        }
    }

end:
    stream->playback.pcm = save;
    return result;
error:
    goto end;
}

static PaError WriteStream( PaStream* s, const void *buffer, unsigned long frames )
{
    PaError result = paNoError;
    signed long err;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    snd_pcm_uframes_t framesGot, framesAvail;
    const void *userBuffer;
    snd_pcm_t *save = stream->capture.pcm;
    
    assert( stream );

    PA_UNLESS( stream->playback.pcm, paCanNotWriteToAnInputOnlyStream );

    /* Disregard capture */
    stream->capture.pcm = NULL;

    if( stream->underrun > 0. )
    {
        result = paOutputUnderflowed;
        stream->underrun = 0.0;
    }

    if( stream->playback.userInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        userBuffer = stream->playback.userBuffers;
        memcpy( (void *)userBuffer, buffer, sizeof (void *) * stream->playback.numUserChannels );
    }

    while( frames > 0 )
    {
        int xrun = 0;
        snd_pcm_uframes_t hwAvail;

        PA_ENSURE( PaAlsaStream_WaitForFrames( stream, &framesAvail, &xrun ) );
        framesGot = PA_MIN( framesAvail, frames );

        PA_ENSURE( PaAlsaStream_SetUpBuffers( stream, &framesGot, &xrun ) );
        if( framesGot > 0 )
        {
            framesGot = PaUtil_CopyOutput( &stream->bufferProcessor, &userBuffer, framesGot );
            PA_ENSURE( PaAlsaStream_EndProcessing( stream, framesGot, &xrun ) );
            frames -= framesGot;
        }

        /* Start stream after one period of samples worth */

        /* Frames residing in buffer */
        PA_ENSURE( err = GetStreamWriteAvailable( stream ) );
        framesAvail = err;
        hwAvail = stream->playback.bufferSize - framesAvail;

        if( snd_pcm_state( stream->playback.pcm ) == SND_PCM_STATE_PREPARED &&
                hwAvail >= stream->playback.framesPerBuffer )
        {
            ENSURE_( snd_pcm_start( stream->playback.pcm ), paUnanticipatedHostError );
        }
    }

end:
    stream->capture.pcm = save;
    return result;
error:
    goto end;
}

/* Return frames available for reading. In the event of an overflow, the capture pcm will be restarted */
static signed long GetStreamReadAvailable( PaStream* s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    unsigned long avail;
    int xrun;

    PA_ENSURE( PaAlsaStreamComponent_GetAvailableFrames( &stream->capture, &avail, &xrun ) );
    if( xrun )
    {
        PA_ENSURE( PaAlsaStream_HandleXrun( stream ) );
        PA_ENSURE( PaAlsaStreamComponent_GetAvailableFrames( &stream->capture, &avail, &xrun ) );
        if( xrun )
            PA_ENSURE( paInputOverflowed );
    }

    return (signed long)avail;

error:
    return result;
}

static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    unsigned long avail;
    int xrun;

    PA_ENSURE( PaAlsaStreamComponent_GetAvailableFrames( &stream->playback, &avail, &xrun ) );
    if( xrun )
    {
        snd_pcm_sframes_t savail;

        PA_ENSURE( PaAlsaStream_HandleXrun( stream ) );
        savail = snd_pcm_avail_update( stream->playback.pcm );

        /* savail should not contain -EPIPE now, since PaAlsaStream_HandleXrun will only prepare the pcm */
        ENSURE_( savail, paUnanticipatedHostError );

        avail = (unsigned long) savail;
    }

    return (signed long)avail;

error:
    return result;
}

/* Extensions */

/* Initialize host api specific structure */
void PaAlsa_InitializeStreamInfo( PaAlsaStreamInfo *info )
{
    info->size = sizeof (PaAlsaStreamInfo);
    info->hostApiType = paALSA;
    info->version = 1;
    info->deviceString = NULL;
}

void PaAlsa_EnableRealtimeScheduling( PaStream *s, int enable )
{
    PaAlsaStream *stream = (PaAlsaStream *) s;
    stream->threading.rtSched = enable;
}

void PaAlsa_EnableWatchdog( PaStream *s, int enable )
{
    PaAlsaStream *stream = (PaAlsaStream *) s;
    stream->threading.useWatchdog = enable;
}
