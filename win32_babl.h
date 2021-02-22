#ifndef WIN32_BABL_H
#define WIN32_BABL_H
 
struct win32_game_code
{
	HMODULE GameCodeDLL;
	FILETIME DLLLastWriteTime;
	game_update_and_render* UpdateAndRender;
	game_get_sound_samples* GetSoundSamples;
	bool IsValid;
};

struct win32_sound_output
{
	int SamplesPerSecond;
	int BytesPerSample;
	DWORD SecondaryBufferSize;
	DWORD SafetyBytes;
	uint32_t RunningSampleIndex;
};

struct win32_offscreen_buffer
{
	BITMAPINFO BitmapInfo;
	void* Memory;
	int Width;
	int Height;
	int Pitch;
	int BytesPerPixel;
};

struct win32_replay_buffer
{
	HANDLE FileHandle;
	HANDLE MemoryMap;
	char ReplayFilename[MAX_PATH];
	void* MemoryBlock;
};

struct win32_state
{
	uint64_t TotalSize;
	void* GameMemoryBlock;
	win32_replay_buffer ReplayBuffers[4];

	HANDLE RecordingHandle;
	int InputRecordingIndex;

	HANDLE PlaybackHandle;
	int InputPlayingIndex;

	char EXEFileName[MAX_PATH];
	char* OnePastLastSlash;
};

#endif // !WIN32_BABL_H