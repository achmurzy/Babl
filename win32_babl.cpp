#include "babl.h"

#include <windows.h>
#include <stdio.h>
#include <Xinput.h>
#include <dsound.h>

#include "win32_babl.h"

/*
	Examples of things that will need to be done in a platform layer:
	-Saved game locations
	-Getting a handle to our own executable file
	-Asset loading path
	-Threading (launch a thread)
	-Raw input (support multiple devices)
	-Sleep/timeBeginPeriod
	-ClipCursor (Multi-monitor support)
	-Fullscreen support
	-WM_SETCURSOR (control cursor visibility)
	-QueryCancelAutoplay
	-WM_ACTIVEATEAPP (for when we're not the active app)
	-Blit speed improvements
	-Hardware acceleration
	-GetKeyboardLayout (for foreign keyboards)
*/

global_variable bool Running, Pause;
global_variable win32_offscreen_buffer GlobalBackbuffer;
global_variable LPDIRECTSOUNDBUFFER SecondaryBuffer;

DEBUG_PLATFORM_FREE_FILE_MEMORY(DEBUGPlatformFreeFileMemory)
{
	if (Memory)
	{
		VirtualFree(Memory, 0, MEM_RELEASE);
	}
	return 0;
}

DEBUG_PLATFORM_READ_ENTIRE_FILE(DEBUGPlatformReadEntireFile)
{
	debug_read_file_result Result = {};
	HANDLE FileHandle = CreateFile(Filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE)
	{
		LARGE_INTEGER FileSize;
		if (GetFileSizeEx(FileHandle, &FileSize))
		{
			//ReadFile below cannot read larger than sizeof(uint32Max)
			uint32_t FileSize32 = SafeTruncateUInt64(FileSize.QuadPart);
			Result.Contents = VirtualAlloc(0, FileSize32, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
			if (Result.Contents)
			{
				DWORD BytesRead;
				if (ReadFile(FileHandle, Result.Contents, FileSize32, &BytesRead, 0) &&
						FileSize32 == BytesRead)
				{
					Result.ContentSize = FileSize32;
				}
				else
				{
					DEBUGPlatformFreeFileMemory(Result.Contents);
				}
			}
			else
			{
				//Casey leaves meticulous TODO's on all these nested else statements for logging
				//Probably has small task days to clean up these goals to feel good about the code and robustify
			}
		}
		CloseHandle(FileHandle);
	}
	return(Result);
}

DEBUG_PLATFORM_WRITE_ENTIRE_FILE(DEBUGPlatformWriteEntireFile)
{
	bool Result = false;
	HANDLE FileHandle = CreateFile(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE)
	{
		DWORD BytesWritten;
		if (WriteFile(FileHandle, Memory, MemorySize, &BytesWritten, 0))
		{
			Result = (BytesWritten == MemorySize);
		}
		CloseHandle(FileHandle);
	}
	return(Result);
}

//Dynamic loading of functions from Xinput.lib to check for platform compatibility (not all machines will have the library)
//General strategy is to macro the target function headers to get compile-time checking, while also aliasing the API calls we need
//to abstract away from the platform/library, which robustifies
#define X_INPUT_GET_STATE(name) DWORD WINAPI name(DWORD dwUserIndex, XINPUT_STATE* pState)
typedef X_INPUT_GET_STATE(x_input_get_state);
X_INPUT_GET_STATE(XInputGetStateStub)
{
	return ERROR_DEVICE_NOT_CONNECTED;
}
global_variable x_input_get_state* XInputGetState_ = XInputGetStateStub;
#define XInputGetState XInputGetState_

#define X_INPUT_SET_STATE(name) DWORD WINAPI name(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
typedef X_INPUT_SET_STATE(x_input_set_state);
X_INPUT_SET_STATE(XInputSetStateStub)
{
	return ERROR_DEVICE_NOT_CONNECTED;
}
global_variable x_input_set_state* XInputSetState_ = XInputSetStateStub;
#define XInputSetState XInputSetState_

#define DIRECT_SOUND_CREATE(name) HRESULT WINAPI name(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter)
typedef DIRECT_SOUND_CREATE(direct_sound_create);

internal void
Win32LoadXInput(void)
{
	HMODULE XInputLibrary = LoadLibraryA("XInput1_4.dll");
	if (XInputLibrary)
	{
		XInputGetState = (x_input_get_state*)GetProcAddress(XInputLibrary, "XInputGetState");
		XInputSetState = (x_input_set_state*)GetProcAddress(XInputLibrary, "XInputSetState");
	}
	else
	{
		XInputLibrary = LoadLibraryA("XInput1_3.dll");
	}
} 

internal void
CatStrings(size_t SourceACount, char* SourceA,
	size_t SourceBCount, char* SourceB,
	size_t DestCount, char* Dest)
{
	// TODO(casey): Dest bounds checking!

	for (int Index = 0;
		Index < SourceACount;
		++Index)
	{
		*Dest++ = *SourceA++;
	}

	for (int Index = 0;
		Index < SourceBCount;
		++Index)
	{
		*Dest++ = *SourceB++;
	}

	*Dest++ = 0;
}

internal int
StringLength(char* String)
{
	int Count = 0;
	while (*String++)
	{
		++Count;
	}
	return Count;
}

internal void
Win32BuildExePathFilename(win32_state* Win32State, char* Filename, int DestCount, char* Dest)
{
	CatStrings(Win32State->OnePastLastSlash - Win32State->EXEFileName, Win32State->EXEFileName,
		StringLength(Filename), Filename, DestCount, Dest);
}

internal void
Win32GetExeFilename(win32_state* Win32State)
{
	GetModuleFileNameA(0, Win32State->EXEFileName, sizeof(Win32State->EXEFileName));
	Win32State->OnePastLastSlash = Win32State->EXEFileName;
	for (char* Scan = Win32State->EXEFileName;
		*Scan;
		++Scan)
	{
		if (*Scan == '\\')
		{
			Win32State->OnePastLastSlash = Scan + 1;
		}
	}
}

inline FILETIME
Win32GetLastWriteTime(char* Filename)
{
	FILETIME LastWriteTime;
	WIN32_FILE_ATTRIBUTE_DATA FileAttributes;
	GetFileAttributesEx(Filename, GetFileExInfoStandard, &FileAttributes);
	LastWriteTime = FileAttributes.ftLastWriteTime;
	return(LastWriteTime);
}

internal win32_game_code
Win32LoadGameCode(char* SourceDLLName, char* TempDLLName)
{
	win32_game_code Result = {};

	CopyFile(SourceDLLName, TempDLLName, FALSE);
	Result.DLLLastWriteTime = Win32GetLastWriteTime(SourceDLLName);
	
	Result.GameCodeDLL = LoadLibraryA(TempDLLName);
	if (Result.GameCodeDLL)
	{
		Result.UpdateAndRender = (game_update_and_render*)GetProcAddress(Result.GameCodeDLL, "GameUpdateAndRender");
		Result.GetSoundSamples = (game_get_sound_samples*)GetProcAddress(Result.GameCodeDLL, "GameGetSoundSamples");
		Result.IsValid = (Result.UpdateAndRender && Result.GetSoundSamples);
	}

	if (!Result.IsValid)
	{
		Result.UpdateAndRender = 0;
		Result.GetSoundSamples = 0;
	}

	return Result;
}

internal void 
Win32UnloadGameCode(win32_game_code* GameCode)
{
	if (GameCode->GameCodeDLL)
	{
		FreeLibrary(GameCode->GameCodeDLL);
	}
	GameCode->IsValid = false;
	GameCode->UpdateAndRender = 0;
	GameCode->GetSoundSamples = 0;
}

internal void
Win32ProcessXInputDigitalButton(game_button_state OldState, game_button_state* NewState, DWORD ButtonBit, DWORD XInputButtonState)
{
	NewState->EndedDown = (XInputButtonState & ButtonBit) == ButtonBit;
	NewState->HalfTransitionCount = (OldState.EndedDown != NewState->EndedDown) ? 1 : 0;
}

internal float 
Win32ProcessStickValue(SHORT stick_val, SHORT XINPUT_DEADZONE)
{
	if (stick_val < -XINPUT_DEADZONE)
	{
		return (float)stick_val / 32768.0f;
	}
	else if (stick_val > XINPUT_DEADZONE)
	{
		return (float)stick_val / 32767.0f;
	}
	else
		return 0;
}

internal void
Win32ProcessKeyboardMessage(game_button_state* NewState, bool IsDown)
{
	if (NewState->EndedDown != IsDown)
	{
		NewState->EndedDown = IsDown;
		NewState->HalfTransitionCount++;
	}
}

internal void 
ResizeDIBSection(win32_offscreen_buffer *buffer, int width, int height)
{
	int BytesPerPixel = 4;
	int BitmapMemorySize = BytesPerPixel * width * height;

	buffer->BytesPerPixel = BytesPerPixel;
	buffer->Width = width;
	buffer->Height = height;

	if (buffer->Memory)
	{
		//VirtualFree(buffer->BitmapMemory, 0, MEM_RELEASE);
		VirtualFree(buffer->Memory, BitmapMemorySize, MEM_RELEASE);
	}

	buffer->BitmapInfo.bmiHeader.biSize = sizeof(buffer->BitmapInfo.bmiHeader);
	buffer->BitmapInfo.bmiHeader.biWidth = width;
	buffer->BitmapInfo.bmiHeader.biHeight = -height;
	buffer->BitmapInfo.bmiHeader.biPlanes = 1;
	buffer->BitmapInfo.bmiHeader.biBitCount = 32;
	buffer->BitmapInfo.bmiHeader.biCompression = BI_RGB;
	buffer->BitmapInfo.bmiHeader.biSizeImage = 0;
	buffer->BitmapInfo.bmiHeader.biXPelsPerMeter = 0;
	buffer->BitmapInfo.bmiHeader.biYPelsPerMeter = 0;
	buffer->BitmapInfo.bmiHeader.biClrUsed = 0;
	buffer->BitmapInfo.bmiHeader.biClrImportant = 0;

	buffer->Memory = VirtualAlloc(0, BitmapMemorySize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
	//This allocation causes a null pointer when we write to pixels later on - make the starting address 0 in order to get the start of the page every time?
	//buffer->BitmapMemory = VirtualAlloc(buffer->BitmapMemory , BitmapMemorySize, MEM_COMMIT, PAGE_READWRITE);
	buffer->Pitch = buffer->Width * BytesPerPixel;	
}

internal void Win32CopyBufferToWindow(win32_offscreen_buffer* buffer, HDC DeviceContext, RECT WindowRect)
{
	//Currently only blitting by buffer size because Casey said so
	//int window_width = WindowRect.right - WindowRect.left;
	//int window_height = WindowRect.bottom - WindowRect.top;
	StretchDIBits(DeviceContext, 0, 0, buffer->Width, buffer->Height, 0, 0, buffer->Width, buffer->Height, buffer->Memory, &buffer->BitmapInfo, DIB_RGB_COLORS, SRCCOPY);
}

internal void 
Win32InitDSound(HWND Window, int32_t BufferSize, int32_t SamplesPerSecond)
{
	HMODULE DSoundLibrary = LoadLibraryA("dsound.dll");

	if (DSoundLibrary)
	{
		direct_sound_create* DirectSoundCreate = (direct_sound_create*)GetProcAddress(DSoundLibrary, "DirectSoundCreate");
		LPDIRECTSOUND DirectSound;
		if (DirectSoundCreate && SUCCEEDED(DirectSoundCreate(0, &DirectSound, 0)))
		{
			WAVEFORMATEX WaveFormat = {};
			WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
			WaveFormat.nChannels = 2;
			WaveFormat.nSamplesPerSec = SamplesPerSecond;
			WaveFormat.wBitsPerSample = 16;
			WaveFormat.nBlockAlign = (WaveFormat.nChannels * WaveFormat.wBitsPerSample) / 8; //[LEFT-channel RIGHT-channel] [LEFT-channel RIGHT-channel] [LEFT-channel RIGHT-channel]... = 16bits + 16bits = 4 bytes per block
			WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * WaveFormat.nBlockAlign;
			WaveFormat.cbSize = 0;

			if (SUCCEEDED(DirectSound->SetCooperativeLevel(Window, DSSCL_PRIORITY)))
			{
				LPDIRECTSOUNDBUFFER PrimaryBuffer;
				DSBUFFERDESC BufferDescription = {};
				BufferDescription.dwSize = sizeof(BufferDescription);
				BufferDescription.dwFlags = DSBCAPS_PRIMARYBUFFER;

				if (SUCCEEDED(DirectSound->CreateSoundBuffer(&BufferDescription, &PrimaryBuffer, 0)))
				{
					PrimaryBuffer->SetFormat(&WaveFormat);
				}
				else
				{
					//TODO - Diagnostic
				}
			}

			//Casey leaves breadcrumbs all over his code - gives himself every opportunity to catch errors with nested if-elses for Diagnostic
			//TODO: DSBCAPS_GETCURRENTPOSITION2 - after reviewing the API, determines this parameter might be relevant to sound optimization in the future - but not now
			
			DSBUFFERDESC BufferDescription = {};
			BufferDescription.dwSize = sizeof(BufferDescription);
			BufferDescription.dwFlags = 0;
			BufferDescription.dwBufferBytes = BufferSize;
			BufferDescription.lpwfxFormat = &WaveFormat;

			if (SUCCEEDED(DirectSound->CreateSoundBuffer(&BufferDescription, &SecondaryBuffer, 0)))
			{
				
			}
			else
			{
				OutputDebugString("Failed to create secondary sound buffer");
			}
		}
	}
}

internal void
Win32FillSoundBuffer(win32_sound_output* SoundOutput, DWORD ByteToLock, DWORD BytesToWrite, 
						game_sound_buffer* SourceBuffer)
{
	VOID* Region1;
	DWORD Region1Size;
	VOID* Region2;
	DWORD Region2Size;

	if (SUCCEEDED(SecondaryBuffer->Lock(ByteToLock, BytesToWrite, &Region1, &Region1Size, &Region2, &Region2Size, 0)))
	{
		int16_t* SourceSample = SourceBuffer->Samples;

		int16_t* DestSample = (int16_t*)Region1;
		DWORD Region1SampleCount = Region1Size / SoundOutput->BytesPerSample;
		for (DWORD SampleIndex = 0; SampleIndex < Region1SampleCount; SampleIndex++)
		{
			*DestSample++ = *SourceSample++;
			*DestSample++ = *SourceSample++;
			++SoundOutput->RunningSampleIndex;
		}

		DestSample = (int16_t*)Region2;
		DWORD Region2SampleCount = Region2Size / SoundOutput->BytesPerSample;
		for (DWORD SampleIndex = 0; SampleIndex < Region2SampleCount; SampleIndex++)
		{
			*DestSample++ = *SourceSample++;
			*DestSample++ = *SourceSample++;
			++SoundOutput->RunningSampleIndex;
		}

		SecondaryBuffer->Unlock(Region1, Region1Size, Region2, Region2Size);
	}
	else
	{
		//OutputDebugString("Failed to lock buffer");
	}
}

#if 0
internal void
Win32DebugDrawVertical(win32_offscreen_buffer* Backbuffer, int X, int Top, int Bottom, uint32_t Color)
{
	uint8_t* Pixel = ((uint8_t*)Backbuffer->Memory + X * Backbuffer->BytesPerPixel + Top * Backbuffer->Pitch);
	for (int Y = Top; Y < Bottom; Y++)
	{
		*(uint32_t*)Pixel = Color;
		Pixel += Backbuffer->Pitch;
	}
}

internal void
Win32DebugSyncDisplay(win32_offscreen_buffer* Backbuffer, DWORD* DEBUGLastPlayCursor, 
						win32_sound_output* SoundOutput, float TargetSecondsPerFrame)
{
	int pad_x = 16;
	int pad_y = 16;
	int bottom = Backbuffer->Height - pad_y;
	int top = pad_y;
	float buffer_stride = (float)(Backbuffer->Width - 2*pad_x) / (float)(SoundOutput->SecondaryBufferSize);
	//int LastPlayCursorCount = ArrayCount(&DEBUGLastPlayCursor);
	int LastPlayCursorCount = 15;
	for (int PlayCursorIndex = 0; PlayCursorIndex < LastPlayCursorCount; PlayCursorIndex++)
	{
		int x = (int)(buffer_stride * (float)DEBUGLastPlayCursor[PlayCursorIndex]);
		Win32DebugDrawVertical(Backbuffer, x, top, bottom, 0xFFFFFFFF);
	}
}
#endif

internal void
Win32GetInputFileLocation(win32_state* State, bool InputStream,
	int SlotIndex, int DestCount, char* Dest)
{
	char Temp[64];
	wsprintf(Temp, "babl_%d_%s.ir", SlotIndex, InputStream ? "input" : "state");
	Win32BuildExePathFilename(State, Temp, DestCount, Dest);
}

internal win32_replay_buffer* Win32GetReplayBuffer(win32_state* Win32State, int unsigned input_recording_index)
{
	Assert(input_recording_index < ArrayCount(Win32State->ReplayBuffers));
	win32_replay_buffer* ReplayBuffer = &Win32State->ReplayBuffers[input_recording_index];
	return(ReplayBuffer);
}

internal void
Win32BeginRecordingInput(win32_state* Win32State, int input_recording_index)
{
	win32_replay_buffer* ReplayBuffer = Win32GetReplayBuffer(Win32State, input_recording_index);
	if (ReplayBuffer->MemoryBlock)
	{
		Win32State->InputRecordingIndex = input_recording_index;

		char Filename[MAX_PATH];
		Win32GetInputFileLocation(Win32State, true, input_recording_index, sizeof(Filename), Filename);
		Win32State->RecordingHandle = CreateFileA(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
#if 0
		LARGE_INTEGER FilePosition;
		FilePosition.QuadPart = Win32State->TotalSize;
		SetFilePointerEx(Win32State->RecordingHandle, FilePosition, 0, FILE_BEGIN);
#endif
		CopyMemory(ReplayBuffer->MemoryBlock, Win32State->GameMemoryBlock, Win32State->TotalSize);
	}
}

internal void
Win32BeginInputPlayback(win32_state* Win32State, int input_playing_index)
{
	win32_replay_buffer* ReplayBuffer = Win32GetReplayBuffer(Win32State, input_playing_index);
	if (ReplayBuffer->MemoryBlock)
	{
		Win32State->InputPlayingIndex = input_playing_index;
		char Filename[MAX_PATH];
		Win32GetInputFileLocation(Win32State, true, input_playing_index, sizeof(Filename), Filename); 
		Win32State->PlaybackHandle = CreateFileA(Filename, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
#if 0		
		LARGE_INTEGER FilePosition;
		FilePosition.QuadPart = Win32State->TotalSize;
		SetFilePointerEx(Win32State->PlaybackHandle, FilePosition, 0, FILE_BEGIN);
#endif	
		CopyMemory(Win32State->GameMemoryBlock, ReplayBuffer->MemoryBlock, Win32State->TotalSize);
	}
}

internal void
Win32EndRecordingInput(win32_state* Win32State)
{
	CloseHandle(Win32State->RecordingHandle);
	Win32State->InputRecordingIndex = 0;
}

internal void
Win32EndInputPlayback(win32_state* Win32State)
{
	CloseHandle(Win32State->PlaybackHandle);
	Win32State->InputPlayingIndex = 0;
}

internal void
Win32RecordInput(win32_state* Win32State, game_input_buffer* NewInput)
{
	DWORD BytesWritten;
	WriteFile(Win32State->RecordingHandle, NewInput, sizeof(*NewInput), &BytesWritten, 0);
}

internal void
Win32PlaybackInput(win32_state* Win32State, game_input_buffer* NewInput)
{
	DWORD BytesRead;
	if (ReadFile(Win32State->PlaybackHandle, NewInput, sizeof(*NewInput), &BytesRead, 0))
	{
		if (BytesRead == 0)
		{
			int playing_index = Win32State->InputPlayingIndex;
			Win32EndInputPlayback(Win32State);
			Win32BeginInputPlayback(Win32State, playing_index);
			ReadFile(Win32State->PlaybackHandle, NewInput, sizeof(*NewInput), &BytesRead, 0);
		}
	}		
}

internal void
Win32ProcessPendingMessages(game_controller_input* KeyboardController, win32_state* Win32State)
{
	MSG Message;
	while (PeekMessage(&Message, 0, 0, 0, PM_REMOVE))
	{
		switch (Message.message)
		{
			case WM_QUIT:
				Running = false;
				break;
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_KEYDOWN:
			case WM_KEYUP:
			{
				uint32_t VKCode = (uint32_t)Message.wParam;
				bool WasDown = ((Message.lParam & (1 << 30)) != 0);
				bool IsDown = ((Message.lParam & (1 << 31)) == 0);
				if (WasDown != IsDown)
				{
					if (VKCode == VK_UP || VKCode == 'W')
					{
						Win32ProcessKeyboardMessage(&KeyboardController->Up, IsDown);
					}
					else if (VKCode == VK_DOWN || VKCode == 'S')
					{
						Win32ProcessKeyboardMessage(&KeyboardController->Down, IsDown);
					}
					else if (VKCode == VK_LEFT || VKCode == 'A')
					{
						Win32ProcessKeyboardMessage(&KeyboardController->Left, IsDown);
					}
					else if (VKCode == VK_RIGHT || VKCode == 'D')
					{
						Win32ProcessKeyboardMessage(&KeyboardController->Right, IsDown);
					}
					else if (VKCode == 'Q')
					{
						Win32ProcessKeyboardMessage(&KeyboardController->LeftShoulder, IsDown);
					}
					else if (VKCode == 'E')
					{
						Win32ProcessKeyboardMessage(&KeyboardController->RightShoulder, IsDown);
					}
					else if (VKCode == VK_SPACE)
					{
						Win32ProcessKeyboardMessage(&KeyboardController->FaceDown, IsDown);
					}
					else if (VKCode == 'L')
					{
						if (IsDown)
						{
							if (Win32State->InputPlayingIndex == 0)
							{
								if (Win32State->InputRecordingIndex == 0)
								{
									Win32BeginRecordingInput(Win32State, 1);
								}
								else
								{
									Win32EndRecordingInput(Win32State);
									Win32BeginInputPlayback(Win32State, 1);
								}
							}
							else
								Win32EndInputPlayback(Win32State);
						}
					}
					else if (VKCode == 'P')
					{
						if(IsDown)
							Pause = !Pause;
					}
					else if (VKCode == VK_F4)
					{
						bool32 alt_key_was_down = Message.lParam & 1 << 29;
						if (alt_key_was_down)
						{
							Running = false;
						}
					}
				}
			}break;
			default:
			{
				TranslateMessage(&Message);
				DispatchMessage(&Message);
			}
		}
	}
}

LRESULT CALLBACK MainWindowCallback(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	LRESULT Result = 0;

	switch (Message)
	{
		case WM_SIZE:
		{}break;
		case WM_CLOSE:
		{
			Running = false;
			PostQuitMessage(0);
		}break;
		case WM_ACTIVATEAPP:
		{}break;
		case WM_DESTROY:
		{
			Running = false;
		}break;
		case WM_PAINT:
		{
			PAINTSTRUCT Paint;
			HDC DeviceContext = BeginPaint(Window, &Paint);
			RECT ClientRect;
			GetClientRect(Window, &ClientRect);
			Win32CopyBufferToWindow(&GlobalBackbuffer, DeviceContext, ClientRect);
			EndPaint(Window, &Paint);
		}break;
		default:
		{
			Result = DefWindowProc(Window, Message, WParam, LParam);
		}break;
	}

	return Result;
}


global_variable int64_t PerfCountFrequency;
inline float 
Win32GetSecondsElapsed(LARGE_INTEGER Start, LARGE_INTEGER End)
{
	float Result = (float)(End.QuadPart - Start.QuadPart) / (float)PerfCountFrequency;
	return Result;
}

inline LARGE_INTEGER
Win32GetWallClock()
{
	LARGE_INTEGER Result;
	QueryPerformanceCounter(&Result);
	return Result;
}

int CALLBACK WinMain(HINSTANCE Instance, HINSTANCE PrevInstance, LPSTR CommandLine, int ShowCode)
{
	win32_state Win32State = {};

	//Finds our executable by passing '0' to the handle
	//Counts the depth to the executable from the root directory?
	Win32GetExeFilename(&Win32State);

	char SourceGameCodeDLLFullPath[MAX_PATH];
	Win32BuildExePathFilename(&Win32State, "babl.dll", sizeof(SourceGameCodeDLLFullPath), SourceGameCodeDLLFullPath);
	
	char TempGameCodeDLLFullPath[MAX_PATH];
	Win32BuildExePathFilename(&Win32State, "babl_temp.dll", sizeof(TempGameCodeDLLFullPath), TempGameCodeDLLFullPath);

	WNDCLASS WindowClass = {};
	WindowClass.style = CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc = MainWindowCallback;
	WindowClass.hInstance = Instance;
	WindowClass.lpszClassName = "BablClass";

	Win32LoadXInput();
	
	LARGE_INTEGER PerfCountFrequencyResult;
	QueryPerformanceFrequency(&PerfCountFrequencyResult);
	PerfCountFrequency = PerfCountFrequencyResult.QuadPart;

	int64_t LastCycleCount = __rdtsc();
	if (RegisterClassA(&WindowClass))
	{
		HWND Window =
			CreateWindowEx(0, WindowClass.lpszClassName, "Babl", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, Instance, 0);
		if (Window)
		{
			HDC RefreshDC = GetDC(Window);
			int MonitorRefreshHz = GetDeviceCaps(RefreshDC, VREFRESH);
			if (MonitorRefreshHz <= 1)
				MonitorRefreshHz = 60;
			float GameUpdateHz = MonitorRefreshHz / 2.0f;

			DWORD MinimumAudioLatencyBytes = 0;
			float AudioLatencySeconds = 0;
			float TargetSecondsPerFrame = 1.0f / GameUpdateHz;
			UINT DesiredSchedulerMS = 1;
			bool SleepIsGranular = timeBeginPeriod(DesiredSchedulerMS) == TIMERR_NOERROR;

			win32_sound_output SoundOutput = {};
			SoundOutput.SamplesPerSecond = 48000;
			SoundOutput.BytesPerSample = sizeof(int16_t) * 2;
			SoundOutput.SecondaryBufferSize = SoundOutput.SamplesPerSecond * SoundOutput.BytesPerSample;
			SoundOutput.SafetyBytes = (int)((float)SoundOutput.BytesPerSample * (float)SoundOutput.SamplesPerSecond / GameUpdateHz / 2.0f);
			
			Win32InitDSound(Window, SoundOutput.SecondaryBufferSize, SoundOutput.SamplesPerSecond);
			
			//Clear the buffer of any garbage that might be inside
			VOID* Region1;
			DWORD Region1Size;
			VOID* Region2;
			DWORD Region2Size;
			if (SUCCEEDED(SecondaryBuffer->Lock(0, SoundOutput.SecondaryBufferSize, &Region1, &Region1Size, &Region2, &Region2Size, 0)))
			{
				uint8_t* DestSample = (uint8_t*)Region1;
				for (DWORD ByteIndex = 0; ByteIndex < Region1Size; ByteIndex++)
				{
					*DestSample++ = 0;
				}
				DestSample = (uint8_t*)Region2;
				for (DWORD ByteIndex = 0; ByteIndex < Region2Size; ByteIndex++)
				{
					*DestSample++ = 0;
				}

				SecondaryBuffer->Unlock(Region1, Region1Size, Region2, Region2Size);
			}

			SecondaryBuffer->Play(0, 0, DSBPLAY_LOOPING);

			int16_t *Samples = (int16_t*)VirtualAlloc(0, SoundOutput.SecondaryBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#if BABL_INTERNAL
			LPVOID BaseAddress = (LPVOID)Terabytes((uint64_t)2); //Arbitrary known base address when running on a machine we know about (our own)
#else
			LPVOID BaseAddress = 0; 
#endif
			
			game_memory GameMemory = {};
			GameMemory.PermanentStorageSize = Megabytes(64);
			GameMemory.TransientStorageSize = Gigabytes(1);
			
			Win32State.TotalSize = GameMemory.PermanentStorageSize + GameMemory.TransientStorageSize;
			Win32State.GameMemoryBlock = VirtualAlloc(BaseAddress, Win32State.TotalSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); //| MEM_LARGE_PAGES larger page sizes reduces cache misses on translation lookaside buffer
			
			GameMemory.PermanentStorage = Win32State.GameMemoryBlock;
			GameMemory.TransientStorage = ((uint8_t*)GameMemory.PermanentStorage + GameMemory.PermanentStorageSize);

			for (int ReplayIndex = 0; ReplayIndex < ArrayCount(Win32State.ReplayBuffers); ReplayIndex++)
			{
				win32_replay_buffer* ReplayBuffer = &Win32State.ReplayBuffers[ReplayIndex];
				Win32GetInputFileLocation(&Win32State, false, ReplayIndex,
					sizeof(ReplayBuffer->ReplayFilename), ReplayBuffer->ReplayFilename);

				ReplayBuffer->FileHandle =
					CreateFileA(ReplayBuffer->ReplayFilename,
						GENERIC_WRITE | GENERIC_READ, 0, 0, CREATE_ALWAYS, 0, 0);

				DWORD MaxSizeHigh = (Win32State.TotalSize >> 32);
				DWORD MaxSizeLow = (Win32State.TotalSize & 0xFFFFFFFF);
				ReplayBuffer->MemoryMap = CreateFileMapping(ReplayBuffer->FileHandle, 0, PAGE_READWRITE, MaxSizeHigh, MaxSizeLow, 0);
				ReplayBuffer->MemoryBlock = MapViewOfFile(ReplayBuffer->MemoryMap, FILE_MAP_ALL_ACCESS, 0, 0, Win32State.TotalSize);
				if(ReplayBuffer->MemoryBlock)
				{
					 
				}
			}

			GameMemory.DEBUGPlatformReadEntireFile = DEBUGPlatformReadEntireFile;
			GameMemory.DEBUGPlatformWriteEntireFile = DEBUGPlatformWriteEntireFile;
			GameMemory.DEBUGPlatformFreeFileMemory = DEBUGPlatformFreeFileMemory;

			if (Samples && GameMemory.PermanentStorage && GameMemory.TransientStorage)
			{
				RECT ClientRect;
				GetClientRect(Window, &ClientRect);
				int Width = ClientRect.right - ClientRect.left;
				int Height = ClientRect.bottom - ClientRect.top;
				ResizeDIBSection(&GlobalBackbuffer, Width, Height);

				game_input_buffer Input[2] = {};
				game_input_buffer* OldInput = &Input[0];
				game_input_buffer* NewInput = &Input[1];

				Running = true;

				LARGE_INTEGER BeginCounter = Win32GetWallClock();

				bool SoundIsValid = false;
				DWORD LastPlayCursor = 0;
				DWORD LastWriteCursor = 0;
				DWORD DEBUGLastPlayCursorIndex = 0;
				DWORD DEBUGLastPlayCursor[15] = {0};

				win32_game_code Game = Win32LoadGameCode(SourceGameCodeDLLFullPath, TempGameCodeDLLFullPath);
				
				while (Running)
				{
					FILETIME NewDLLWriteTime = Win32GetLastWriteTime(SourceGameCodeDLLFullPath);
					if (CompareFileTime(&NewDLLWriteTime, &Game.DLLLastWriteTime) != 0)
					{
						Win32UnloadGameCode(&Game);
						Game = Win32LoadGameCode(SourceGameCodeDLLFullPath, TempGameCodeDLLFullPath);
					}

					game_controller_input* OldKeyboardController = &OldInput->Controllers[0];
					game_controller_input* NewKeyboardController = &NewInput->Controllers[0];
					game_controller_input ZeroController = {};
					*NewKeyboardController = ZeroController;//={}
					for (int ButtonIndex = 0; ButtonIndex < ArrayCount(NewKeyboardController->Buttons); ButtonIndex++)
					{
						NewKeyboardController->Buttons[ButtonIndex].EndedDown =
							OldKeyboardController->Buttons[ButtonIndex].EndedDown;
					}
					Win32ProcessPendingMessages(NewKeyboardController, &Win32State);
					
					if (!Pause)
					{
						POINT MouseP;
						GetCursorPos(&MouseP);
						ScreenToClient(Window, &MouseP);
						NewInput->MouseX = MouseP.x;
						NewInput->MouseY = MouseP.y;
						NewInput->MouseZ = 0;
						Win32ProcessKeyboardMessage(&NewInput->MouseButtons[0], GetKeyState(VK_LBUTTON) & (1 << 15));
						Win32ProcessKeyboardMessage(&NewInput->MouseButtons[1], GetKeyState(VK_RBUTTON) & (1 << 15));
						Win32ProcessKeyboardMessage(&NewInput->MouseButtons[2], GetKeyState(VK_MBUTTON) & (1 << 15));
						Win32ProcessKeyboardMessage(&NewInput->MouseButtons[3], GetKeyState(VK_XBUTTON1) & (1 << 15));
						Win32ProcessKeyboardMessage(&NewInput->MouseButtons[4], GetKeyState(VK_XBUTTON2) & (1 << 15));

						uint8_t MaxControllerCount = XUSER_MAX_COUNT+1;
						if (MaxControllerCount > ArrayCount(NewInput->Controllers))
							MaxControllerCount = ArrayCount(NewInput->Controllers);

						for (DWORD ControllerIndex = 1; ControllerIndex < MaxControllerCount; ControllerIndex++)
						{
							XINPUT_STATE ControllerState;
							if (XInputGetState(ControllerIndex, &ControllerState) == ERROR_SUCCESS)
							{
								game_controller_input* OldController = &OldInput->Controllers[ControllerIndex];
								game_controller_input* NewController = &NewInput->Controllers[ControllerIndex];

								XINPUT_GAMEPAD Gamepad = ControllerState.Gamepad;
								Win32ProcessXInputDigitalButton(OldController->Up, &NewController->Up, XINPUT_GAMEPAD_DPAD_UP, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->Down, &NewController->Down, XINPUT_GAMEPAD_DPAD_DOWN, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->Left, &NewController->Left, XINPUT_GAMEPAD_DPAD_LEFT, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->Right, &NewController->Right, XINPUT_GAMEPAD_DPAD_RIGHT, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->LeftShoulder, &NewController->LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->RightShoulder, &NewController->RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER, Gamepad.wButtons);

								Win32ProcessXInputDigitalButton(OldController->FaceUp, &NewController->FaceUp, XINPUT_GAMEPAD_Y, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->FaceDown, &NewController->FaceDown, XINPUT_GAMEPAD_A, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->FaceLeft, &NewController->FaceLeft, XINPUT_GAMEPAD_X, Gamepad.wButtons);
								Win32ProcessXInputDigitalButton(OldController->FaceRight, &NewController->FaceRight, XINPUT_GAMEPAD_B, Gamepad.wButtons);

								NewController->IsAnalog = OldController->IsAnalog;

								//Casey did a ton of hand-wringing about whether intraframe input values should be processed
								//I'll find out for myself whether it matters or not
								NewController->StickX = Win32ProcessStickValue(Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
								NewController->StickY = Win32ProcessStickValue(Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);

								/*if (NewController->RightShoulder.EndedDown)
								{
									XINPUT_VIBRATION Vibration;
									Vibration.wLeftMotorSpeed = 60000;
									Vibration.wRightMotorSpeed = 60000;
									XInputSetState(ControllerIndex, &Vibration);
								}*/
							}
							else
							{
								//OutputDebugString("Controller configuration error");
							}
						}

						game_offscreen_buffer Buffer = {};
						Buffer.Memory = GlobalBackbuffer.Memory;
						Buffer.BytesPerPixel = GlobalBackbuffer.BytesPerPixel;
						Buffer.Width = GlobalBackbuffer.Width;
						Buffer.Height = GlobalBackbuffer.Height;
						Buffer.Pitch = GlobalBackbuffer.Pitch;

						if (Win32State.InputRecordingIndex)
						{
							Win32RecordInput(&Win32State, NewInput);
						}
						if (Win32State.InputPlayingIndex)
						{
							Win32PlaybackInput(&Win32State, NewInput);
						}

						//Hook into the main game loop
						if(Game.UpdateAndRender)
							Game.UpdateAndRender(&GameMemory, &Buffer, NewInput); 

						LARGE_INTEGER AudioWallClock = Win32GetWallClock();
						float FromBeginToAudioSeconds = Win32GetSecondsElapsed(BeginCounter, AudioWallClock);

						game_input_buffer* Temp = NewInput;
						NewInput = OldInput;
						OldInput = Temp;

						DWORD ByteToLock = 0, BytesToWrite = 0;
						DWORD TargetCursor = 0;
						DWORD PlayCursor = 0, WriteCursor = 0;
						if (SecondaryBuffer->GetCurrentPosition(&PlayCursor, &WriteCursor) == DS_OK)
						{
							LastPlayCursor = PlayCursor;
							LastWriteCursor = WriteCursor;
							if (!SoundIsValid)
							{
								SoundOutput.RunningSampleIndex = WriteCursor / SoundOutput.BytesPerSample;
								SoundIsValid = true;
							}

							ByteToLock = (SoundOutput.RunningSampleIndex * SoundOutput.BytesPerSample) % SoundOutput.SecondaryBufferSize;

							DWORD ExpectedSoundBytesPerFrame = (int)((float)SoundOutput.SamplesPerSecond * (float)SoundOutput.BytesPerSample / GameUpdateHz);
							DWORD ActualSoundBytesThisFrame = (DWORD)(TargetSecondsPerFrame - FromBeginToAudioSeconds);
							DWORD FrameBoundaryByte = PlayCursor + ActualSoundBytesThisFrame;
							DWORD SafeWriteCursor = WriteCursor;
							if (SafeWriteCursor < PlayCursor)
							{
								SafeWriteCursor += SoundOutput.SecondaryBufferSize;
							}
							Assert(SafeWriteCursor >= PlayCursor);
							SafeWriteCursor += SoundOutput.SafetyBytes;
							//Casey never really signed off on the efficacy of this audio sync code
							//In particular, the target write cursor may need to be more consistently aligned with the write cursor
							//I.e. use smarter across-frame statistics to predict where we should be writing based on game update loop
							bool AudioCardIsLatent = (SafeWriteCursor) >= FrameBoundaryByte;
							if (AudioCardIsLatent)
							{
								TargetCursor = (WriteCursor + ExpectedSoundBytesPerFrame + SoundOutput.SafetyBytes);
							}
							else
							{
								TargetCursor = (FrameBoundaryByte + ExpectedSoundBytesPerFrame);
							}
							TargetCursor = TargetCursor % SoundOutput.SecondaryBufferSize;

							if (ByteToLock > TargetCursor)
							{
								BytesToWrite = SoundOutput.SecondaryBufferSize - ByteToLock;
								BytesToWrite += TargetCursor;
							}
							else
							{
								BytesToWrite = TargetCursor - ByteToLock;
							}

							game_sound_buffer SoundBuffer = {};
							SoundBuffer.SamplesPerSecond = SoundOutput.SamplesPerSecond;
							SoundBuffer.SampleCount = BytesToWrite / SoundOutput.BytesPerSample;
							SoundBuffer.Samples = Samples;

							//Hook into the game service for sound logic
							if(Game.GetSoundSamples)
								Game.GetSoundSamples(&GameMemory, &SoundBuffer);

							DWORD UnwrappedWriteCursor = WriteCursor;
							if (UnwrappedWriteCursor < PlayCursor)
							{
								UnwrappedWriteCursor += SoundOutput.SecondaryBufferSize;
							}
							MinimumAudioLatencyBytes = UnwrappedWriteCursor - PlayCursor;
							AudioLatencySeconds = (float)(MinimumAudioLatencyBytes / SoundOutput.BytesPerSample / SoundOutput.SamplesPerSecond);
							Win32FillSoundBuffer(&SoundOutput, ByteToLock, BytesToWrite, &SoundBuffer);
						}
						else
							SoundIsValid = false;

						LARGE_INTEGER WorkCounter = Win32GetWallClock();
						float SecondsElapsedForWork = Win32GetSecondsElapsed(BeginCounter, WorkCounter);
						float SecondsElapsedForFrame = SecondsElapsedForWork;

						if (SecondsElapsedForFrame < TargetSecondsPerFrame)
						{
							if (SleepIsGranular)
							{
								DWORD SleepMS = (DWORD)((TargetSecondsPerFrame - SecondsElapsedForFrame) * 1000.0f);
								if (SleepMS > 0)
									Sleep(SleepMS);
							}
							//LARGE_INTEGER WaitCounter = Win32GetWallClock();
							//float TestSecondsElapsedForFrame = Win32GetSecondsElapsed(BeginCounter, WaitCounter);
							//Assert(TestSecondsElapsedForFrame < TargetSecondsPerFrame);
							while (SecondsElapsedForFrame < TargetSecondsPerFrame)
							{

								LARGE_INTEGER WaitCounter = Win32GetWallClock();
								SecondsElapsedForFrame = Win32GetSecondsElapsed(BeginCounter, WaitCounter);
							}
						}
						else
						{
							//Missed our target framerate
							OutputDebugString("Missed our target");
						}

						LARGE_INTEGER EndCounter = Win32GetWallClock();
						float frame_seconds = Win32GetSecondsElapsed(BeginCounter, EndCounter);
						float time_elapsed_ms = 1000.0f * frame_seconds;
						BeginCounter = EndCounter;

						HDC DeviceContext = GetDC(Window);
						GetClientRect(Window, &ClientRect);
#if 0
						Win32DebugSyncDisplay(&GlobalBackbuffer, DEBUGLastPlayCursor,
							&SoundOutput, TargetSecondsPerFrame);
#endif
						Win32CopyBufferToWindow(&GlobalBackbuffer, DeviceContext, ClientRect);
						ReleaseDC(Window, DeviceContext);

#if BABL_INTERNAL
						{
							DWORD DEBUGPlayCursor = PlayCursor;
							//Tighten up sound logic so that we can predict how long the game update loop will be
							//This will ensure that we are always writing ahead of the play cursor for direct sound

							DEBUGLastPlayCursor[DEBUGLastPlayCursorIndex++] = DEBUGPlayCursor;
							if (DEBUGLastPlayCursorIndex >= ArrayCount(DEBUGLastPlayCursor))
							{
								DEBUGLastPlayCursorIndex = 0;
							}
						}
#endif
						int64_t EndCycleCount = __rdtsc();
						int64_t CyclesElapsed = EndCycleCount - LastCycleCount;
						LastCycleCount = EndCycleCount;

						float FPS = (float)PerfCountFrequency / frame_seconds;
						float MHZ = CyclesElapsed / (1000.0f * 1000.0f);

						char time_buffer[256];
						sprintf_s(time_buffer, "Milliseconds/frame: %.02fms, %.02fFPS, %.02fMHz\n", time_elapsed_ms, FPS, MHZ);
						OutputDebugString(time_buffer);
					}
				}
			}
		}
	}
	
	return 0;
}