# winusb-usbaudio

This project is aimed at implementing similar functions to the ones provided by [USBaudio](https://github.com/rom1v/usbaudio), but on Windows systems. Feel free to fork and modify it to meet your need.

## Prerequisites

* WINUSB API is used to communicate devices through ADB interface to put them into the accessory mode, so "Developer options" of Android must be enabled.
* [zadig](https://zadig.akeo.ie/) is needed during the installation to replace the default usbaudio driver installed by Windows

## Limitations

* It is assumed that ADB interface is installed with a WINUSB-based driver; if your device requires a specific driver from the manufacturer to install ADB interface and it is not a WINUSB-based one, this programme is likely to fail to detect you device.
* It has been found that if ADB server is running, it will occupy the ADB interface and any further attempts to open it throught WINUSB API will fail. As a result, this programme incorporates a version of adb.exe and uses it to stop ADB server before enumerate available devices. That means all other applications (such as [scrcpy](https://github.com/Genymobile/scrcpy)) which depend on the services provided by ADB server will be affected during the start of this programme; but it is fine to run them **after** this programme has successfully put your device into accessory mode.
* The audio stream received from a device is redirected to the default audio device on your PC using Windows Audio Session API (WASAPI); however, the audio format designated by Android Open Accessory(AOA) Protocol 2.0 might not be supported by the audio device, and therefore conversion is required. This project is only equipped with few types of software conversion

   * PCM to PCM with a different channel width (1, 2, or 4 bytes)
   * PCM to IEEE_FLOAT
   * Downsampling
   * Upsampling
   
   Note that not all types of conversion have been fully verified, and upsampling is simply done via a naive way of sample replication, so the quality of audio is not guaranteed.

## Build Environment

* Visual Studio Community 2013, Update 4 \
Newer versions of VS could also be viable, but I have never tried them.
   
## Installation

1. Copy the folder "usbaudio/others" to the directory where the executable is placed.
1. Plug you device into your PC. Make sure "Developer options" is enabled.
1. Open "Device Manager", locate the ADB Interface of your device, and check the VID, PID, and MI of it in the "Detail" tab.
1. Open "Registry" (by executing regedit.exe), under "HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\USB" locate the entry which matches the VID, PID, and MI of the ADB Interface. Expand that entry and there will be a subfolder named "Device Parameters" containing a setting named "DeviceInterfaceGUIDs".
1. Replace the value of ADB_DeviceInterface_GUID in "others/DeviceInterface_GUID.config" with the value of "DeviceInterfaceGUIDs" found in Step 4. 
1. Run this programme for the first time. The purpose of this run is to put your device into accessory mode, and it is normal to see it fail and exit with an error message.
1. While your device is in accessory mode, open "Device Manager" and there will be a new audio controller device appearing under the category of "Sound, video, and game controllers" (possibly with a yellow exclamation). Use zadig (I used zadig-2.4) to replace the driver of it with a WINUSB one.
1. After the driver replacement, that device should be moved into the category of "Universal Serial Bus Devices" and work properly. Now repeat Step 3 ~ 4 for that device, and update the value of "Audio_DeviceInterface_GUID" in "others/DeviceInterface_GUID.config" with the value of "DeviceInterfaceGUIDs" found.
1. All set. Start this programme again and you should hear the sound of your device from your PC's speakers now.

   
