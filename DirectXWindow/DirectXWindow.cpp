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
#include "Helpers.h"

const uint8_t g_NumFrames = 3;	// The number of swap chain back buffers. Should not be less than 2.
bool g_UseWarp = false;
bool g_IsInitialized = false;
bool g_VSync = true;
bool g_TearingSupported = false;
bool g_Fullsceen = false;
uint32_t g_Width = 1280;
uint32_t g_Height = 720;
uint64_t g_FenceValue = 0;
uint64_t g_FrameFaceValues[g_NumFrames] = {};
UINT g_RTVDescriptorSize;
UINT g_CurrentBackBufferIndex;
HWND g_hWnd;	// Window handel.
RECT g_WindowRect;	// Window rectangle.
HANDLE g_FenceEvent;
Microsoft::WRL::ComPtr<ID3D12Device> g_Device;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> g_CommandQueue;
Microsoft::WRL::ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames];
Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> g_CommandList;
Microsoft::WRL::ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames];
Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;
Microsoft::WRL::ComPtr<ID3D12Fence> g_Fence;
Microsoft::WRL::ComPtr<IDXGISwapChain4> g_SwapChain;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void ParseCommandLineArguments()
{
	int argc;
	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

	for (size_t i = 0; i < argc; i++)
	{
		if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)	g_Width = ::wcstol(argv[++i], nullptr, 10);
		if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)	g_Height = ::wcstol(argv[++i], nullptr, 10);
		if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)	g_UseWarp = true;
	}
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
	Microsoft::WRL::ComPtr<ID3D12Debug> debugInterface;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
	debugInterface->EnableDebugLayer();
#endif
}

void RegisterWindowClass(HINSTANCE hInst, const wchar_t* windowClassName)
{
	WNDCLASSEXW windowClass = {};

	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = &WndProc;
	windowClass.cbClsExtra = 0;
	windowClass.cbWndExtra = 0;
	windowClass.hInstance = hInst;
	windowClass.hIcon = ::LoadIcon(hInst, NULL);
	windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	windowClass.lpszMenuName = NULL;
	windowClass.lpszClassName = windowClassName;
	windowClass.hIconSm = ::LoadIcon(hInst, NULL);

	static ATOM atom = ::RegisterClassExW(&windowClass);
	assert(atom > 0);
}

HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst, const wchar_t* windowTitle, uint32_t width, uint32_t height)
{
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	int windowX = std::max<int>(0, (screenWidth - windowHeight) / 2);
	int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

	HWND hWnd = ::CreateWindowExW(
		NULL,
		windowClassName,
		windowTitle,
		WS_OVERLAPPEDWINDOW,
		windowX,
		windowY,
		windowWidth,
		windowHeight,
		NULL,
		NULL,
		hInst,
		nullptr
	);

	assert(hWnd && "Failed to create window");

	return hWnd;
}

Microsoft::WRL::ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
	Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

	Microsoft::WRL::ComPtr<IDXGIAdapter1> dxgiAdapter1;
	Microsoft::WRL::ComPtr<IDXGIAdapter4> dxgiAdapter4;

	if (useWarp)
	{
		ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
		ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
	}
	else
	{
		SIZE_T maxDedicatedVideoMemory = 0;
		for (UINT i = 0; i < dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; i++) //Enumerate the available GPU adapters in the system.
		{
			DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
			dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);


			/* 
			* Check to see if the adapter can create a D3D12 device without actually
			* creating it. The adapter with the largest dedicated video memory
			* is favored.
			*/
			if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && // DXGI_ADAPTER_FLAG_SOFTWARE == 0 -> hardware adapters only
				SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(),D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
				dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
			{
				maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
				ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
			}
		}
	}

	return dxgiAdapter4;
}

Microsoft::WRL::ComPtr<ID3D12Device2> CreateDevice(Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter)
{
	Microsoft::WRL::ComPtr<ID3D12Device2> d3d12Device2;
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(),				// Pointer to the adapter
									D3D_FEATURE_LEVEL_11_0,		// Minimum feature level 
									IID_PPV_ARGS(&d3d12Device2)	//Globaly Unique Identifier (GUID) for the device interface
									));

#if defined(_DEBUG)
	Microsoft::WRL::ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(d3d12Device2.As(&pInfoQueue)))
	{
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);


		//Suppress INFO messages (by level)
		D3D12_MESSAGE_SEVERITY Severities[] =
		{
			D3D12_MESSAGE_SEVERITY_INFO
		};

		//Suppres individual messages (by ID)
		D3D12_MESSAGE_ID DenyIds[] =
		{
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE
		};

		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;

		ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
	}
#endif

	return d3d12Device2;
}

Microsoft::WRL::ComPtr<ID3D12CommandQueue> CreateCommandQueue(Microsoft::WRL::ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

	D3D12_COMMAND_QUEUE_DESC desc = {};

	desc.Type = type; 
	/*
	* Specifies the type of command queue to create and can be one of the following types :
	* D3D12_COMMAND_LIST_TYPE_DIRECT:  The command queue can be used to execute draw, compute, and copy commands. This is the most general type of command queue and will be used in most cases.
	* D3D12_COMMAND_LIST_TYPE_COMPUTE: The command queue can be used to execute compute and copy commands.
	* D3D12_COMMAND_LIST_TYPE_COPY: Command queue can be used to execute copy commands.
	*/
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	/*
	* The priority for the command queue. Can be one of the following values:
	* D3D12_COMMAND_QUEUE_PRIORITY_NORMAL: The command queue has normal priority.
	* D3D12_COMMAND_QUEUE_PRIORITY_HIGH: The command queue has high priority.
	* D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME: The command queue has global realtime priority.
	*/
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	/*
	* Specifies additional flags from the D3D12_COMMAND_QUEUE_FLAGS enumeratrion.
	*/
	desc.NodeMask = 0;
	/*
	* For single GPU operation, set this to zero. If there are multiple GPU nodes, set a bit to identify the node (the device’s physical adapter) to which the command queue applies. Each bit in the mask corresponds to a single node. Only 1 bit must be set. 
	*/

	ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

	return d3d12CommandQueue;
}

bool CheckTearingSupport()
{
	BOOL allowTearing = FALSE;

	Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;

	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
	{
		Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory4.As(&factory5)))
		{
			if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
			{
				allowTearing = FALSE;
			}
		}
	}

	return allowTearing == TRUE;

}

int main()
{
	return 0;
}
