// This file is part of Background Music.
//
// Background Music is free software: you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 2 of the
// License, or (at your option) any later version.
//
// Background Music is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Background Music. If not, see <http://www.gnu.org/licenses/>.

//
//  BGM_Device.cpp
//  BGMDriver
//
//  Copyright © 2016, 2017 Kyle Neideck
//  Copyright © 2016 Josh Junon
//  Copyright © 2017 Andrew Tonner
//  Portions copyright (C) 2013 Apple Inc. All Rights Reserved.
//
//  Based largely on SA_Device.cpp from Apple's SimpleAudioDriver Plug-In sample code. Also uses a few sections from Apple's
//  NullAudio.c sample code (found in the same sample project).
//  https://developer.apple.com/library/mac/samplecode/AudioDriverExamples
//
//  TODO: This class is now almost 2500 lines long.
//

// Self Include
#include "BGM_Device.h"

// Local Includes
#include "BGM_PlugIn.h"
#include "BGM_XPCHelper.h"
#include "BGM_Utils.h"

// PublicUtility Includes
#include "CADispatchQueue.h"
#include "CAException.h"
#include "CACFArray.h"
#include "CACFString.h"
#include "CADebugMacros.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#include "CAAtomic.h"
#pragma clang diagnostic pop

// System Includes
#include <mach/mach_time.h>

// STL Includes
#include <algorithm>


#pragma mark Construction/Destruction

pthread_once_t				BGM_Device::sStaticInitializer = PTHREAD_ONCE_INIT;
BGM_Device*					BGM_Device::sInstance = nullptr;

BGM_Device&	BGM_Device::GetInstance()
{
    pthread_once(&sStaticInitializer, StaticInitializer);
    return *sInstance;
}

void	BGM_Device::StaticInitializer()
{
    try
    {
        sInstance = new BGM_Device;
        sInstance->Activate();
    }
    catch(...)
    {
        DebugMsg("BGM_Device::StaticInitializer: failed to create the device");
        delete sInstance;
        sInstance = nullptr;
    }
}

BGM_Device::BGM_Device()
:
	BGM_AbstractDevice(kObjectID_Device, kAudioObjectPlugInObject),
	mStateMutex("Device State"),
	mIOMutex("Device IO"),
	mSampleRateShadow(0),
    mWrappedAudioEngine(nullptr),
    mClients(&mTaskQueue),
    mInputStream(kObjectID_Stream_Input, kObjectID_Device, false, kSampleRateDefault),
    mOutputStream(kObjectID_Stream_Output, kObjectID_Device, false, kSampleRateDefault),
    mDeviceAudibleState(kBGMDeviceIsSilent),
    mAudibleStateSampleTimes({0, 0, 0, 0}),
	//mInputMasterVolumeControlRawValueShadow(kDefaultMinRawVolumeValue),
	mOutputMasterVolumeControlRawValueShadow(kDefaultMinRawVolumeValue),
    mOutputMasterMinRawVolumeShadow(kDefaultMinRawVolumeValue),
    mOutputMasterMaxRawVolumeShadow(kDefaultMaxRawVolumeValue),
    mOutputMasterMinDbVolumeShadow(kDefaultMinDbVolumeValue),
    mOutputMasterMaxDbVolumeShadow(kDefaultMaxDbVolumeValue),
    mOutputMuteValueShadow(0)
{
	// Setup the volume curve with the one range
    mVolumeCurve.AddRange(kDefaultMinRawVolumeValue, kDefaultMaxRawVolumeValue, kDefaultMinDbVolumeValue, kDefaultMaxDbVolumeValue);
    
    // Initialises the loopback clock with the default sample rate and, if there is one, sets the wrapped device to the same sample rate
    SetSampleRate(kSampleRateDefault);
}

BGM_Device::~BGM_Device()
{
}

void	BGM_Device::Activate()
{
	//	Open the connection to the driver and initialize things.
	//_HW_Open();
	
	//	Call the super-class, which just marks the object as active
	BGM_AbstractDevice::Activate();
}

void	BGM_Device::Deactivate()
{
	//	When this method is called, the object is basically dead, but we still need to be thread
	//	safe. In this case, we also need to be safe vs. any IO threads, so we need to take both
	//	locks.
	CAMutex::Locker theStateLocker(mStateMutex);
	CAMutex::Locker theIOLocker(mIOMutex);
	
	//	mark the object inactive by calling the super-class
	BGM_AbstractDevice::Deactivate();
	
	//	close the connection to the driver
	//_HW_Close();
}

void    BGM_Device::InitLoopback()
{
    //	Calculate the host ticks per frame for the loopback timer
    struct mach_timebase_info theTimeBaseInfo;
    mach_timebase_info(&theTimeBaseInfo);
    Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;
    theHostClockFrequency *= 1000000000.0;
    mLoopbackTime.hostTicksPerFrame = theHostClockFrequency / mLoopbackSampleRate;
    
    //  Zero-out the loopback buffer
    //  2 channels * 32-bit float = bytes in each frame
    memset(mLoopbackRingBuffer, 0, sizeof(Float32) * 2 * kLoopbackRingBufferFrameSize);
}

#pragma mark Property Operations

bool	BGM_Device::HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	This object implements several API-level objects. So the first thing to do is to figure out
	//	which object this request is really for. Note that mObjectID is an invariant as this
	//	driver's structure does not change dynamically. It will always have the parts it has.
	bool theAnswer = false;
	if(inObjectID == mObjectID)
	{
		theAnswer = Device_HasProperty(inObjectID, inClientPID, inAddress);
	}
    else if(IsStreamID(inObjectID))
	{
		theAnswer = GetStreamByID(inObjectID).HasProperty(inObjectID, inClientPID, inAddress);
	}
	else if(/*(inObjectID == mInputMasterVolumeControlObjectID) ||*/ (inObjectID == kObjectID_Volume_Output_Master) || (inObjectID == kObjectID_Mute_Output_Master))
	{
		theAnswer = Control_HasProperty(inObjectID, inClientPID, inAddress);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
	return theAnswer;
}

bool	BGM_Device::IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	bool theAnswer = false;
	if(inObjectID == mObjectID)
	{
		theAnswer = Device_IsPropertySettable(inObjectID, inClientPID, inAddress);
	}
	else if(IsStreamID(inObjectID))
	{
		theAnswer = GetStreamByID(inObjectID).IsPropertySettable(inObjectID, inClientPID, inAddress);
	}
	else if(/*(inObjectID == mInputMasterVolumeControlObjectID) ||*/ (inObjectID == kObjectID_Volume_Output_Master) || (inObjectID == kObjectID_Mute_Output_Master))
	{
		theAnswer = Control_IsPropertySettable(inObjectID, inClientPID, inAddress);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
	return theAnswer;
}

UInt32	BGM_Device::GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	UInt32 theAnswer = 0;
	if(inObjectID == mObjectID)
	{
		theAnswer = Device_GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
	}
	else if(IsStreamID(inObjectID))
	{
		theAnswer = GetStreamByID(inObjectID).GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
	}
	else if(/*(inObjectID == mInputMasterVolumeControlObjectID) ||*/ (inObjectID == kObjectID_Volume_Output_Master) || (inObjectID == kObjectID_Mute_Output_Master))
	{
		theAnswer = Control_GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
	return theAnswer;
}

void	BGM_Device::GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
    ThrowIfNULL(outData, BGM_RuntimeException(), "BGM_Device::GetPropertyData: !outData");
    
	if(inObjectID == mObjectID)
	{
		Device_GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
	}
	else if(IsStreamID(inObjectID))
	{
		GetStreamByID(inObjectID).GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
	}
	else if(/*(inObjectID == mInputMasterVolumeControlObjectID) ||*/ (inObjectID == kObjectID_Volume_Output_Master) || (inObjectID == kObjectID_Mute_Output_Master))
	{
		Control_GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
}

void	BGM_Device::SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
    ThrowIfNULL(inData, BGM_RuntimeException(), "BGM_Device::SetPropertyData: no data");
    
	if(inObjectID == mObjectID)
	{
		Device_SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
	}
	else if(IsStreamID(inObjectID))
    {
        // Forward stream properties.
        BGM_Stream& stream = (inObjectID == kObjectID_Stream_Input) ? mInputStream : mOutputStream;
        stream.SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);

        // When one of the stream's sample rate changes, set the new sample rate for both streams
        // and the device. The streams check the new format before this point but don't change until
        // the device tells them to, as it has to get the host to pause IO first.
        if(inAddress.mSelector == kAudioStreamPropertyVirtualFormat ||
           inAddress.mSelector == kAudioStreamPropertyPhysicalFormat)
        {
            const AudioStreamBasicDescription* theNewFormat =
                reinterpret_cast<const AudioStreamBasicDescription*>(inData);
            RequestSampleRate(theNewFormat->mSampleRate);
        }
	}
	else if(/*(inObjectID == mInputMasterVolumeControlObjectID) ||*/ (inObjectID == kObjectID_Volume_Output_Master) || (inObjectID == kObjectID_Mute_Output_Master))
	{
		Control_SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
}

#pragma mark Device Property Operations

