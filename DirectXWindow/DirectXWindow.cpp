#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>

#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

#if defined(CreateWindow)
#undef CreateWindow
#endif

#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <directx/d3dx12.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>

const uint8_t g_NumFrames = 3; // The number of swap chain back buffers. Should not be less than 2.
bool g_UseWarp = false;
bool g_IsInitialized = false;
uint32_t g_Width = 1280;
uint32_t g_Height = 720;

int main()
{
	return 0;
}

