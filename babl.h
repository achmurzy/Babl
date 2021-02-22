#if !defined(BABL_H)
#define BABL_H
#endif

#include <math.h>
#include <stdint.h>

#define internal static 
#define local_persist static 
#define global_variable static 

typedef int32_t bool32;

#if BABL_SLOW
//Assertion writes to the null pointer, manually crashing the program if (Expression) is false
#define Assert(Expression) \
	if(!(Expression)) {*(int*)0 = 0;}
#else
#define Assert(Expression)
#endif

#define Kilobytes(Value) ((Value)*1024LL)
#define Megabytes(Value) (Kilobytes(Value)*1024LL)
#define Gigabytes(Value) (Megabytes(Value)*1024LL)
#define Terabytes(Value) (Gigabytes(Value)*1024LL)
#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))

inline uint32_t
SafeTruncateUInt64(uint64_t value)
{
	Assert(value <= 0xFFFFFFFF);
	uint32_t FileSize32 = (uint32_t)value;
	return(FileSize32);
}

//Services that the platform layer provides to the game
#if BABL_INTERNAL
struct debug_read_file_result
{
	uint32_t ContentSize;
	void* Contents;
};

#define DEBUG_PLATFORM_READ_ENTIRE_FILE(name) debug_read_file_result name(char* Filename)
typedef DEBUG_PLATFORM_READ_ENTIRE_FILE(debug_platform_read_entire_file);

#define DEBUG_PLATFORM_FREE_FILE_MEMORY(name) void* name(void* Memory)
typedef DEBUG_PLATFORM_FREE_FILE_MEMORY(debug_platform_free_file_memory);

#define DEBUG_PLATFORM_WRITE_ENTIRE_FILE(name) bool name(char* Filename, uint32_t MemorySize, void* Memory)
typedef DEBUG_PLATFORM_WRITE_ENTIRE_FILE(debug_platform_write_entire_file);
#endif

//Services that the game provides to the platform layer
struct game_memory
{
	bool IsInitialized;
	uint64_t PermanentStorageSize;
	void* PermanentStorage;

	uint64_t TransientStorageSize;
	void* TransientStorage;

	debug_platform_read_entire_file* DEBUGPlatformReadEntireFile;
	debug_platform_free_file_memory* DEBUGPlatformFreeFileMemory;
	debug_platform_write_entire_file* DEBUGPlatformWriteEntireFile;
};

struct game_offscreen_buffer
{
	void* Memory;
	int BytesPerPixel;
	int Width;
	int Height;
	int Pitch;
};

struct game_sound_buffer
{
	int SamplesPerSecond;
	int SampleCount;
	int16_t * Samples;
};

struct game_button_state
{
	int HalfTransitionCount;
	bool EndedDown;
};

struct game_controller_input
{
	bool IsAnalog;

	float StickX;
	float StickY;

	union
	{
		game_button_state Buttons[10];
		struct
		{
			game_button_state Up;
			game_button_state Down;
			game_button_state Left;
			game_button_state Right;
			
			game_button_state LeftShoulder;
			game_button_state RightShoulder;

			game_button_state FaceUp;
			game_button_state FaceDown;
			game_button_state FaceLeft;
			game_button_state FaceRight;
		};
	};
	
};

struct game_input_buffer
{
	game_button_state MouseButtons[5];
	int32_t MouseX, MouseY, MouseZ;

	game_controller_input Controllers[5];
};

//Like Unity's Time API, but needs to come from platform
struct game_clock
{
	float SecondsElapsed;
};

struct game_state
{
	int ToneHz;
	int GreenOffset;
	int BlueOffset;
	int PlayerX;
	int PlayerY;

	float tSin = 0;
	float tJump = 0;
};

//Not defining stubs here eases platform layer development where multiple files will import this header
//Requires explicitly checking for nullity when calling these hooks to the game service from a given platform
#define GAME_UPDATE_AND_RENDER(name) void name(game_memory* Memory, game_offscreen_buffer* Buffer, game_input_buffer* Input)
typedef GAME_UPDATE_AND_RENDER(game_update_and_render);
//GAME_UPDATE_AND_RENDER(GameUpdateAndRenderStub)
//{}
//GAME_UPDATE_AND_RENDER(GameUpdateAndRender);

#define GAME_GET_SOUND_SAMPLES(name) void name(game_memory* Memory, game_sound_buffer* SoundBuffer)
typedef GAME_GET_SOUND_SAMPLES(game_get_sound_samples);
//GAME_GET_SOUND_SAMPLES(GameGetSoundSamplesStub)
//{}
//GAME_GET_SOUND_SAMPLES(GameGetSoundSamples);