bool	BGM_Device::Device_HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.
	
	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyIcon:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioDeviceCustomPropertyDeviceAudibleState:
        case kAudioDeviceCustomPropertyMusicPlayerProcessID:
        case kAudioDeviceCustomPropertyMusicPlayerBundleID:
        case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp:
        case kAudioDeviceCustomPropertyAppVolumes:
        case kAudioDeviceCustomPropertyEnabledOutputControls:
			theAnswer = true;
			break;
			
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyPreferredChannelsForStereo:
		case kAudioDevicePropertyPreferredChannelLayout:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			theAnswer = (inAddress.mScope == kAudioObjectPropertyScopeInput) || (inAddress.mScope == kAudioObjectPropertyScopeOutput);
			break;
			
		default:
			theAnswer = BGM_AbstractDevice::HasProperty(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

bool	BGM_Device::Device_IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.
	
	bool theAnswer = false;
	switch(inAddress.mSelector)
    {
		case kAudioDevicePropertyStreams:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyIcon:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioDeviceCustomPropertyDeviceAudibleState:
        case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp:
			theAnswer = false;
			break;
            
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDeviceCustomPropertyMusicPlayerProcessID:
        case kAudioDeviceCustomPropertyMusicPlayerBundleID:
        case kAudioDeviceCustomPropertyAppVolumes:
        case kAudioDeviceCustomPropertyEnabledOutputControls:
			theAnswer = true;
			break;
		
		default:
			theAnswer = BGM_AbstractDevice::IsPropertySettable(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

UInt32	BGM_Device::Device_GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.
	
	UInt32 theAnswer = 0;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyOwnedObjects:
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					theAnswer = kNumberOfSubObjects * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeInput:
					theAnswer = kNumberOfInputSubObjects * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeOutput:
					theAnswer = kNumberOfOutputSubObjects * sizeof(AudioObjectID);
					break;
			};
			break;

        case kAudioDevicePropertyStreams:
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					theAnswer = kNumberOfStreams * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeInput:
					theAnswer = kNumberOfInputStreams * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeOutput:
					theAnswer = kNumberOfOutputStreams * sizeof(AudioObjectID);
					break;
			};
			break;

        case kAudioObjectPropertyControlList:
            theAnswer = (mOutputVolumeControlEnabled ? 1 : 0) * sizeof(AudioObjectID);
            theAnswer += (mOutputMuteControlEnabled ? 1 : 0) * sizeof(AudioObjectID);
            break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			theAnswer = 1 * sizeof(AudioValueRange);
			break;

		case kAudioDevicePropertyPreferredChannelsForStereo:
			theAnswer = 2 * sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			theAnswer = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
			break;

        case kAudioDevicePropertyIcon:
            theAnswer = sizeof(CFURLRef);
            break;
            
        case kAudioObjectPropertyCustomPropertyInfoList:
            theAnswer = sizeof(AudioServerPlugInCustomPropertyInfo) * 6;
            break;
            
        case kAudioDeviceCustomPropertyDeviceAudibleState:
            theAnswer = sizeof(CFNumberRef);
            break;

        case kAudioDeviceCustomPropertyMusicPlayerProcessID:
            theAnswer = sizeof(CFPropertyListRef);
			break;
            
        case kAudioDeviceCustomPropertyMusicPlayerBundleID:
            theAnswer = sizeof(CFStringRef);
            break;
            
        case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp:
            theAnswer = sizeof(CFBooleanRef);
            break;
            
        case kAudioDeviceCustomPropertyAppVolumes:
            theAnswer = sizeof(CFPropertyListRef);
            break;

        case kAudioDeviceCustomPropertyEnabledOutputControls:
            theAnswer = sizeof(CFArrayRef);
            break;
		
		default:
			theAnswer = BGM_AbstractDevice::GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
			break;
	};
	return theAnswer;
}

