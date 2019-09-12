

#include "audio_player.h"
#include <mmdeviceapi.h>
#include <iostream>
#include <comdef.h>
#include <queue>
#include <vector>
#include "log.h"

#define EXIT_ON_ERROR(hres) if (FAILED(hres)) break;				


#define SAFE_RELEASE(punk) if ((punk) != NULL) { (punk)->Release(); (punk) = NULL; }


WAVEFORMATEX AudioPlayer::AOA_FORMAT =
{
	WAVE_FORMAT_PCM, //WORD  wFormatTag;
	2, //WORD  nChannels;
	44100, //DWORD nSamplesPerSec;
	44100 * 4, //DWORD nAvgBytesPerSec;
	4, //WORD  nBlockAlign;
	16, //WORD  wBitsPerSample;
	0, //WORD  cbSize;
};

float AudioPlayer::CACHE_LENGTH_IN_SECS = 0.5;

AudioPlayer::AudioPlayer()
{
	
	_usbHandle = INVALID_HANDLE_VALUE;
	_receiverThreadHandle = INVALID_HANDLE_VALUE;
	
	_running = false;

	_audioClient = NULL;
	_renderClient = NULL;
	_audioFormat = NULL;
	

}

AudioPlayer::~AudioPlayer()
{

	if (_audioFormat != NULL)
		delete _audioFormat;

	SAFE_RELEASE(_renderClient)
	SAFE_RELEASE(_audioClient)
	
}



UINT32 AudioPlayer::ConvertAudio(UCHAR *output, const UCHAR *head, const UINT32 validLength, unsigned char bytesPerChannelAfterConversion,
	const double samplingInterval, const bool replicating, double &samplingCount)
{
	//replicating is true if there is a need of upsampling.
	//In that case, the upsampling is done by replicating each sample in the incomming frame buffer, so that the total number of samples gets doubled,
	//then new samples are generated by sampling from the enlarged frame buffer at the desired sample rate
	UINT32 writeIndex = 0;			

	for (UINT32 byteIndex = 0; byteIndex < validLength; byteIndex += AOA_FORMAT.nBlockAlign)
	{
		UINT32 writeStartIndex = writeIndex;
		if (replicating)
			samplingCount += 2.0;
		else
			samplingCount += 1.0;

		UINT32 samplingTimes = (samplingCount / samplingInterval);
		if (samplingTimes >= 1)
		{
			samplingCount -= samplingInterval;
			for (unsigned char channel = 0; channel < AOA_FORMAT.nChannels; ++channel)
			{
				float ratio = 0, rescaled = 0;

				//the incoming data of the AOA fromat contains channels each of which takes two bytes 
				short sample16 = *((short*)&(head[byteIndex + 2 * channel]));
				float denominator = 0x8000;
				ratio = (float)sample16 / denominator;
				if (ratio > 1.0) ratio = 1.0;
				if (ratio < -1.0) ratio = -1.0;







				UCHAR *ptr = NULL;
				char sample8 = 0;
				int sample32 = 0;



				if (_actualFormatTag == WAVE_FORMAT_PCM)
				{
					if (bytesPerChannelAfterConversion == 1)
					{
						denominator = (float)0x80;
						rescaled = (ratio * denominator);
						if (rescaled >= denominator) rescaled = denominator - 1;
						if (rescaled < -denominator) rescaled = -denominator;
						sample8 = (char)rescaled;
						ptr = (UCHAR *)&sample8;

					}
					else if (bytesPerChannelAfterConversion == 2)
					{
						ptr = (UCHAR *)&sample16;

					}
					else if (bytesPerChannelAfterConversion == 4)
					{
						denominator = (float)0x80000000;
						rescaled = (ratio * denominator);
						if (rescaled >= denominator) rescaled = denominator - 1;
						if (rescaled < -denominator) rescaled = -denominator;
						sample32 = (int)rescaled;
						ptr = (UCHAR *)&sample32;

					}
					else
					{
						//The conversino is not supported
						return 0;
					}
				}
				else if (_actualFormatTag == WAVE_FORMAT_IEEE_FLOAT)
				{
					ptr = (UCHAR *)&ratio;

				}
				else
				{
					//The conversino is not supported
					return 0;
				}

				memcpy(&output[writeIndex], ptr, bytesPerChannelAfterConversion);
				writeIndex += bytesPerChannelAfterConversion;


			}
		}

		
		if (samplingTimes >= 2)
		{
			samplingCount -= samplingInterval;

			memcpy(&output[writeIndex], &output[writeIndex - bytesPerChannelAfterConversion * AOA_FORMAT.nChannels], bytesPerChannelAfterConversion * AOA_FORMAT.nChannels);
			writeIndex += bytesPerChannelAfterConversion * AOA_FORMAT.nChannels;
		}
		
	}
	
	

	return writeIndex;
}


