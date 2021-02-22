#include "babl.h"

internal void
RenderWeirdGradient(game_offscreen_buffer* buffer, int x_offset, int y_offset)
{
	uint8_t* row = (uint8_t*)buffer->Memory;
	for (int y = 0; y < buffer->Height; ++y)
	{
		uint32_t* pixel = (uint32_t*)row;
		for (int x = 0; x < buffer->Width; ++x)
		{
			uint8_t r = (uint8_t)(y + y_offset);
			uint8_t g = (uint8_t)(x + x_offset);
			uint8_t b = 0;

			*pixel++ = ((r << 16) | (g << 8) | b);
		}
		row += buffer->Pitch;
	}
}

internal void
RenderPlayer(game_offscreen_buffer* buffer, int player_x, int player_y)
{
	uint8_t* end_of_buffer = (uint8_t*)buffer->Memory + buffer->Pitch * buffer->Height;
	uint32_t Color = 0xFFFFFFFF;
	int Top = player_y;
	int Bottom = player_y + 10;
	for (int X = player_x; X < player_x + 10; X++)
	{
		uint8_t* Pixel = (uint8_t*)buffer->Memory + X*buffer->BytesPerPixel + Top*buffer->Pitch;
		if (Pixel >= buffer->Memory && Pixel <= end_of_buffer)
		{
			for (int Y = Top; Y < Bottom; Y++)
			{

				*(uint32_t*)Pixel = Color;
				Pixel += buffer->Pitch;
			}
		}
	}
}

void
OutputGameSound(game_sound_buffer *SoundBuffer, game_state* GameState)
{
	int16_t tone_volume = 3000;
	int WavePeriod = SoundBuffer->SamplesPerSecond / GameState->ToneHz;

	int16_t* SampleOut = SoundBuffer->Samples;
	for (int SampleIndex = 0; SampleIndex < SoundBuffer->SampleCount; SampleIndex++)
	{
		GameState->tSin += 2.0f * 3.14f * (1.0f / (float)WavePeriod);
		float sin_val = sinf(GameState->tSin);
		int16_t SampleValue = (int16_t)(sin_val * tone_volume);
		*SampleOut++ = SampleValue;
		*SampleOut++ = SampleValue;
	}
}

//extern "C" prevents name mangling, allowing us to preserve the function handle when we import from the .dll
extern "C" GAME_UPDATE_AND_RENDER(GameUpdateAndRender)
{
	Assert(sizeof(game_state) <= Memory->PermanentStorageSize)
	game_state* GameState = (game_state*)Memory->PermanentStorage;
	if (!Memory->IsInitialized)
	{
		char* Filename = "C:/Users/adaml/Documents/Babl/l_fern.png";
		//char* Filename = __FILE__;
		debug_read_file_result BitmapMemory = Memory->DEBUGPlatformReadEntireFile(Filename);
		if (BitmapMemory.Contents)
		{
			Memory->DEBUGPlatformWriteEntireFile("C:/Users/adaml/Documents/Babl/Babl.out", BitmapMemory.ContentSize, BitmapMemory.Contents);
			Memory->DEBUGPlatformFreeFileMemory(BitmapMemory.Contents);
		}

		GameState->ToneHz = 256;
		GameState->GreenOffset = 0;
		GameState->BlueOffset = 0;
		GameState->PlayerX = 100;
		GameState->PlayerY = 100;

		GameState->tSin = 0.0f;
		GameState->tJump = 0.0f;

		Memory->IsInitialized = true; //This really makes more sense in the platform layer, who actually doles memory
	}

	for (int ControllerIndex = 0; ControllerIndex < ArrayCount(Input->Controllers); ControllerIndex++)
	{
		game_controller_input* Controller = &Input->Controllers[ControllerIndex];
		if (Controller->IsAnalog)
		{
			GameState->BlueOffset += (int)(4.0f * Controller->StickY);
			GameState->ToneHz = 256 + (int)(128.0f * Controller->StickX);
		}

		int jump_power = 15;
		if (Controller->FaceDown.EndedDown)
		{
			GameState->tJump = -1.0f;
		}
		GameState->tJump += 0.033f;
		if(GameState->tJump < 0)
			GameState->PlayerY += (int)(sinf(GameState->tJump)*jump_power);

		int player_acceleration = 5;
		GameState->PlayerX += Controller->Left.EndedDown ? -player_acceleration : 0;
		GameState->PlayerX += Controller->Right.EndedDown ? player_acceleration : 0;
		GameState->PlayerY += Controller->Up.EndedDown ? -player_acceleration : 0;
		GameState->PlayerY += Controller->Down.EndedDown ? player_acceleration : 0;
	}

	RenderWeirdGradient(Buffer, GameState->BlueOffset, GameState->GreenOffset);
	RenderPlayer(Buffer, Input->MouseX, Input->MouseY);
}

extern "C" GAME_GET_SOUND_SAMPLES(GameGetSoundSamples)
{
	game_state* GameState = (game_state*)Memory->PermanentStorage;
	OutputGameSound(SoundBuffer, GameState);
}

#if BABL_WIN32
#include "windows.h"
int CALLBACK WinMain(HINSTANCE Instance, HINSTANCE PrevInstance, LPSTR CommandLine, int ShowCode)
{

}

BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,  // handle to DLL module
	DWORD fdwReason,     // reason for calling function
	LPVOID lpReserved)
{
	return(TRUE) ;
}
#endif