void	BGM_Device::Device_GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required.
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.

	UInt32 theNumberItemsToFetch;
	UInt32 theItemIndex;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyName:
			//	This is the human readable name of the device. Note that in this case we return a
			//	value that is a key into the localizable strings in this bundle. This allows us to
			//	return a localized name for the device.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyName for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR(kDeviceName);
			outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyManufacturer:
			//	This is the human readable name of the maker of the plug-in. Note that in this case
			//	we return a value that is a key into the localizable strings in this bundle. This
			//	allows us to return a localized name for the manufacturer.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR(kDeviceManufacturerName);
			outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyOwnedObjects:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			
			//	The device owns its streams and controls. Note that what is returned here
			//	depends on the scope requested.
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					//	global scope means return all objects
					if(theNumberItemsToFetch > kNumberOfSubObjects)
					{
						theNumberItemsToFetch = kNumberOfSubObjects;
					}
					
					//	fill out the list with as many objects as requested, which is everything
					if(theNumberItemsToFetch > 0)
					{
						reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Stream_Input;
					}
					if(theNumberItemsToFetch > 1)
					{
						reinterpret_cast<AudioObjectID*>(outData)[1] = kObjectID_Stream_Output;
					}
                    /*
					if(theNumberItemsToFetch > 2)
					{
						reinterpret_cast<AudioObjectID*>(outData)[2] = mInputMasterVolumeControlObjectID;
					}
                     */
					if(theNumberItemsToFetch > 2)
					{
						reinterpret_cast<AudioObjectID*>(outData)[2] = kObjectID_Volume_Output_Master;
                    }
                    if(theNumberItemsToFetch > 3)
                    {
                        reinterpret_cast<AudioObjectID*>(outData)[3] = kObjectID_Mute_Output_Master;
                    }
					break;
					
				case kAudioObjectPropertyScopeInput:
					//	input scope means just the objects on the input side
					if(theNumberItemsToFetch > kNumberOfInputSubObjects)
					{
						theNumberItemsToFetch = kNumberOfInputSubObjects;
					}
					
					//	fill out the list with the right objects
					if(theNumberItemsToFetch > 0)
					{
						reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Stream_Input;
					}
                    /*
					if(theNumberItemsToFetch > 1)
					{
						reinterpret_cast<AudioObjectID*>(outData)[1] = mInputMasterVolumeControlObjectID;
					}
                     */
					break;
					
				case kAudioObjectPropertyScopeOutput:
					//	output scope means just the objects on the output side
					if(theNumberItemsToFetch > kNumberOfOutputSubObjects)
					{
						theNumberItemsToFetch = kNumberOfOutputSubObjects;
					}
					
					//	fill out the list with the right objects
					if(theNumberItemsToFetch > 0)
					{
						reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Stream_Output;
					}
					if(theNumberItemsToFetch > 1)
					{
						reinterpret_cast<AudioObjectID*>(outData)[1] = kObjectID_Volume_Output_Master;
                    }
                    if(theNumberItemsToFetch > 2)
                    {
                        reinterpret_cast<AudioObjectID*>(outData)[2] = kObjectID_Mute_Output_Master;
                    }
					break;
			};
			
			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyDeviceUID:
			//	This is a CFString that is a persistent token that can identify the same
			//	audio device across boot sessions. Note that two instances of the same
			//	device must have different values for this property.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR(kBGMDeviceUID);
			outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			//	This is a CFString that is a persistent token that can identify audio
			//	devices that are the same kind of device. Note that two instances of the
			//	save device must have the same value for this property.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR(kBGMDeviceModelUID);
			outDataSize = sizeof(CFStringRef);
			break;
            
		case kAudioDevicePropertyDeviceIsRunning:
			//	This property returns whether or not IO is running for the device.
            ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device");
            *reinterpret_cast<UInt32*>(outData) = mClients.ClientsRunningIO() ? 1 : 0;
            outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			
			//	Note that what is returned here depends on the scope requested.
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					//	global scope means return all streams
					if(theNumberItemsToFetch > kNumberOfStreams)
					{
						theNumberItemsToFetch = kNumberOfStreams;
					}
					
					//	fill out the list with as many objects as requested
					if(theNumberItemsToFetch > 0)
					{
						reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Stream_Input;
					}
					if(theNumberItemsToFetch > 1)
					{
						reinterpret_cast<AudioObjectID*>(outData)[1] = kObjectID_Stream_Output;
					}
					break;
					
				case kAudioObjectPropertyScopeInput:
					//	input scope means just the objects on the input side
					if(theNumberItemsToFetch > kNumberOfInputStreams)
					{
						theNumberItemsToFetch = kNumberOfInputStreams;
					}
					
					//	fill out the list with as many objects as requested
					if(theNumberItemsToFetch > 0)
					{
						reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Stream_Input;
					}
					break;
					
				case kAudioObjectPropertyScopeOutput:
					//	output scope means just the objects on the output side
					if(theNumberItemsToFetch > kNumberOfOutputStreams)
					{
						theNumberItemsToFetch = kNumberOfOutputStreams;
					}
					
					//	fill out the list with as many objects as requested
					if(theNumberItemsToFetch > 0)
					{
						reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Stream_Output;
					}
					break;
			};
			
			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyControlList:
            {
                //	Calculate the number of items that have been requested. Note that this
                //	number is allowed to be smaller than the actual size of the list, in which
                //	case only that many items will be returned.
                theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
                if(theNumberItemsToFetch > 2)
                {
                    theNumberItemsToFetch = 2;
                }

                UInt32 theNumberOfItemsFetched = 0;

                CAMutex::Locker theStateLocker(mStateMutex);
                
                //	fill out the list with as many objects as requested
                if(theNumberItemsToFetch > 0)
                {
                    if(mOutputVolumeControlEnabled)
                    {
                        reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Volume_Output_Master;
                        theNumberOfItemsFetched++;
                    }
                    else if (mOutputMuteControlEnabled)
                    {
                        reinterpret_cast<AudioObjectID*>(outData)[0] = kObjectID_Mute_Output_Master;
                        theNumberOfItemsFetched++;
                    }
                }

                if(theNumberItemsToFetch > 1 && mOutputVolumeControlEnabled && mOutputMuteControlEnabled)
                {
                    reinterpret_cast<AudioObjectID*>(outData)[1] = kObjectID_Mute_Output_Master;
                    theNumberOfItemsFetched++;
                }
                
                //	report how much we wrote
                outDataSize = theNumberOfItemsFetched * sizeof(AudioObjectID);
            }
			break;

        // TODO: Should we return the real kAudioDevicePropertyLatency and/or
        //       kAudioDevicePropertySafetyOffset for the real/wrapped output device?
        //       If so, should we also add on the extra latency added by Background Music? 

		case kAudioDevicePropertyNominalSampleRate:
			//	This property returns the nominal sample rate of the device.
            ThrowIf(inDataSize < sizeof(Float64),
                    CAException(kAudioHardwareBadPropertySizeError),
                    "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device");

            *reinterpret_cast<Float64*>(outData) = GetSampleRate();
            outDataSize = sizeof(Float64);
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			//	This returns all nominal sample rates the device supports as an array of
			//	AudioValueRangeStructs. Note that for discrete sampler rates, the range
			//	will have the minimum value equal to the maximum value.
            //
            //  BGMDevice supports any sample rate so it can be set to match the output
            //  device when in loopback mode.
			
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);
			
			//	clamp it to the number of items we have
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}
			
			//	fill out the return array
			if(theNumberItemsToFetch > 0)
			{
                // 0 would cause divide-by-zero errors in other BGM_Device functions (and
                // wouldn't make sense anyway).
                ((AudioValueRange*)outData)[0].mMinimum = 1.0;
                // Just in case DBL_MAX would cause problems in a client for some reason,
                // use an arbitrary very large number instead. (It wouldn't make sense to
                // actually set the sample rate this high, but I don't know what a
                // reasonable maximum would be.)
                ((AudioValueRange*)outData)[0].mMaximum = 1000000000.0;
			}
			
			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
			break;

		case kAudioDevicePropertyPreferredChannelsForStereo:
			//	This property returns which two channels to use as left/right for stereo
			//	data by default. Note that the channel numbers are 1-based.
			ThrowIf(inDataSize < (2 * sizeof(UInt32)), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelsForStereo for the device");
			((UInt32*)outData)[0] = 1;
			((UInt32*)outData)[1] = 2;
			outDataSize = 2 * sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			//	This property returns the default AudioChannelLayout to use for the device
			//	by default. For this device, we return a stereo ACL.
			{
				UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
				ThrowIf(inDataSize < theACLSize, CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device");
				((AudioChannelLayout*)outData)->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
				((AudioChannelLayout*)outData)->mChannelBitmap = 0;
				((AudioChannelLayout*)outData)->mNumberChannelDescriptions = 2;
				for(theItemIndex = 0; theItemIndex < 2; ++theItemIndex)
				{
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelLabel = kAudioChannelLabel_Left + theItemIndex;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelFlags = 0;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[0] = 0;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[1] = 0;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[2] = 0;
				}
				outDataSize = theACLSize;
			}
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			//	This property returns how many frames the HAL should expect to see between
			//	successive sample times in the zero time stamps this device provides.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device");
			*reinterpret_cast<UInt32*>(outData) = kLoopbackRingBufferFrameSize;
			outDataSize = sizeof(UInt32);
            break;
            
        case kAudioDevicePropertyIcon:
            {
                // This property is a CFURL that points to the device's icon in the plugin's resource bundle
                ThrowIf(inDataSize < sizeof(CFURLRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyIcon for the device");
                
                CFBundleRef theBundle = CFBundleGetBundleWithIdentifier(BGM_PlugIn::GetInstance().GetBundleID());
                ThrowIf(theBundle == NULL, CAException(kAudioHardwareUnspecifiedError), "BGM_Device::Device_GetPropertyData: could not get the plugin bundle for kAudioDevicePropertyIcon");
                
                CFURLRef theURL = CFBundleCopyResourceURL(theBundle, CFSTR("DeviceIcon.icns"), NULL, NULL);
                ThrowIf(theURL == NULL, CAException(kAudioHardwareUnspecifiedError), "BGM_Device::Device_GetPropertyData: could not get the URL for kAudioDevicePropertyIcon");
                
                *reinterpret_cast<CFURLRef*>(outData) = theURL;
                outDataSize = sizeof(CFURLRef);
            }
            break;
            
        case kAudioObjectPropertyCustomPropertyInfoList:
            theNumberItemsToFetch = inDataSize / sizeof(AudioServerPlugInCustomPropertyInfo);
            
            //	clamp it to the number of items we have
            if(theNumberItemsToFetch > 6)
            {
                theNumberItemsToFetch = 6;
            }
            
            if(theNumberItemsToFetch > 0)
            {
                ((AudioServerPlugInCustomPropertyInfo*)outData)[0].mSelector = kAudioDeviceCustomPropertyAppVolumes;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[0].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[0].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }
            if(theNumberItemsToFetch > 1)
            {
                ((AudioServerPlugInCustomPropertyInfo*)outData)[1].mSelector = kAudioDeviceCustomPropertyMusicPlayerProcessID;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[1].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[1].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }
            if(theNumberItemsToFetch > 2)
            {
                ((AudioServerPlugInCustomPropertyInfo*)outData)[2].mSelector = kAudioDeviceCustomPropertyMusicPlayerBundleID;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[2].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[2].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }
            if(theNumberItemsToFetch > 3)
            {
                ((AudioServerPlugInCustomPropertyInfo*)outData)[3].mSelector = kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[3].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[3].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }
            if(theNumberItemsToFetch > 4)
            {
                ((AudioServerPlugInCustomPropertyInfo*)outData)[4].mSelector = kAudioDeviceCustomPropertyDeviceAudibleState;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[4].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[4].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }
            if(theNumberItemsToFetch > 5)
            {
                ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mSelector = kAudioDeviceCustomPropertyEnabledOutputControls;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }

            outDataSize = theNumberItemsToFetch * sizeof(AudioServerPlugInCustomPropertyInfo);
            break;
            
        case kAudioDeviceCustomPropertyDeviceAudibleState:
            {
                ThrowIf(inDataSize < sizeof(CFNumberRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyDeviceAudibleState for the device");
                
                // mDeviceAudibleState is accessed without locking to avoid priority inversions on the IO threads. (The memory barrier
                // is probably unnecessary.)
                CAMemoryBarrier();
                *reinterpret_cast<CFNumberRef*>(outData) = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &mDeviceAudibleState);
                outDataSize = sizeof(CFNumberRef);
            }
            break;
            
        case kAudioDeviceCustomPropertyMusicPlayerProcessID:
            {
                ThrowIf(inDataSize < sizeof(CFNumberRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyMusicPlayerProcessID for the device");
                CAMutex::Locker theStateLocker(mStateMutex);
                pid_t pid = mClients.GetMusicPlayerProcessIDProperty();
                *reinterpret_cast<CFNumberRef*>(outData) = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pid);
                outDataSize = sizeof(CFNumberRef);
            }
            break;
            
        case kAudioDeviceCustomPropertyMusicPlayerBundleID:
            {
                ThrowIf(inDataSize < sizeof(CFStringRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyMusicPlayerBundleID for the device");
                CAMutex::Locker theStateLocker(mStateMutex);
                *reinterpret_cast<CFStringRef*>(outData) = mClients.CopyMusicPlayerBundleIDProperty();
                outDataSize = sizeof(CFStringRef);
            }
            break;
            
        case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp:
            ThrowIf(inDataSize < sizeof(CFBooleanRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp for the device");
            *reinterpret_cast<CFBooleanRef*>(outData) = mClients.ClientsOtherThanBGMAppRunningIO() ? kCFBooleanTrue : kCFBooleanFalse;
            outDataSize = sizeof(CFBooleanRef);
            break;
            
        case kAudioDeviceCustomPropertyAppVolumes:
            {
                ThrowIf(inDataSize < sizeof(CFArrayRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyAppVolumes for the device");
                CAMutex::Locker theStateLocker(mStateMutex);
                *reinterpret_cast<CFArrayRef*>(outData) = mClients.CopyClientRelativeVolumesAsAppVolumes().GetCFArray();
                outDataSize = sizeof(CFArrayRef);
            }
            break;

        case kAudioDeviceCustomPropertyEnabledOutputControls:
            {
                ThrowIf(inDataSize < sizeof(CFArrayRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyEnabledOutputControls for the device");
                CACFArray theEnabledControls(2, true);

				{
					CAMutex::Locker theStateLocker(mStateMutex);
					theEnabledControls.AppendCFType(mOutputVolumeControlEnabled ? kCFBooleanTrue : kCFBooleanFalse);
					theEnabledControls.AppendCFType(mOutputMuteControlEnabled ? kCFBooleanTrue : kCFBooleanFalse);
				}

                *reinterpret_cast<CFArrayRef*>(outData) = theEnabledControls.CopyCFArray();
                outDataSize = sizeof(CFArrayRef);
            }
            break;

		default:
			BGM_AbstractDevice::GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
	};
}

void	BGM_Device::Device_SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	switch(inAddress.mSelector)
	{
        case kAudioDevicePropertyNominalSampleRate:
            ThrowIf(inDataSize < sizeof(Float64),
                    CAException(kAudioHardwareBadPropertySizeError),
                    "BGM_Device::Device_SetPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
            RequestSampleRate(*reinterpret_cast<const Float64*>(inData));
            break;
            
        case kAudioDeviceCustomPropertyMusicPlayerProcessID:
            {
                ThrowIf(inDataSize < sizeof(CFNumberRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyMusicPlayerProcessID");
                
                CFNumberRef pidRef = *reinterpret_cast<const CFNumberRef*>(inData);
                
                ThrowIf(pidRef == NULL, CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: null reference given for kAudioDeviceCustomPropertyMusicPlayerProcessID");
                ThrowIf(CFGetTypeID(pidRef) != CFNumberGetTypeID(), CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: CFType given for kAudioDeviceCustomPropertyMusicPlayerProcessID was not a CFNumber");
                
                // Get the pid out of the CFNumber we received
                // (Not using CACFNumber::GetSInt32 here because it would return 0 if CFNumberGetValue didn't write to our
                // pid variable, and we want that to be an error.)
                pid_t pid = INT_MIN;
                // CFNumberGetValue docs: "If the conversion is lossy, or the value is out of range, false is returned."
                Boolean success = CFNumberGetValue(pidRef, kCFNumberSInt32Type, &pid);
                
                ThrowIf(!success, CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: probable error from CFNumberGetValue when reading pid for kAudioDeviceCustomPropertyMusicPlayerProcessID");
                
                CAMutex::Locker theStateLocker(mStateMutex);
                
                bool propertyWasChanged = false;
                
                try
                {
                    propertyWasChanged = mClients.SetMusicPlayer(pid);
                }
                catch(BGM_InvalidClientPIDException)
                {
                    Throw(CAException(kAudioHardwareIllegalOperationError));
                }
                
                if(propertyWasChanged)
                {
                    // Send notification
                    CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
                        AudioObjectPropertyAddress theChangedProperties[] = { kBGMMusicPlayerProcessIDAddress, kBGMMusicPlayerBundleIDAddress };
                        BGM_PlugIn::Host_PropertiesChanged(inObjectID, 2, theChangedProperties);
                    });
                }
            }
            break;
            
        case kAudioDeviceCustomPropertyMusicPlayerBundleID:
            {
                ThrowIf(inDataSize < sizeof(CFStringRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyMusicPlayerBundleID");
            
                CFStringRef theBundleIDRef = *reinterpret_cast<const CFStringRef*>(inData);
                
                ThrowIfNULL(theBundleIDRef, CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: kAudioDeviceCustomPropertyMusicPlayerBundleID cannot be set to NULL");
                ThrowIf(CFGetTypeID(theBundleIDRef) != CFStringGetTypeID(), CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: CFType given for kAudioDeviceCustomPropertyMusicPlayerBundleID was not a CFString");
                
                CAMutex::Locker theStateLocker(mStateMutex);
                
                CFRetain(theBundleIDRef);
                CACFString bundleID(theBundleIDRef);
                
                bool propertyWasChanged = mClients.SetMusicPlayer(bundleID);
                
                if(propertyWasChanged)
                {
                    // Send notification
                    CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
                        AudioObjectPropertyAddress theChangedProperties[] = { kBGMMusicPlayerBundleIDAddress, kBGMMusicPlayerProcessIDAddress };
                        BGM_PlugIn::Host_PropertiesChanged(inObjectID, 2, theChangedProperties);
                    });
                }
            }
            break;
            
        case kAudioDeviceCustomPropertyAppVolumes:
            {
                ThrowIf(inDataSize < sizeof(CFArrayRef), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyAppVolumes");
                
                CFArrayRef arrayRef = *reinterpret_cast<const CFArrayRef*>(inData);

                ThrowIfNULL(arrayRef, CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: kAudioDeviceCustomPropertyAppVolumes cannot be set to NULL");
                ThrowIf(CFGetTypeID(arrayRef) != CFArrayGetTypeID(), CAException(kAudioHardwareIllegalOperationError), "BGM_Device::Device_SetPropertyData: CFType given for kAudioDeviceCustomPropertyAppVolumes was not a CFArray");
                
                CACFArray array(arrayRef, false);

                bool propertyWasChanged = false;

				CAMutex::Locker theStateLocker(mStateMutex);

				try
                {
                    propertyWasChanged = mClients.SetClientsRelativeVolumes(array);
                }
                catch(BGM_InvalidClientRelativeVolumeException)
                {
                    Throw(CAException(kAudioHardwareIllegalOperationError));
                }
                
                if(propertyWasChanged)
                {
                    // Send notification
                    CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
                        AudioObjectPropertyAddress theChangedProperties[] = { kBGMAppVolumesAddress };
                        BGM_PlugIn::Host_PropertiesChanged(inObjectID, 1, theChangedProperties);
                    });
                }
            }
            break;

        case kAudioDeviceCustomPropertyEnabledOutputControls:
            {
                ThrowIf(inDataSize < sizeof(CFArrayRef),
                        CAException(kAudioHardwareBadPropertySizeError),
                        "BGM_Device::Device_SetPropertyData: wrong size for the data for "
                        "kAudioDeviceCustomPropertyEnabledOutputControls");

                CFArrayRef theEnabledControlsRef = *reinterpret_cast<const CFArrayRef*>(inData);

                ThrowIfNULL(theEnabledControlsRef,
                            CAException(kAudioHardwareIllegalOperationError),
                            "BGM_Device::Device_SetPropertyData: null reference given for "
                            "kAudioDeviceCustomPropertyEnabledOutputControls");
                ThrowIf(CFGetTypeID(theEnabledControlsRef) != CFArrayGetTypeID(),
                        CAException(kAudioHardwareIllegalOperationError),
                        "BGM_Device::Device_SetPropertyData: CFType given for "
                        "kAudioDeviceCustomPropertyEnabledOutputControls was not a CFArray");

                CACFArray theEnabledControls(theEnabledControlsRef, true);

                ThrowIf(theEnabledControls.GetNumberItems() != 2,
                        CAException(kAudioHardwareIllegalOperationError),
                        "BGM_Device::Device_SetPropertyData: Expected the CFArray given for "
                        "kAudioDeviceCustomPropertyEnabledOutputControls to have exactly 2 elements");

                bool theVolumeControlEnabled;
                bool didGetBool = theEnabledControls.GetBool(kBGMEnabledOutputControlsIndex_Volume,
															 theVolumeControlEnabled);
                ThrowIf(!didGetBool,
                        CAException(kAudioHardwareIllegalOperationError),
                        "BGM_Device::Device_SetPropertyData: Expected CFBoolean for volume elem of "
                        "kAudioDeviceCustomPropertyEnabledOutputControls");

                bool theMuteControlEnabled;
                didGetBool = theEnabledControls.GetBool(kBGMEnabledOutputControlsIndex_Mute,
														theMuteControlEnabled);
                ThrowIf(!didGetBool,
                        CAException(kAudioHardwareIllegalOperationError),
                        "BGM_Device::Device_SetPropertyData: Expected CFBoolean for mute elem of "
                        "kAudioDeviceCustomPropertyEnabledOutputControls");

                RequestEnabledControls(theVolumeControlEnabled, theMuteControlEnabled);
            }
            break;

		default:
			BGM_AbstractDevice::SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
			break;
    };
}

#pragma mark Control Property Operations

bool	BGM_Device::Control_HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Control_GetPropertyData() method.
	
	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioLevelControlPropertyScalarValue:
		case kAudioLevelControlPropertyDecibelValue:
		case kAudioLevelControlPropertyDecibelRange:
		case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
        case kAudioBooleanControlPropertyValue:
			theAnswer = true;
			break;
		
		default:
			theAnswer = BGM_Object::HasProperty(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

bool	BGM_Device::Control_IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Control_GetPropertyData() method.
	
	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioLevelControlPropertyDecibelRange:
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			theAnswer = false;
			break;
		
		case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioBooleanControlPropertyValue:
			theAnswer = true;
			break;
		
		default:
			theAnswer = BGM_Object::IsPropertySettable(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

UInt32	BGM_Device::Control_GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Control_GetPropertyData() method.
	
	UInt32 theAnswer = 0;
	switch(inAddress.mSelector)
	{
		case kAudioControlPropertyScope:
			theAnswer = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:
			theAnswer = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioLevelControlPropertyScalarValue:
			theAnswer = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyDecibelValue:
			theAnswer = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyDecibelRange:
			theAnswer = sizeof(AudioValueRange);
			break;

		case kAudioLevelControlPropertyConvertScalarToDecibels:
			theAnswer = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			theAnswer = sizeof(Float32);
            break;
            
        case kAudioBooleanControlPropertyValue:
            theAnswer = sizeof(UInt32);
            break;

		default:
			theAnswer = BGM_Object::GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
			break;
	};
	return theAnswer;
}

void	BGM_Device::Control_GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required.
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	
	SInt32 theControlRawValue;
	Float32 theVolumeValue;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base classes of kAudioVolumeControlClassID and kAudioMuteControlClassID are kAudioLevelControlClassID and kAudioBooleanControlClassID, respectively
			ThrowIf(inDataSize < sizeof(AudioClassID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the volume/mute control");
            *reinterpret_cast<AudioClassID*>(outData) =
                (inObjectID == kObjectID_Mute_Output_Master) ? kAudioBooleanControlClassID : kAudioLevelControlClassID;
			outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyClass:
			//	Volume controls are of the class kAudioVolumeControlClassID. Mute controls are of the class kAudioMuteControlClassID.
			ThrowIf(inDataSize < sizeof(AudioClassID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the volume/mute control");
            *reinterpret_cast<AudioClassID*>(outData) =
                (inObjectID == kObjectID_Mute_Output_Master) ? kAudioMuteControlClassID : kAudioVolumeControlClassID;
			outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyOwner:
			//	The control's owner is the device object
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the volume/mute control");
            *reinterpret_cast<AudioObjectID*>(outData) = GetObjectID();
			outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioControlPropertyScope:
			//	This property returns the scope that the control is attached to.
			ThrowIf(inDataSize < sizeof(AudioObjectPropertyScope), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioControlPropertyScope for the volume/mute control");
			*reinterpret_cast<AudioObjectPropertyScope*>(outData) =
                /*(inObjectID == mInputMasterVolumeControlObjectID) ? kAudioObjectPropertyScopeInput :*/ kAudioObjectPropertyScopeOutput;
			outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:
			//	This property returns the element that the control is attached to.
			ThrowIf(inDataSize < sizeof(AudioObjectPropertyElement), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioControlPropertyElement for the volume/mute control");
			*reinterpret_cast<AudioObjectPropertyElement*>(outData) = kAudioObjectPropertyElementMaster;
			outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioLevelControlPropertyScalarValue:
			//	This returns the value of the control in the normalized range of 0 to 1.
			{
				ThrowIf(inDataSize < sizeof(Float32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioLevelControlPropertyScalarValue for the volume control");
				CAMutex::Locker theStateLocker(mStateMutex);
				theControlRawValue = _HW_GetVolumeControlValue(inObjectID);
				*reinterpret_cast<Float32*>(outData) = mVolumeCurve.ConvertRawToScalar(theControlRawValue);
				outDataSize = sizeof(Float32);
			}
			break;

		case kAudioLevelControlPropertyDecibelValue:
			//	This returns the dB value of the control.
			{
				ThrowIf(inDataSize < sizeof(Float32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
				CAMutex::Locker theStateLocker(mStateMutex);
				theControlRawValue = _HW_GetVolumeControlValue(inObjectID);
				*reinterpret_cast<Float32*>(outData) = mVolumeCurve.ConvertRawToDB(theControlRawValue);
				outDataSize = sizeof(Float32);
			}
			break;

		case kAudioLevelControlPropertyDecibelRange:
			//	This returns the dB range of the control.
			ThrowIf(inDataSize < sizeof(AudioValueRange), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelRange for the volume control");
			reinterpret_cast<AudioValueRange*>(outData)->mMinimum = mVolumeCurve.GetMinimumDB();
			reinterpret_cast<AudioValueRange*>(outData)->mMaximum = mVolumeCurve.GetMaximumDB();
			outDataSize = sizeof(AudioValueRange);
			break;

		case kAudioLevelControlPropertyConvertScalarToDecibels:
			//	This takes the scalar value in outData and converts it to dB.
			ThrowIf(inDataSize < sizeof(Float32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
			
			//	clamp the value to be between 0 and 1
			theVolumeValue = *reinterpret_cast<Float32*>(outData);
			theVolumeValue = std::min(1.0f, std::max(0.0f, theVolumeValue));
			
			//	do the conversion
			*reinterpret_cast<Float32*>(outData) = mVolumeCurve.ConvertScalarToDB(theVolumeValue);
			
			//	report how much we wrote
			outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			//	This takes the dB value in outData and converts it to scalar.
			ThrowIf(inDataSize < sizeof(Float32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
			
			//	clamp the value to be between mOutputMasterMinDbVolumeShadow and mOutputMasterMaxDbVolumeShadow
			theVolumeValue = *reinterpret_cast<Float32*>(outData);
			theVolumeValue = std::min(mOutputMasterMaxDbVolumeShadow, std::max(mOutputMasterMinDbVolumeShadow, theVolumeValue));
			
			//	do the conversion
			*reinterpret_cast<Float32*>(outData) = mVolumeCurve.ConvertDBToScalar(theVolumeValue);
			
			//	report how much we wrote
			outDataSize = sizeof(Float32);
            break;
            
        case kAudioBooleanControlPropertyValue:
            //	This returns the mute value of the control.
            {
                ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_GetPropertyData: not enough space for the return value of kAudioBooleanControlPropertyValue for the mute control");
                CAMutex::Locker theStateLocker(mStateMutex);
                *reinterpret_cast<UInt32*>(outData) = _HW_GetMuteControlValue(inObjectID);
                outDataSize = sizeof(UInt32);
            }
            break;

		default:
			BGM_Object::GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
	};
}

void	BGM_Device::Control_SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Control_GetPropertyData() method.
	
    bool sendMuteNotification = false;
    bool sendVolumeNotification = false;
	kern_return_t theError = 0;
	Float32 theNewVolumeValue;
	SInt32 theNewRawVolumeValue;
	switch(inAddress.mSelector)
	{
		case kAudioLevelControlPropertyScalarValue:
			//	For the scalar volume, we clamp the new value to [0, 1]. Note that if this
			//	value changes, it implies that the dB value changed too.
			{
				ThrowIf(inDataSize != sizeof(Float32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_SetPropertyData: wrong size for the data for kAudioLevelControlPropertyScalarValue");
				theNewVolumeValue = *((const Float32*)inData);
				theNewVolumeValue = std::min(1.0f, std::max(0.0f, theNewVolumeValue));
				theNewRawVolumeValue = mVolumeCurve.ConvertScalarToRaw(theNewVolumeValue);
				CAMutex::Locker theStateLocker(mStateMutex);
				theError = _HW_SetVolumeControlValue(inObjectID, theNewRawVolumeValue);
                sendVolumeNotification = theError == 0;
			}
			break;
		
		case kAudioLevelControlPropertyDecibelValue:
			//	For the dB value, we first convert it to a scalar value since that is how
			//	the value is tracked. Note that if this value changes, it implies that the
			//	scalar value changes as well.
			{
				ThrowIf(inDataSize != sizeof(Float32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_SetPropertyData: wrong size for the data for kAudioLevelControlPropertyScalarValue");
				theNewVolumeValue = *((const Float32*)inData);
				theNewVolumeValue = std::min(mOutputMasterMaxDbVolumeShadow, std::max(mOutputMasterMinDbVolumeShadow, theNewVolumeValue));
				theNewRawVolumeValue = mVolumeCurve.ConvertDBToRaw(theNewVolumeValue);
				CAMutex::Locker theStateLocker(mStateMutex);
				theError = _HW_SetVolumeControlValue(inObjectID, theNewRawVolumeValue);
                sendVolumeNotification = theError == 0;
            }
            break;
            
        case kAudioBooleanControlPropertyValue:
            {
                ThrowIf(inDataSize != sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "BGM_Device::Control_SetPropertyData: wrong size for the data for kAudioBooleanControlPropertyValue");
                ThrowIf(inObjectID != kObjectID_Mute_Output_Master, CAException(kAudioHardwareBadObjectError), "BGM_Device::Control_SetPropertyData: unexpected control object id for kAudioBooleanControlPropertyValue");
                CAMutex::Locker theStateLocker(mStateMutex);
                theError = _HW_SetMuteControlValue(inObjectID, *((const UInt32*)inData));
                sendMuteNotification = theError == 0;
            }
            break;
		
		default:
			BGM_Object::SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
			break;
    };
    
	if(sendMuteNotification || sendVolumeNotification)
	{
		CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
            AudioObjectPropertyAddress theChangedProperties[2];
            if(sendMuteNotification)
            {
                theChangedProperties[0] = { kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
            }
            if(sendVolumeNotification)
            {
                theChangedProperties[0] = { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
                theChangedProperties[1] = { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
            }
            BGM_PlugIn::Host_PropertiesChanged(inObjectID, sendVolumeNotification ? 2 : 1, theChangedProperties);
        });
	}
}

#pragma mark IO Operations

void	BGM_Device::StartIO(UInt32 inClientID)
{
    bool clientIsBGMApp, bgmAppHasClientRegistered;
    
    {
        CAMutex::Locker theStateLocker(mStateMutex);
        
        // An overview of the process this function is part of:
        //   - A client starts IO.
        //   - The plugin host (the HAL) calls the StartIO function in BGM_PlugInInterface, which calls this function.
        //   - BGMDriver sends a message to BGMApp telling it to start the (real) audio hardware.
        //   - BGMApp starts the hardware and, after the hardware is ready, replies to BGMDriver's message.
        //   - BGMDriver lets the host know that it's ready to do IO by returning from StartIO.
        
        // Update our client data.
        //
        // We add the work to the task queue, rather than doing it here, because BeginIOOperation and EndIOOperation
        // also add this task to the queue and the updates should be done in order.
        bool didStartIO = mTaskQueue.QueueSync_StartClientIO(&mClients, inClientID);
        
        // We only tell the hardware to start if this is the first time IO has been started.
        if(didStartIO)
        {
            kern_return_t theError = _HW_StartIO();
            ThrowIfKernelError(theError,
                               CAException(theError),
                               "BGM_Device::StartIO: Failed to start because of an error calling down to the driver.");
        }
        
        clientIsBGMApp = mClients.IsBGMApp(inClientID);
        bgmAppHasClientRegistered = mClients.BGMAppHasClientRegistered();
    }
    
    // We only return from StartIO after BGMApp is ready to pass the audio through to the output device. That way
    // the HAL doesn't start sending us data before BGMApp can play it, which would mean we'd have to either drop
    // frames or increase latency.
    if(!clientIsBGMApp && bgmAppHasClientRegistered)
    {
        UInt64 theXPCError = WaitForBGMAppToStartOutputDevice();
        
        switch(theXPCError)
        {
            case kBGMXPC_Success:
                DebugMsg("BGM_Device::StartIO: Ready for IO.");
                break;
        
            case kBGMXPC_MessageFailure:
                // This most likely means BGMXPCHelper isn't installed or has crashed. IO will probably still work,
                // but we may drop frames while the audio hardware starts up.
                LogError("BGM_Device::StartIO: Couldn't reach BGMApp via XPC. Attempting to start IO anyway.");
                break;
                
            case kBGMXPC_HardwareNotStartingError:
                // This can (and might always) happen when the user changes output device in BGMApp while IO is running.
                // See BGMAudioDeviceManager::waitForOutputDeviceToStart and BGMPlayThrough::WaitForOutputDeviceToStart.
                LogWarning("BGM_Device::StartIO: BGMApp hadn't been told to start IO, so BGMDriver has to return early "
                           "from StartIO. Attempting to start IO anyway.");
                break;
                
            default:
                LogError("BGM_Device::StartIO: BGMApp failed to start the output device. theXPCError=%llu", theXPCError);
                Throw(CAException(kAudioHardwareNotRunningError));
        }
    }
}

void	BGM_Device::StopIO(UInt32 inClientID)
{
    CAMutex::Locker theStateLocker(mStateMutex);
    
    // Update our client data.
    //
    // We add the work to the task queue, rather than doing it here, because BeginIOOperation and EndIOOperation also
    // add this task to the queue and the updates should be done in order.
    bool didStopIO = mTaskQueue.QueueSync_StopClientIO(&mClients, inClientID);
	
	//	we tell the hardware to stop if this is the last stop call
	if(didStopIO)
	{
		_HW_StopIO();
	}
}

void	BGM_Device::GetZeroTimeStamp(Float64& outSampleTime, UInt64& outHostTime, UInt64& outSeed)
{
	// accessing the buffers requires holding the IO mutex
	CAMutex::Locker theIOLocker(mIOMutex);
    
    if(mWrappedAudioEngine != NULL)
    {
    }
    else
    {
        // Without a wrapped device, we base our timing on the host. This is mostly from Apple's NullAudio.c sample code
    	UInt64 theCurrentHostTime;
    	Float64 theHostTicksPerRingBuffer;
    	Float64 theHostTickOffset;
    	UInt64 theNextHostTime;
    	
    	//	get the current host time
    	theCurrentHostTime = mach_absolute_time();
    	
    	//	calculate the next host time
    	theHostTicksPerRingBuffer = mLoopbackTime.hostTicksPerFrame * kLoopbackRingBufferFrameSize;
    	theHostTickOffset = static_cast<Float64>(mLoopbackTime.numberTimeStamps + 1) * theHostTicksPerRingBuffer;
    	theNextHostTime = mLoopbackTime.anchorHostTime + static_cast<UInt64>(theHostTickOffset);
    	
    	//	go to the next time if the next host time is less than the current time
    	if(theNextHostTime <= theCurrentHostTime)
    	{
            mLoopbackTime.numberTimeStamps++;
    	}
    	
    	//	set the return values
    	outSampleTime = mLoopbackTime.numberTimeStamps * kLoopbackRingBufferFrameSize;
    	outHostTime = static_cast<UInt64>(mLoopbackTime.anchorHostTime + (static_cast<Float64>(mLoopbackTime.numberTimeStamps) * theHostTicksPerRingBuffer));
        // TODO: I think we should increment outSeed whenever this device switches to/from having a wrapped engine
    	outSeed = 1;
    }
}

void	BGM_Device::WillDoIOOperation(UInt32 inOperationID, bool& outWillDo, bool& outWillDoInPlace) const
{
	switch(inOperationID)
	{
        case kAudioServerPlugInIOOperationThread:
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationProcessOutput:
		case kAudioServerPlugInIOOperationWriteMix:
			outWillDo = true;
			outWillDoInPlace = true;
			break;
			
		case kAudioServerPlugInIOOperationCycle:
        case kAudioServerPlugInIOOperationConvertInput:
        case kAudioServerPlugInIOOperationProcessInput:
		case kAudioServerPlugInIOOperationMixOutput:
		case kAudioServerPlugInIOOperationProcessMix:
		case kAudioServerPlugInIOOperationConvertMix:
		default:
			outWillDo = false;
			outWillDoInPlace = true;
			break;
			
	};
}

void	BGM_Device::BeginIOOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, UInt32 inClientID)
{
	#pragma unused(inIOBufferFrameSize, inIOCycleInfo)
    
    if(inOperationID == kAudioServerPlugInIOOperationThread)
    {
        // Update this client's IO state and send notifications if that changes the value of
        // kAudioDeviceCustomPropertyDeviceIsRunning or
        // kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp. We have to do this here
        // as well as in StartIO because the HAL only calls StartIO/StopIO with the first/last clients.
        //
        // We perform the update async because it isn't real-time safe, but we can't just dispatch it with
        // dispatch_async because that isn't real-time safe either. (Apparently even constructing a block
        // isn't.)
        //
        // We don't have to hold the IO mutex here because mTaskQueue and mClients don't change and
        // adding a task to mTaskQueue is thread safe.
        mTaskQueue.QueueAsync_StartClientIO(&mClients, inClientID);
    }
}

void	BGM_Device::DoIOOperation(AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
    #pragma unused(inStreamObjectID, ioSecondaryBuffer)
    
	switch(inOperationID)
	{
		case kAudioServerPlugInIOOperationReadInput:
            {
                CAMutex::Locker theIOLocker(mIOMutex);
                ReadInputData(inIOBufferFrameSize, inIOCycleInfo.mInputTime.mSampleTime, ioMainBuffer);
            }
			break;
            
        case kAudioServerPlugInIOOperationProcessOutput:
            {
                CAMutex::Locker theIOLocker(mIOMutex);
                // Called in this IO operation so we can get the music player client's data separately
                UpdateAudibleStateSampleTimes_PreMix(inClientID, inIOBufferFrameSize, inIOCycleInfo.mOutputTime.mSampleTime, ioMainBuffer);
            }
            ApplyClientRelativeVolume(inClientID, inIOBufferFrameSize, ioMainBuffer);
            break;

        case kAudioServerPlugInIOOperationWriteMix:
            {
                CAMutex::Locker theIOLocker(mIOMutex);
                UpdateAudibleStateSampleTimes_PostMix(inIOBufferFrameSize, inIOCycleInfo.mOutputTime.mSampleTime, ioMainBuffer);
                UpdateDeviceAudibleState(inIOBufferFrameSize, inIOCycleInfo.mOutputTime.mSampleTime);
                WriteOutputData(inIOBufferFrameSize, inIOCycleInfo.mOutputTime.mSampleTime, ioMainBuffer);
            }
			break;
	};
}

void	BGM_Device::EndIOOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, UInt32 inClientID)
{
    #pragma unused(inIOBufferFrameSize, inIOCycleInfo)
    if(inOperationID == kAudioServerPlugInIOOperationThread)
    {
        // Tell BGM_Clients that this client has stopped IO. Queued async because we have to be real-time safe here.
        //
        // We don't have to hold the IO mutex here because mTaskQueue and mClients don't change and adding a task to
        // mTaskQueue is thread safe.
        mTaskQueue.QueueAsync_StopClientIO(&mClients, inClientID);
    }
}

void	BGM_Device::ReadInputData(UInt32 inIOBufferFrameSize, Float64 inSampleTime, void* outBuffer)
{
	//	figure out where we are starting
	UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
	UInt32 theStartFrameOffset = theSampleTime % kLoopbackRingBufferFrameSize;
	
	//	figure out how many frames we need to copy
	UInt32 theNumberFramesToCopy1 = inIOBufferFrameSize;
	UInt32 theNumberFramesToCopy2 = 0;
	if((theStartFrameOffset + theNumberFramesToCopy1) > kLoopbackRingBufferFrameSize)
	{
		theNumberFramesToCopy1 = kLoopbackRingBufferFrameSize - theStartFrameOffset;
		theNumberFramesToCopy2 = inIOBufferFrameSize - theNumberFramesToCopy1;
	}
	
	//	do the copying (the byte sizes here assume a 32 bit stereo sample format)
	Float32* theDestination = reinterpret_cast<Float32*>(outBuffer);
	memcpy(theDestination, mLoopbackRingBuffer + (theStartFrameOffset * 2), theNumberFramesToCopy1 * 8);
	if(theNumberFramesToCopy2 > 0)
	{
		memcpy(theDestination + (theNumberFramesToCopy1 * 2), mLoopbackRingBuffer, theNumberFramesToCopy2 * 8);
    }
    
    //DebugMsg("BGM_Device::ReadInputData: Reading. theSampleTime=%llu theStartFrameOffset=%u theNumberFramesToCopy1=%u theNumberFramesToCopy2=%u", theSampleTime, theStartFrameOffset, theNumberFramesToCopy1, theNumberFramesToCopy2);
}

void	BGM_Device::WriteOutputData(UInt32 inIOBufferFrameSize, Float64 inSampleTime, const void* inBuffer)
{
	//	figure out where we are starting
	UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
	UInt32 theStartFrameOffset = theSampleTime % kLoopbackRingBufferFrameSize;
	
	//	figure out how many frames we need to copy
	UInt32 theNumberFramesToCopy1 = inIOBufferFrameSize;
	UInt32 theNumberFramesToCopy2 = 0;
	if((theStartFrameOffset + theNumberFramesToCopy1) > kLoopbackRingBufferFrameSize)
	{
		theNumberFramesToCopy1 = kLoopbackRingBufferFrameSize - theStartFrameOffset;
		theNumberFramesToCopy2 = inIOBufferFrameSize - theNumberFramesToCopy1;
	}
	
	//	do the copying (the byte sizes here assume a 32 bit stereo sample format)
	const Float32* theSource = reinterpret_cast<const Float32*>(inBuffer);
	memcpy(mLoopbackRingBuffer + (theStartFrameOffset * 2), theSource, theNumberFramesToCopy1 * 8);
	if(theNumberFramesToCopy2 > 0)
	{
		memcpy(mLoopbackRingBuffer, theSource + (theNumberFramesToCopy1 * 2), theNumberFramesToCopy2 * 8);
    }
    
    //DebugMsg("BGM_Device::WriteOutputData: Writing. theSampleTime=%llu theStartFrameOffset=%u theNumberFramesToCopy1=%u theNumberFramesToCopy2=%u", theSampleTime, theStartFrameOffset, theNumberFramesToCopy1, theNumberFramesToCopy2);
}

void	BGM_Device::ApplyClientRelativeVolume(UInt32 inClientID, UInt32 inIOBufferFrameSize, void* ioBuffer) const
{
    Float32* theBuffer = reinterpret_cast<Float32*>(ioBuffer);
    Float32 theRelativeVolume = mClients.GetClientRelativeVolumeRT(inClientID);
    
    auto thePanPositionInt = mClients.GetClientPanPositionRT(inClientID);
    Float32 thePanPosition = static_cast<Float32>(thePanPositionInt) / 100.0f;
    
    // TODO When we get around to supporting devices with more than two channels, it would be worth looking into
    //      kAudioFormatProperty_PanningMatrix and kAudioFormatProperty_BalanceFade in AudioFormat.h.
    
    // TODO precompute matrix coefficients w/ volume and do everything in one pass
    
    // Apply balance w/ crossfeed to the frames in the buffer.
    // Expect samples interleaved, starting with left
    if (thePanPosition > 0.0f) {
        for (UInt32 i = 0; i < inIOBufferFrameSize * 2; i += 2) {
            auto L = i;
            auto R = i + 1;
            
            theBuffer[R] = theBuffer[R] + theBuffer[L] * thePanPosition;
            theBuffer[L] = theBuffer[L] * (1 - thePanPosition);
        }
    } else if (thePanPosition < 0.0f) {
        for (UInt32 i = 0; i < inIOBufferFrameSize * 2; i += 2) {
            auto L = i;
            auto R = i + 1;
            
            theBuffer[L] = theBuffer[L] + theBuffer[R] * (-thePanPosition);
            theBuffer[R] = theBuffer[R] * (1 + thePanPosition);
        }
    }

    if(theRelativeVolume != 1.0f)
    {
        for(UInt32 i = 0; i < inIOBufferFrameSize * 2; i++)
        {
            Float32 theAdjustedSample = theBuffer[i] * theRelativeVolume;
            
            // Clamp to [-1, 1].
            // (This way is roughly 6 times faster than using std::min and std::max because the compiler can vectorize the loop.)
            const Float32 theAdjustedSampleClippedBelow = theAdjustedSample < -1.0f ? -1.0f : theAdjustedSample;
            theBuffer[i] = theAdjustedSampleClippedBelow > 1.0f ? 1.0f : theAdjustedSampleClippedBelow;
        }
    }
}

bool	BGM_Device::BufferIsAudible(UInt32 inIOBufferFrameSize, const void* inBuffer)
{
    // Check each frame to see if any are audible
    for(UInt32 i = 0; i < inIOBufferFrameSize * 2; i++)
    {
        if (0.0f != reinterpret_cast<const Float32*>(inBuffer)[i]) {
            return true;
        }
    }
    
    return false;
}

void	BGM_Device::UpdateAudibleStateSampleTimes_PreMix(UInt32 inClientID, UInt32 inIOBufferFrameSize, Float64 inOutputSampleTime, const void* inBuffer)
{
    // Updates the sample times of the most recent audible music, silent music and audible non-music samples we've received
    
    Float64 endFrameSampleTime = inOutputSampleTime + inIOBufferFrameSize - 1;
    
    if(mClients.IsMusicPlayerRT(inClientID))
    {
        if(BufferIsAudible(inIOBufferFrameSize, inBuffer))
        {
            mAudibleStateSampleTimes.latestAudibleMusic = std::max(mAudibleStateSampleTimes.latestAudibleMusic, endFrameSampleTime);
        }
        else
        {
            mAudibleStateSampleTimes.latestSilentMusic = std::max(mAudibleStateSampleTimes.latestSilentMusic, endFrameSampleTime);
        }
    }
    else if(endFrameSampleTime > mAudibleStateSampleTimes.latestAudibleNonMusic &&  // Don't bother checking the buffer if it won't change anything
            BufferIsAudible(inIOBufferFrameSize, inBuffer))
    {
        mAudibleStateSampleTimes.latestAudibleNonMusic = std::max(mAudibleStateSampleTimes.latestAudibleNonMusic, endFrameSampleTime);
    }
}

void	BGM_Device::UpdateAudibleStateSampleTimes_PostMix(UInt32 inIOBufferFrameSize, Float64 inOutputSampleTime, const void* inBuffer)
{
    // Updates the sample time of the most recent silent sample we've received. (The music player client is not considered separate
    // for the latest silent sample.)
    
    bool audible = BufferIsAudible(inIOBufferFrameSize, inBuffer);
    Float64 endFrameSampleTime = inOutputSampleTime + inIOBufferFrameSize - 1;
    
    if(!audible)
    {
        mAudibleStateSampleTimes.latestSilent = std::max(mAudibleStateSampleTimes.latestSilent, endFrameSampleTime);
    }
}

void	BGM_Device::UpdateDeviceAudibleState(UInt32 inIOBufferFrameSize, Float64 inOutputSampleTime)
{
    // The sample time of the last frame we're looking at
    Float64 endFrameSampleTime = inOutputSampleTime + inIOBufferFrameSize - 1;
    Float64 sinceLatestSilent = endFrameSampleTime - mAudibleStateSampleTimes.latestSilent;
    Float64 sinceLatestMusicSilent = endFrameSampleTime - mAudibleStateSampleTimes.latestSilentMusic;
    Float64 sinceLatestAudible = endFrameSampleTime - mAudibleStateSampleTimes.latestAudibleNonMusic;
    Float64 sinceLatestMusicAudible = endFrameSampleTime - mAudibleStateSampleTimes.latestAudibleMusic;
    bool sendNotifications = false;
    
    // Update mDeviceAudibleState
    
    // Change from silent/silentExceptMusic to audible
    if(mDeviceAudibleState != kBGMDeviceIsAudible &&
       sinceLatestSilent >= kDeviceAudibleStateMinChangedFramesForUpdate &&
       // Check that non-music audio is currently playing
       sinceLatestAudible <= 0 && mAudibleStateSampleTimes.latestAudibleNonMusic != 0)
    {
        DebugMsg("BGM_Device::UpdateDeviceAudibleState: Changing kAudioDeviceCustomPropertyDeviceAudibleState to audible");
        mDeviceAudibleState = kBGMDeviceIsAudible;
        CAMemoryBarrier();
        sendNotifications = true;
    }
    // Change from silent to silentExceptMusic
    else if(((mDeviceAudibleState == kBGMDeviceIsSilent &&
              sinceLatestMusicSilent >= kDeviceAudibleStateMinChangedFramesForUpdate) ||
                 // ...or from audible to silentExceptMusic
                 (mDeviceAudibleState == kBGMDeviceIsAudible &&
                  sinceLatestAudible >= kDeviceAudibleStateMinChangedFramesForUpdate &&
                  sinceLatestMusicSilent >= kDeviceAudibleStateMinChangedFramesForUpdate)) &&
            // In case we haven't seen any music samples yet (either audible or silent), check that music is currently playing
            sinceLatestMusicAudible <= 0 && mAudibleStateSampleTimes.latestAudibleMusic != 0)
    {
        DebugMsg("BGM_Device::UpdateDeviceAudibleState: Changing kAudioDeviceCustomPropertyDeviceAudibleState to silent except music");
        mDeviceAudibleState = kBGMDeviceIsSilentExceptMusic;
        CAMemoryBarrier();
        sendNotifications = true;
    }
    // Change from audible/silentExceptMusic to silent
    else if(mDeviceAudibleState != kBGMDeviceIsSilent &&
            sinceLatestAudible >= kDeviceAudibleStateMinChangedFramesForUpdate &&
            sinceLatestMusicAudible >= kDeviceAudibleStateMinChangedFramesForUpdate)
    {
        DebugMsg("BGM_Device::UpdateDeviceAudibleState: Changing kAudioDeviceCustomPropertyDeviceAudibleState to silent");
        mDeviceAudibleState = kBGMDeviceIsSilent;
        CAMemoryBarrier();
        sendNotifications = true;
    }

    if(sendNotifications)
    {
        // I'm pretty sure we don't have to use RequestDeviceConfigurationChange for this, but the docs seemed a little unclear to me
        mTaskQueue.QueueAsync_SendPropertyNotification(kAudioDeviceCustomPropertyDeviceAudibleState);
    }
}

#pragma mark Accessors

void    BGM_Device::RequestEnabledControls(bool inVolumeEnabled, bool inMuteEnabled)
{
    CAMutex::Locker theStateLocker(mStateMutex);

    bool changeVolume = (mOutputVolumeControlEnabled != inVolumeEnabled);
    bool changeMute = (mOutputMuteControlEnabled != inMuteEnabled);

    if(changeVolume)
    {
        DebugMsg("BGM_Device::RequestEnabledControls: %s volume control",
                 (inVolumeEnabled ? "Enabling" : "Disabling"));
        mPendingOutputVolumeControlEnabled = inVolumeEnabled;
    }

    if(changeMute)
    {
        DebugMsg("BGM_Device::RequestEnabledControls: %s mute control",
                 (inMuteEnabled ? "Enabling" : "Disabling"));
        mPendingOutputMuteControlEnabled = inMuteEnabled;
    }

    if(changeVolume || changeMute)
    {
        // Ask the host to stop IO (and whatever else) so we can safely update the device's list of
        // controls. See RequestDeviceConfigurationChange in AudioServerPlugIn.h.
        AudioObjectID theDeviceObjectID = GetObjectID();
        UInt64 action = static_cast<UInt64>(ChangeAction::SetEnabledControls);
        
        CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
            BGM_PlugIn::Host_RequestDeviceConfigurationChange(theDeviceObjectID, action, nullptr);
        });
    }
}

Float64	BGM_Device::GetSampleRate() const
{
    // The sample rate is guarded by the state lock. Note that we don't need to take the IO lock.
    CAMutex::Locker theStateLocker(mStateMutex);

    Float64 theSampleRate;

    // Report the sample rate from the wrapped device if we have one. Note that _HW_GetSampleRate
    // the device's nominal sample rate, not one calculated from its timestamps.
    if(mWrappedAudioEngine == nullptr)
    {
        theSampleRate = mLoopbackSampleRate;
    }
    else
    {
        theSampleRate = _HW_GetSampleRate();
    }

    return theSampleRate;
}

void	BGM_Device::RequestSampleRate(Float64 inRequestedSampleRate)
{
    // Changing the sample rate needs to be handled via the RequestConfigChange/PerformConfigChange
    // machinery. See RequestDeviceConfigurationChange in AudioServerPlugIn.h.

	// We try to support any sample rate a real output device might.
    ThrowIf(inRequestedSampleRate < 1.0,
            CAException(kAudioDeviceUnsupportedFormatError),
            "BGM_Device::RequestSampleRate: unsupported sample rate");

    DebugMsg("BGM_Device::RequestSampleRate: Sample rate change requested: %f",
             inRequestedSampleRate);

    CAMutex::Locker theStateLocker(mStateMutex);

    if(inRequestedSampleRate != GetSampleRate())  // Check the sample rate will actually be changed.
    {
        mPendingSampleRate = inRequestedSampleRate;

        // Dispatch this so the change can happen asynchronously.
        auto requestSampleRate = ^{
			UInt64 action = static_cast<UInt64>(ChangeAction::SetSampleRate);
            BGM_PlugIn::Host_RequestDeviceConfigurationChange(GetObjectID(), action, nullptr);
        };

        CADispatchQueue::GetGlobalSerialQueue().Dispatch(false, requestSampleRate);
    }
}

void    BGM_Device::SetEnabledControls(bool inVolumeEnabled, bool inMuteEnabled)
{
    CAMutex::Locker theStateLocker(mStateMutex);

    if(mOutputVolumeControlEnabled != inVolumeEnabled)
    {
        DebugMsg("BGM_Device::SetEnabledControls: %s the volume control",
                 inVolumeEnabled ? "Enabling" : "Disabling");

        mOutputVolumeControlEnabled = inVolumeEnabled;
    }

    if(mOutputMuteControlEnabled != inMuteEnabled)
    {
        DebugMsg("BGM_Device::SetEnabledControls: %s the mute control",
                 inMuteEnabled ? "Enabling" : "Disabling");

        mOutputMuteControlEnabled = inMuteEnabled;
    }
}

void	BGM_Device::SetSampleRate(Float64 inSampleRate)
{
    // We try to support any sample rate a real output device might.
    ThrowIf(inSampleRate < 1.0,
            CAException(kAudioDeviceUnsupportedFormatError),
            "BGM_Device::SetSampleRate: unsupported sample rate");

    CAMutex::Locker theStateLocker(mStateMutex);
    
    // Update the sample rate on the wrapped device if we have one.
    if(mWrappedAudioEngine != nullptr)
    {
        kern_return_t theError = _HW_SetSampleRate(inSampleRate);
        ThrowIfKernelError(theError,
                           CAException(kAudioHardwareUnspecifiedError),
                           "BGM_Device::SetSampleRate: Error setting the sample rate on the "
                           "wrapped audio device.");
    }

    // Update the sample rate for loopback.
    mLoopbackSampleRate = inSampleRate;
    InitLoopback();

    // Update the streams.
    mInputStream.SetSampleRate(inSampleRate);
    mOutputStream.SetSampleRate(inSampleRate);
}

bool    BGM_Device::IsStreamID(AudioObjectID inObjectID) const noexcept
{
    return (inObjectID == kObjectID_Stream_Input) || (inObjectID == kObjectID_Stream_Output);
}

const BGM_Stream&  BGM_Device::GetStreamByID(AudioObjectID inObjectID) const
{
    BGMAssert(IsStreamID(inObjectID),
              "BGM_Device::GetStreamByID: Object is not a stream. inObjectID = %u",
              inObjectID);
    ThrowIf(!IsStreamID(inObjectID),
            CAException(kAudioHardwareBadStreamError),
            "BGM_Device::GetStreamByID: Object is not a stream");

    switch(inObjectID)
    {
        case kObjectID_Stream_Input:
            return mInputStream;

        case kObjectID_Stream_Output:
            return mOutputStream;

        default:
            LogError("BGM_Device::GetStreamByID: Unknown stream ID. inObjectID = %u", inObjectID);
            throw CAException(kAudioHardwareBadStreamError);
    };
}

#pragma mark Hardware Accessors

// TODO: Out of laziness, some of these hardware functions do more than their names suggest

void	BGM_Device::_HW_Open()
{
}

void	BGM_Device::_HW_Close()
{
}

kern_return_t	BGM_Device::_HW_StartIO()
{
    if(mWrappedAudioEngine != NULL)
    {
    }
    
    // Reset the loopback timing values
    mLoopbackTime.numberTimeStamps = 0;
    mLoopbackTime.anchorHostTime = mach_absolute_time();
    // ...and the most-recent audible/silent sample times
    mAudibleStateSampleTimes.latestSilent = 0;
    mAudibleStateSampleTimes.latestAudibleNonMusic = 0;
    mAudibleStateSampleTimes.latestSilentMusic = 0;
    mAudibleStateSampleTimes.latestAudibleMusic = 0;
    
    return KERN_SUCCESS;
}

void	BGM_Device::_HW_StopIO()
{
    if(mWrappedAudioEngine != NULL)
    {
    }
}

Float64	BGM_Device::_HW_GetSampleRate() const
{
    // This function should only be called when wrapping a device.
    ThrowIf(mWrappedAudioEngine == nullptr,
            CAException(kAudioHardwareUnspecifiedError),
            "BGM_Device::_HW_GetSampleRate: No wrapped audio device");

    return mWrappedAudioEngine->GetSampleRate();
}

kern_return_t	BGM_Device::_HW_SetSampleRate(Float64 inNewSampleRate)
{
    // This function should only be called when wrapping a device.
    ThrowIf(mWrappedAudioEngine == nullptr,
            CAException(kAudioHardwareUnspecifiedError),
            "BGM_Device::_HW_SetSampleRate: No wrapped audio device");

    return mWrappedAudioEngine->SetSampleRate(inNewSampleRate);
}

UInt32	BGM_Device::_HW_GetRingBufferFrameSize() const
{
    return (mWrappedAudioEngine != NULL) ? mWrappedAudioEngine->GetSampleBufferFrameSize() : 0;
}

SInt32	BGM_Device::_HW_GetVolumeControlValue(AudioObjectID inObjectID) const
{
    if(mWrappedAudioEngine != NULL)
    {
    }

	switch(inObjectID)
	{
        //case kObjectID_Volume_Input_Master:
            //return mInputMasterVolumeControlRawValueShadow;
			
        case kObjectID_Volume_Output_Master:
            return mOutputMasterVolumeControlRawValueShadow;
	};
    
    Throw(CAException(kAudioHardwareBadObjectError));
}

kern_return_t	BGM_Device::_HW_SetVolumeControlValue(AudioObjectID inObjectID, SInt32 inNewControlValue)
{
    kern_return_t theError = 0;
    
    if(mWrappedAudioEngine != NULL)
    {
    }
	
	//	if there wasn't an error, the new value was applied, so we need to update the shadow
	if(theError == 0)
	{
		switch(inObjectID)
		{
			//case kSimpleAudioDriver_Control_MasterInputVolume:
            	//	make sure the new value is in the proper range
            	//inNewControlValue = std::min(std::max(kSimpleAudioDriver_Control_MinRawVolumeValue, inNewControlValue), kSimpleAudioDriver_Control_MaxRawVolumeValue);
				//mInputMasterVolumeControlRawValueShadow = inNewControlValue;
				//break;
				
            case kObjectID_Volume_Output_Master:
            	//	make sure the new value is in the proper range
            	inNewControlValue = std::min(std::max(mOutputMasterMinRawVolumeShadow, inNewControlValue), mOutputMasterMaxRawVolumeShadow);
                
				mOutputMasterVolumeControlRawValueShadow = inNewControlValue;
				break;
		};
	}
	
	return theError;
}

UInt32 BGM_Device::_HW_GetMuteControlValue(AudioObjectID inObjectID) const
{
    if(inObjectID == kObjectID_Mute_Output_Master)
    {
        if(mWrappedAudioEngine != NULL)
        {
        }

        return mOutputMuteValueShadow;
    }
    
    Throw(CAException(kAudioHardwareBadObjectError));
}

kern_return_t BGM_Device::_HW_SetMuteControlValue(AudioObjectID inObjectID, UInt32 inValue)
{
    #pragma unused(inObjectID)
    
    kern_return_t theError = 0;
    
    if(mWrappedAudioEngine != NULL)
    {
    }
    
    if(theError == 0)
    {
        mOutputMuteValueShadow = inValue;
    }
    
    return theError;
}

#pragma mark Implementation

void	BGM_Device::AddClient(const AudioServerPlugInClientInfo* inClientInfo)
{
    DebugMsg("BGM_Device::AddClient: Adding client %u (%s)",
             inClientInfo->mClientID,
             (inClientInfo->mBundleID == NULL ?
                 "no bundle ID" :
                 CFStringGetCStringPtr(inClientInfo->mBundleID, kCFStringEncodingUTF8)));
    
    CAMutex::Locker theStateLocker(mStateMutex);

    mClients.AddClient(inClientInfo);
}

void	BGM_Device::RemoveClient(const AudioServerPlugInClientInfo* inClientInfo)
{
    DebugMsg("BGM_Device::RemoveClient: Removing client %u (%s)",
             inClientInfo->mClientID,
             CFStringGetCStringPtr(inClientInfo->mBundleID, kCFStringEncodingUTF8));
    
    CAMutex::Locker theStateLocker(mStateMutex);

    // If we're removing BGMApp, reenable all of BGMDevice's controls.
    if(mClients.IsBGMApp(inClientInfo->mClientID))
    {
        RequestEnabledControls(true, true);
    }

    mClients.RemoveClient(inClientInfo->mClientID);
}

void	BGM_Device::PerformConfigChange(UInt64 inChangeAction, void* inChangeInfo)
{
	#pragma unused(inChangeInfo)
    DebugMsg("BGM_Device::PerformConfigChange: inChangeAction = %llu", inChangeAction);

    // Apply a change requested with BGM_PlugIn::Host_RequestDeviceConfigurationChange. See
    // PerformDeviceConfigurationChange in AudioServerPlugIn.h.

    switch(static_cast<ChangeAction>(inChangeAction))
    {
        case ChangeAction::SetSampleRate:
            SetSampleRate(mPendingSampleRate);
            break;

        case ChangeAction::SetEnabledControls:
            SetEnabledControls(mPendingOutputVolumeControlEnabled,
                               mPendingOutputMuteControlEnabled);
            break;

        default:
            throw CAException(kAudioHardwareIllegalOperationError);
            break;
    }
}

void	BGM_Device::AbortConfigChange(UInt64 inChangeAction, void* inChangeInfo)
{
	#pragma unused(inChangeAction, inChangeInfo)
	
	//	this device doesn't need to do anything special if a change request gets aborted
}