DWORD WINAPI AudioPlayer::Receive(LPVOID lpParam)
{		

	AudioPlayer *player = (AudioPlayer *)lpParam;		
	HRESULT hr = S_OK;

	UINT32 bufferFrameCount;
	UINT32 numFramesAvailable;
	UINT32 numFramesPadding;
	BYTE *pData = NULL;
	

	// Get the actual size of the allocated frame buffer.
	hr = HRESULT_FROM_WIN32(player->_audioClient->GetBufferSize(&bufferFrameCount));
	if (FAILED(hr))
	{
		LOGE("%s: %s", __FUNCTION__, _com_error(hr).ErrorMessage());
		return hr;
	}



	
	UCHAR *tempBuffer = new UCHAR[bufferFrameCount * player->_audioFormat->nBlockAlign];
	
	
	unsigned char bytesPerChannelAfterConversion = player->_audioFormat->nBlockAlign / player->_audioFormat->nChannels;		
	int resamplingPeriod = player->_audioFormat->nSamplesPerSec - AOA_FORMAT.nSamplesPerSec;
	
	bool replicating;
	double samplingInterval;
	if (AOA_FORMAT.nSamplesPerSec >= player->_audioFormat->nSamplesPerSec)
	{
		replicating = false;
		samplingInterval = (double)AOA_FORMAT.nSamplesPerSec / player->_audioFormat->nSamplesPerSec;
	}
	else
	{
		replicating = true;
		samplingInterval = (double)AOA_FORMAT.nSamplesPerSec * 2.0 / player->_audioFormat->nSamplesPerSec;
	}
	
	double samplingCounter = 0.0;
	
	

	WINUSB_ISOCH_BUFFER_HANDLE isochReadBufferHandle = INVALID_HANDLE_VALUE;
	LPOVERLAPPED overlapped = NULL;
	

	//only placing 1 packet of the maximum size in each transfer, so that at every polling interval there will be a finished transfer and the data of it will be sent back 
	ULONG isochInPacketCount = 1;	
	ULONG isochInTransferSize = player->_pipe->MaximumBytesPerInterval * isochInPacketCount;	

	//Note that Too larger AudioPlayer::CACHE_LENGTH_IN_SECS might be undesired, 
	//since there that will induce a longer delay between the current sound generated by a phone and the sound you actually hear.
	ULONG bytesOfCache = AOA_FORMAT.nAvgBytesPerSec * AudioPlayer::CACHE_LENGTH_IN_SECS;
	ULONG numberOfTransfers = bytesOfCache / isochInTransferSize;
	if (bytesOfCache % isochInTransferSize != 0)
		numberOfTransfers += 1;
		
	
	UCHAR *readBuffer = new UCHAR[isochInTransferSize * numberOfTransfers];			
	USBD_ISO_PACKET_DESCRIPTOR *isochPackets = new USBD_ISO_PACKET_DESCRIPTOR[isochInPacketCount*numberOfTransfers];
		
	std::queue<int> tranferIndexQueue;

	bool continuous = true;

	do
	{
		

		overlapped = new OVERLAPPED[numberOfTransfers];
		ZeroMemory(overlapped, (sizeof(OVERLAPPED) * numberOfTransfers));

		for (int transfer = 0; transfer < numberOfTransfers; ++transfer)
		{
			overlapped[transfer].hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

			if (overlapped[transfer].hEvent == NULL)
			{
				hr = HRESULT_FROM_WIN32(GetLastError());
				break;
			}
			tranferIndexQueue.push(transfer);
		}
		if (FAILED(hr))
			break;

		

		ZeroMemory(readBuffer, (isochInTransferSize)* numberOfTransfers);

		if (!WinUsb_RegisterIsochBuffer(
			player->_usbHandle,
			player->_pipe->PipeId,
			readBuffer,
			(isochInTransferSize) * numberOfTransfers,
			&isochReadBufferHandle))
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			break;
		}

	
		ZeroMemory(isochPackets, sizeof(USBD_ISO_PACKET_DESCRIPTOR) * isochInPacketCount*numberOfTransfers);
		for (int transfer = 0; transfer < numberOfTransfers; ++transfer)
		{
			
			if (!WinUsb_ReadIsochPipeAsap(
				isochReadBufferHandle,
				(isochInTransferSize)*transfer,
				isochInTransferSize,
				(transfer == 0) ? FALSE : TRUE,
				//FALSE,
				isochInPacketCount,
				&isochPackets[isochInPacketCount*transfer],
				&overlapped[transfer]))				
			{
				DWORD lastError = GetLastError();
				if (lastError != ERROR_IO_PENDING)
				{
					hr = HRESULT_FROM_WIN32(lastError);
					break;					
				}							
				
			}
			
				
		}
		if (FAILED(hr))
			break;

		hr = HRESULT_FROM_WIN32(player->_audioClient->Start());
		if (FAILED(hr))  
			break;

	} while (false);

	while (SUCCEEDED(hr) && player->_running)
	{
		int nextTransfer = tranferIndexQueue.front();
		tranferIndexQueue.pop();
		DWORD numBytes;
		
		if (WinUsb_GetOverlappedResult(
			player->_usbHandle,
			&overlapped[nextTransfer],
			&numBytes,
			TRUE))
		{
			
			for (int packetIndex = 0; packetIndex < isochInPacketCount; ++packetIndex)
			{
				USBD_ISO_PACKET_DESCRIPTOR *currentPacket = &isochPackets[isochInPacketCount*nextTransfer + packetIndex];
				UCHAR *currentHead = &readBuffer[isochInTransferSize*nextTransfer] + currentPacket->Offset;
				ULONG validLength = currentPacket->Length;
				
				if (validLength == 0)
					continue;


				// See how much buffer space is available.		
				hr = player->_audioClient->GetCurrentPadding(&numFramesPadding);
				EXIT_ON_ERROR(hr)				
				numFramesAvailable = bufferFrameCount - numFramesPadding;
				if (numFramesAvailable == 0)
					continue;
							

				UINT32 lengthOfConvertedData = player->ConvertAudio(tempBuffer, currentHead, validLength, bytesPerChannelAfterConversion,
					samplingInterval, replicating, samplingCounter);
				UINT32 framesToAudioEngine = lengthOfConvertedData / player->_audioFormat->nBlockAlign;

				// if the free space of the frame buffer is insufficient, copying frame as much as the frame buffer can accommodate, and throwing away the rest
				if (framesToAudioEngine > numFramesAvailable)
				{
					framesToAudioEngine = numFramesAvailable;
					lengthOfConvertedData = framesToAudioEngine * player->_audioFormat->nBlockAlign;
				}


				// Grab all the needed space in the shared buffer.				
				hr = player->_renderClient->GetBuffer(framesToAudioEngine, &pData);
				EXIT_ON_ERROR(hr)

				memcpy(pData, tempBuffer, lengthOfConvertedData);								

						
				hr = player->_renderClient->ReleaseBuffer(framesToAudioEngine, 0);
				EXIT_ON_ERROR(hr)

											

			}															

			

		}
		else
		{			

			DWORD lastError = GetLastError();
			// if there is an discontinuity of frames between two adjacent transfers read with the flag 'continuous' on, WinUsb_GetOverlappedResult will report back ERROR_INVALID_PARAMETER.
			// Resetting this flag here when this error is encountered, so that WIMUSB can treat the reading of the following transfers as a new sequenece.
			if (lastError == ERROR_INVALID_PARAMETER)
				continuous = false;
			else
			{
				hr = HRESULT_FROM_WIN32(lastError);
				break;
				//LOGW("%s: %s", __FUNCTION__, _com_error(hr).ErrorMessage());
			}
			
		}

		EXIT_ON_ERROR(hr)

		

		tranferIndexQueue.push(nextTransfer);
		ZeroMemory(&isochPackets[isochInPacketCount*nextTransfer], sizeof(USBD_ISO_PACKET_DESCRIPTOR) * isochInPacketCount);
		
		HANDLE eventHandle = overlapped[nextTransfer].hEvent;
		ZeroMemory(&overlapped[nextTransfer], sizeof(OVERLAPPED));
		overlapped[nextTransfer].hEvent = eventHandle;
		//overlapped[nextTransfer].hEvent= CreateEvent(NULL, TRUE, FALSE, NULL);
		//CloseHandle(eventHandle);

		ResetEvent(overlapped[nextTransfer].hEvent);
		/*if (!ResetEvent(overlapped[nextTransfer].hEvent))
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			break;
		}*/
	

			
		if (!WinUsb_ReadIsochPipeAsap(
			isochReadBufferHandle,
			(isochInTransferSize)*nextTransfer,
			isochInTransferSize,
			continuous,
			isochInPacketCount,
			&isochPackets[isochInPacketCount*nextTransfer],
			&overlapped[nextTransfer]))	
		{
			DWORD lastError = GetLastError();
			if (lastError != ERROR_IO_PENDING)
			{
				hr = HRESULT_FROM_WIN32(GetLastError());
				break;
				//LOGW("%s: %s", __FUNCTION__, _com_error(hr)).ErrorMessage());
				//continuous = false;
			}
			else
				continuous = true;
			
		}
		
		

	}			
	
	
	if (FAILED(hr))
		LOGE("%s: %s", __FUNCTION__, _com_error(hr).ErrorMessage());

	player->Stop();
		
	if (isochReadBufferHandle != INVALID_HANDLE_VALUE)
		WinUsb_UnregisterIsochBuffer(isochReadBufferHandle);

	if (readBuffer != NULL)
		delete[] readBuffer;
	if (tempBuffer != NULL)
		delete[] tempBuffer;

	if (isochPackets != NULL)	
		delete[] isochPackets;

	for (int transfer = 0; transfer < numberOfTransfers; ++transfer)
	{
		if (overlapped[transfer].hEvent != NULL)
		{
			CloseHandle(overlapped[transfer].hEvent);
		}

	}

	if (overlapped != NULL)
	{
		delete[] overlapped;
	}
	
	

	return hr;
	
}

bool AudioPlayer::Prepare()
{
	// the unit of the reference time used by audio engine is 100 nanoseconds
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

	static const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
	static const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
	static const IID IID_IAudioClient = __uuidof(IAudioClient);
	static const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);


	HRESULT hr = S_OK;
	REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
	//REFERENCE_TIME hnsActualDuration;


	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDevice *pDevice = NULL;

	WAVEFORMATEX *pwfx = NULL;


	if (_audioFormat != NULL)
		delete _audioFormat;

	SAFE_RELEASE(_renderClient)
	SAFE_RELEASE(_audioClient)


	do
	{


		/*hr = pAudioClient->GetMixFormat(&pwfx);
		if (FAILED(hr)) break;*/

		hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		EXIT_ON_ERROR(hr)

			hr = CoCreateInstance(
			CLSID_MMDeviceEnumerator, NULL,
			CLSCTX_ALL, IID_IMMDeviceEnumerator,
			(void**)&pEnumerator);
		EXIT_ON_ERROR(hr)

			hr = pEnumerator->GetDefaultAudioEndpoint(
			eRender, eConsole, &pDevice);
		EXIT_ON_ERROR(hr)

			hr = pDevice->Activate(
			IID_IAudioClient, CLSCTX_ALL,
			NULL, (void**)&_audioClient);
		EXIT_ON_ERROR(hr)


			hr = _audioClient->IsFormatSupported(
			AUDCLNT_SHAREMODE_SHARED,
			&AOA_FORMAT,
			&pwfx
			);
		EXIT_ON_ERROR(hr)


		WAVEFORMATEX *targetFormat = NULL;
		if (pwfx != NULL)
		{
			targetFormat = pwfx;

		}
		else
		{
			targetFormat = &AOA_FORMAT;
		}

		EXIT_ON_ERROR(hr)

			if (targetFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				WAVEFORMATEXTENSIBLE *extFormat = (WAVEFORMATEXTENSIBLE *)targetFormat;
				if (!memcmp(&extFormat->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM, sizeof(GUID)))
					_actualFormatTag = WAVE_FORMAT_PCM;
				else if (!memcmp(&extFormat->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(GUID)))
					_actualFormatTag = WAVE_FORMAT_IEEE_FLOAT;
				else
				{
					hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
					break;
				}

				_audioFormat = (WAVEFORMATEX*)new WAVEFORMATEXTENSIBLE;
				memcpy(_audioFormat, targetFormat, sizeof(WAVEFORMATEXTENSIBLE));
			}
			else if (targetFormat->wFormatTag == WAVE_FORMAT_PCM)
			{
				_actualFormatTag = WAVE_FORMAT_PCM;
				_audioFormat = (WAVEFORMATEX*)new WAVEFORMATEX;
				memcpy(_audioFormat, targetFormat, sizeof(WAVEFORMATEX));
			}
			else
			{
				hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
				break;
			}


			unsigned char channelWidth = _audioFormat->nBlockAlign / _audioFormat->nChannels;
			if (channelWidth != 1 &&
				channelWidth != 2 &&
				channelWidth != 4)
			{
				hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
				break;
			}

			hr = _audioClient->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				0,
				hnsRequestedDuration*AudioPlayer::CACHE_LENGTH_IN_SECS,
				0,
				_audioFormat,
				NULL);
			EXIT_ON_ERROR(hr)




				hr = _audioClient->GetService(
				IID_IAudioRenderClient,
				(void**)&_renderClient);
			EXIT_ON_ERROR(hr)






				



	} while (false);

	if (pwfx != NULL)
		CoTaskMemFree(pwfx);

	SAFE_RELEASE(pEnumerator)
	SAFE_RELEASE(pDevice)

	if (FAILED(hr))
	{
		if (_audioFormat != NULL)
			delete _audioFormat;

		SAFE_RELEASE(_renderClient)
		SAFE_RELEASE(_audioClient)
		LOGE("Fail to initialise the audio player: %s", _com_error(hr).ErrorMessage());
		return false;
	}
	else
		return true;

}


void AudioPlayer::Stop()
{			
	_running = false;
	_audioClient->Stop();
}

void AudioPlayer::Wait()
{
	while (_running)
		Sleep(1000);

	WaitForSingleObject(_receiverThreadHandle, INFINITE);
	CloseHandle(_receiverThreadHandle);


}


bool AudioPlayer::Start(const WINUSB_INTERFACE_HANDLE usbHandle, const WINUSB_PIPE_INFORMATION_EX &pipe)
{
	

	DWORD   threadId;
	_usbHandle = usbHandle;
	_pipe = &pipe;

	_running = true;

	if (!Prepare())
		return false;



	_receiverThreadHandle = CreateThread(
		NULL,                   // default security attributes
		0,                      // use default stack size  
		&AudioPlayer::Receive,       // thread function name
		this,          // argument to thread function 
		0,                      // use default creation flags 
		&threadId);   // returns the thread identifier

	if (_receiverThreadHandle == INVALID_HANDLE_VALUE)
	{
		LOGE("Could not start the audio player");		
		return false;
	}

	return true;
}
