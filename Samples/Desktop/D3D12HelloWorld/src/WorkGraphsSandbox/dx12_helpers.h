#pragma once

#include <windows.h>
#include <iostream>
#include <atlbase.h>
#include <vector>
#include <initguid.h>
#include "dxcapi.h"
#include "d3d12.h"
#include "d3dx12.h"
#include <dxgi1_6.h>

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 614; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = u8".\\D3D12\\"; }

using namespace std;
const char* g_File = "D3D12WorkGraphsSandbox.hlsl";
wstring g_wFile;
bool g_bUseCollections = false;

// use a warp device instead of a hardware device
bool g_useWarpDevice = false;


// Config for example app
static const int APP_NUM_FRAMES_IN_FLIGHT = 3;
static const int APP_NUM_BACK_BUFFERS = 3;
static const int APP_SRV_HEAP_SIZE = 64;
static const int APP_CLEAR_HEAP_SIZE = 4;


//=================================================================================================================================
// Helper / setup code, not specific to work graphs
// Look for "Start of interesting code" further below.
//=================================================================================================================================

//=================================================================================================================================
// Print with flush to get the text out in case there's delays in app
#define PRINT(text) cout << (char*)text << "\n" << flush; 
#define PRINT_NO_NEWLINE(text) cout << text << flush; 

//=================================================================================================================================
void Analyze(HRESULT hr)
{
	PRINT_NO_NEWLINE("HRESULT == ");
	switch (hr)
	{
	case DXGI_ERROR_DEVICE_HUNG:
		PRINT("DXGI_ERROR_DEVICE_HUNG");
		break;
	case DXGI_ERROR_DEVICE_REMOVED:
		PRINT("DXGI_ERROR_DEVICE_REMOVED");
		break;
	default:
		PRINT("0x" << hex << hr);
	}
}
#define VERIFY_SUCCEEDED(hr) {HRESULT hrLocal = hr; if(FAILED(hrLocal)) {PRINT_NO_NEWLINE("Error at: " << __FILE__ << ", line: " << __LINE__ << ", "); Analyze(hrLocal); throw E_FAIL;} }

// Simple free list based allocator from IMGUI sample
struct ExampleDescriptorHeapAllocator
{
	ID3D12DescriptorHeap* Heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
	D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
	UINT                        HeapHandleIncrement;
	std::vector<int>               FreeIndices;
	bool						shader_visible = true;

	void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap, bool is_shader_visible)
	{
		Heap = heap;
		shader_visible = is_shader_visible;
		D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
		HeapType = desc.Type;
		HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
		if(shader_visible)
			HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
		HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
		FreeIndices.reserve((int)desc.NumDescriptors);
		for (int n = desc.NumDescriptors; n > 0; n--)
			FreeIndices.push_back(n);
	}
	void Destroy()
	{
		Heap = NULL;
		FreeIndices.clear();
	}
	void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
	{
		int idx = FreeIndices.back();
		FreeIndices.pop_back();
		out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
		if(shader_visible)
			out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
	}
	void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
	{
		int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
		FreeIndices.push_back(cpu_idx);
	}
};

struct FrameContext
{
	ID3D12CommandAllocator* CommandAllocator;
	UINT64                      FenceValue = 0u;
};

//=================================================================================================================================
class D3DContext
{
public:
	ID3D12Device14* device = nullptr;
	ID3D12InfoQueue* spInfoQueue = nullptr;
	ID3D12GraphicsCommandList10* command_list = nullptr;
	ID3D12CommandQueue* command_queue = nullptr;
	ID3D12Fence* fence = nullptr;
	ID3D12DescriptorHeap* rtv_desc_heap = nullptr;
	ID3D12DescriptorHeap* srv_desc_heap = nullptr;
	ID3D12DescriptorHeap* clear_desc_heap = nullptr;
	UINT64 FenceValue = 0u;
	HANDLE hEvent = nullptr;
	ID3D12Resource* main_render_target_resource[APP_NUM_BACK_BUFFERS] = {};
	D3D12_CPU_DESCRIPTOR_HANDLE mainRenderTargetDescriptor[APP_NUM_BACK_BUFFERS] = {};
	IDXGISwapChain3* swapchain = nullptr;
	bool SwapChainOccluded = false;
	HANDLE hSwapChainWaitableObject = nullptr;
	ExampleDescriptorHeapAllocator srv_desc_heap_alloc;
	ExampleDescriptorHeapAllocator clear_desc_heap_alloc;
	
	FrameContext frameContext[APP_NUM_FRAMES_IN_FLIGHT] = {};
	UINT uFrameIndex = 0u;
};

void CreateRenderTarget(D3DContext& D3D);
void CleanupRenderTarget(D3DContext& D3D);

//=================================================================================================================================
HRESULT CompileFromFile(
	_In_ LPCWSTR pFileName,
	_In_opt_ LPCWSTR pEntrypoint,
	_In_ LPCWSTR pTarget,
	_In_reads_(cDefines) DxcDefine* pDefines,
	_In_ UINT cDefines,
	_Out_ ID3DBlob** ppCode)
{
	HRESULT hr = S_OK;

	*ppCode = nullptr;

	static HMODULE s_hmod = 0;
	static HMODULE s_hmodDxil = 0;
	static DxcCreateInstanceProc s_pDxcCreateInstanceProc = nullptr;
	if (s_hmodDxil == 0)
	{
		s_hmodDxil = LoadLibrary(L"dxil.dll");
		if (s_hmodDxil == 0)
		{
			PRINT("dxil.dll missing or wrong architecture");
			return E_FAIL;
		}
	}
	if (s_hmod == 0)
	{
		s_hmod = LoadLibrary(L"dxcompiler.dll");
		if (s_hmod == 0)
		{
			PRINT("dxcompiler.dll missing or wrong architecture");
			return E_FAIL;
		}

		if (s_pDxcCreateInstanceProc == nullptr)
		{
			s_pDxcCreateInstanceProc = (DxcCreateInstanceProc)GetProcAddress(s_hmod, "DxcCreateInstance");
			if (s_pDxcCreateInstanceProc == nullptr)
			{
				PRINT("Unable to find dxcompiler!DxcCreateInstance");
				return E_FAIL;
			}
		}
	}

	CComPtr<IDxcCompiler> compiler;
	CComPtr<IDxcLibrary> library;
	CComPtr<IDxcBlobEncoding> source;
	CComPtr<IDxcOperationResult> operationResult;
	CComPtr<IDxcIncludeHandler> includeHandler;
	hr = s_pDxcCreateInstanceProc(CLSID_DxcLibrary, __uuidof(IDxcLibrary), reinterpret_cast<LPVOID*>(&library));
	if (FAILED(hr))
	{
		PRINT("Failed to instantiate compiler.");
		return hr;
	}

	HRESULT createBlobHr = library->CreateBlobFromFile(g_wFile.c_str(), nullptr, &source);
	if (createBlobHr != S_OK)
	{
		PRINT("Create Blob From File Failed - perhaps file is missing?");
		return E_FAIL;
	}

	hr = s_pDxcCreateInstanceProc(CLSID_DxcCompiler, __uuidof(IDxcCompiler), reinterpret_cast<LPVOID*>(&compiler));
	if (FAILED(hr))
	{
		PRINT("Failed to instantiate compiler.");
		return hr;
	}

	hr = library->CreateIncludeHandler(&includeHandler);
	if (FAILED(hr))
	{
		PRINT("Failed to create include handler.");
		return hr;
	}

	LPCWSTR args[] = { NULL };
	UINT cArgs = 0;

	hr = compiler->Compile(
		source,
		g_wFile.c_str(),
		pEntrypoint,
		pTarget,
		args, cArgs,
		pDefines, cDefines,
		includeHandler,
		&operationResult);
	if (FAILED(hr))
	{
		PRINT("Failed to compile.");
		return hr;
	}

	operationResult->GetStatus(&hr);
	if (SUCCEEDED(hr))
	{
		hr = operationResult->GetResult((IDxcBlob**)ppCode);
		if (FAILED(hr))
		{
			PRINT("Failed to retrieve compiled code.");
		}
	}
	CComPtr<IDxcBlobEncoding> pErrors;
	if (SUCCEEDED(operationResult->GetErrorBuffer(&pErrors)))
	{
		auto pText = pErrors->GetBufferPointer();
		if (pText)
		{
			PRINT(pText);
		}
	}

	return hr;
}

//=================================================================================================================================
HRESULT CompileDxilLibraryFromFile(
	_In_ LPCWSTR pFile,
	_In_ LPCWSTR pTarget,
	_In_reads_(cDefines) DxcDefine* pDefines,
	_In_ UINT cDefines,
	_Out_ ID3DBlob** ppCode)
{
	return CompileFromFile(pFile, nullptr, pTarget, pDefines, cDefines, ppCode);
}

//=================================================================================================================================
void InitDeviceAndContext(D3DContext& D3D, HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC1 sd;
	{
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = APP_NUM_BACK_BUFFERS;
		sd.Width = 0;
		sd.Height = 0;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.Stereo = FALSE;
	}

	D3D.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	CComPtr<ID3D12Debug1> pDebug;
	VERIFY_SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug)));
	pDebug->EnableDebugLayer();

	D3D_FEATURE_LEVEL FL = D3D_FEATURE_LEVEL_11_0;
	ID3D12Device* spDevice;

	if (g_useWarpDevice)
	{
		CComPtr<IDXGIFactory4> factory;
		VERIFY_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

		CComPtr<IDXGIAdapter> warpAdapter;
		VERIFY_SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
		VERIFY_SUCCEEDED(D3D12CreateDevice(warpAdapter, FL, IID_PPV_ARGS(&spDevice)));
	}
	else
	{
		VERIFY_SUCCEEDED(D3D12CreateDevice(NULL, FL, IID_PPV_ARGS(&spDevice)));
	}
	D3D.device = (ID3D12Device14*) spDevice;
	D3D.spInfoQueue = (ID3D12InfoQueue*)spDevice;

	{
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			desc.NumDescriptors = APP_NUM_BACK_BUFFERS;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			desc.NodeMask = 1;
			VERIFY_SUCCEEDED(D3D.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&D3D.rtv_desc_heap)));

			SIZE_T rtvDescriptorSize = D3D.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = D3D.rtv_desc_heap->GetCPUDescriptorHandleForHeapStart();
			for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
			{
				D3D.mainRenderTargetDescriptor[i] = rtvHandle;
				rtvHandle.ptr += rtvDescriptorSize;
			}
		}
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = APP_CLEAR_HEAP_SIZE;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			desc.NodeMask = 1;
			VERIFY_SUCCEEDED(D3D.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&D3D.clear_desc_heap)));
			D3D.clear_desc_heap_alloc.Create(D3D.device, D3D.clear_desc_heap, false);
		}
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = APP_SRV_HEAP_SIZE;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			D3D.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&D3D.srv_desc_heap));
			D3D.srv_desc_heap_alloc.Create(D3D.device, D3D.srv_desc_heap, true);
		}
	}

	VERIFY_SUCCEEDED(D3D.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&D3D.fence)));

	for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
	{
		VERIFY_SUCCEEDED(D3D.device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&D3D.frameContext[i].CommandAllocator)
		));
	}

	D3D12_COMMAND_QUEUE_DESC CQD = {};
	CQD.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	VERIFY_SUCCEEDED(D3D.device->CreateCommandQueue(&CQD, IID_PPV_ARGS(&D3D.command_queue)));

	ID3D12CommandList* spCL;
	VERIFY_SUCCEEDED(D3D.device->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		D3D.frameContext[0].CommandAllocator,
		nullptr,
		IID_PPV_ARGS(&spCL)));
	D3D.command_list = (ID3D12GraphicsCommandList10*)spCL;
	VERIFY_SUCCEEDED(D3D.command_list->Close())

	{
		IDXGIFactory4* dxgiFactory = nullptr;
		IDXGISwapChain1* swapChain1 = nullptr;
		VERIFY_SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))
		VERIFY_SUCCEEDED(dxgiFactory->CreateSwapChainForHwnd(D3D.command_queue, hWnd, &sd, nullptr, nullptr, &swapChain1))
		VERIFY_SUCCEEDED(swapChain1->QueryInterface(IID_PPV_ARGS(&D3D.swapchain)))
		swapChain1->Release();
		dxgiFactory->Release();
		D3D.swapchain->SetMaximumFrameLatency(APP_NUM_BACK_BUFFERS);
		D3D.hSwapChainWaitableObject = D3D.swapchain->GetFrameLatencyWaitableObject();
	}
	{
		CreateRenderTarget(D3D);
	}
}

void CleanDeviceAndContext(D3DContext& D3D)
{
	CleanupRenderTarget(D3D);
	if (D3D.swapchain)
	{
		D3D.swapchain->SetFullscreenState(false, nullptr);
		D3D.swapchain->Release();
		D3D.swapchain = nullptr;
	}
	if (D3D.hSwapChainWaitableObject != nullptr)
	{
		CloseHandle(D3D.hSwapChainWaitableObject);
	}
	for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
	{
		if (D3D.frameContext[i].CommandAllocator)
		{
			D3D.frameContext[i].CommandAllocator->Release();
			D3D.frameContext[i].CommandAllocator = nullptr;
		}
	}
	if (D3D.command_queue)
	{
		D3D.command_queue->Release();
		D3D.command_queue = nullptr;
	}
	if (D3D.command_list)
	{
		D3D.command_list->Release();
		D3D.command_list = nullptr;
	}
	if (D3D.rtv_desc_heap)
	{
		D3D.rtv_desc_heap->Release();
		D3D.rtv_desc_heap = nullptr;
	}
	if (D3D.srv_desc_heap)
	{
		D3D.srv_desc_heap->Release();
		D3D.srv_desc_heap = nullptr;
	}
	if (D3D.fence)
	{
		D3D.fence->Release();
		D3D.fence = nullptr;
	}
	if (D3D.hEvent)
	{
		CloseHandle(D3D.hEvent);
		D3D.hEvent = nullptr;
	}
	if (D3D.device)
	{
		D3D.device->Release();
		D3D.device = nullptr;
	}
}

//=================================================================================================================================
void Transition(ID3D12GraphicsCommandList* pCL, ID3D12Resource* pResource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter)
{
	if (StateBefore != StateAfter)
	{
		D3D12_RESOURCE_BARRIER RB = {};
		RB.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		RB.Transition.pResource = pResource;
		RB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		RB.Transition.StateBefore = StateBefore;
		RB.Transition.StateAfter = StateAfter;
		pCL->ResourceBarrier(1, &RB);
	}
}

//=================================================================================================================================
void MakeBuffer(
	D3DContext& D3D,
	ID3D12Resource** ppResource,
	UINT64 SizeInBytes,
	D3D12_RESOURCE_FLAGS ResourceMiscFlags = D3D12_RESOURCE_FLAG_NONE,
	D3D12_HEAP_TYPE HeapType = D3D12_HEAP_TYPE_DEFAULT)
{
	CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(SizeInBytes);
	rd.Flags = ResourceMiscFlags;
	CD3DX12_HEAP_PROPERTIES hp(HeapType);

	VERIFY_SUCCEEDED(D3D.device->CreateCommittedResource(
		&hp,
		D3D12_HEAP_FLAG_NONE,
		&rd,
		D3D12_RESOURCE_STATE_COMMON,
		NULL,
		__uuidof(ID3D12Resource),
		(void**)ppResource));
}

//=================================================================================================================================
void UploadData(
	D3DContext& D3D,
	ID3D12Resource* pResource,
	const VOID* pData,
	SIZE_T Size,
	ID3D12Resource** ppStagingResource, // only used if doFlush == false
	D3D12_RESOURCE_STATES CurrentState)
{
	CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_UPLOAD);
	UINT64 IntermediateSize = GetRequiredIntermediateSize(pResource, 0, 1);
	if (Size != IntermediateSize)
	{
		PRINT("Provided Size of pData needs to account for the whole buffer (i.e. equal to GetRequiredIntermediateSize() output)");
		throw E_FAIL;
	}
	CD3DX12_RESOURCE_DESC BufferDesc = CD3DX12_RESOURCE_DESC::Buffer(IntermediateSize);
	CComPtr<ID3D12Resource> pStagingResource;
	if (ppStagingResource == nullptr)
	{
		ppStagingResource = &pStagingResource;
	}
	VERIFY_SUCCEEDED(D3D.device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &BufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(ppStagingResource)));

	bool NeedTransition = (CurrentState & D3D12_RESOURCE_STATE_COPY_DEST) == 0;
	D3D12_RESOURCE_BARRIER BarrierDesc; ZeroMemory(&BarrierDesc, sizeof(BarrierDesc));
	if (NeedTransition)
	{
		// Transition to COPY_DEST
		BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		BarrierDesc.Transition.pResource = pResource;
		BarrierDesc.Transition.Subresource = 0;
		BarrierDesc.Transition.StateBefore = CurrentState;
		BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		D3D.command_list->ResourceBarrier(1, &BarrierDesc);
		swap(BarrierDesc.Transition.StateBefore, BarrierDesc.Transition.StateAfter); // ensure StateBefore represents current state
	}

	// Execute upload
	D3D12_SUBRESOURCE_DATA SubResourceData = { pData, static_cast<LONG_PTR>(Size), static_cast<LONG_PTR>(Size) };
	if (Size != UpdateSubresources(D3D.command_list, pResource, *ppStagingResource, 0, 0, 1, &SubResourceData))
	{
		PRINT("UpdateSubresources returns the number of bytes updated, so 0 if nothing was updated");
		throw E_FAIL;
	}
	if (NeedTransition)
	{
		// Transition back to whatever the app had
		D3D.command_list->ResourceBarrier(1, &BarrierDesc);
		swap(BarrierDesc.Transition.StateBefore, BarrierDesc.Transition.StateAfter); // ensure StateBefore represents current state
	}
}

//=================================================================================================================================
void MakeBufferAndInitialize(
	D3DContext& D3D,
	ID3D12Resource** ppResource,
	const VOID* pInitialData,
	UINT64 SizeInBytes,
	ID3D12Resource** ppStagingResource = nullptr, // only used if doFlush == false
	bool doFlush = true,
	D3D12_RESOURCE_FLAGS ResourceMiscFlags = D3D12_RESOURCE_FLAG_NONE)
{
	MakeBuffer(D3D, ppResource, SizeInBytes, ResourceMiscFlags, D3D12_HEAP_TYPE_DEFAULT);
	UploadData(D3D, *ppResource, pInitialData, SizeInBytes, ppStagingResource, D3D12_RESOURCE_STATE_COMMON);
}

//=================================================================================================================================
void CreateComputeProgram(D3DContext& D3D, LPCWSTR pFileName, LPCWSTR pEntry,
	_In_reads_(cDefines) DxcDefine* pDefines,
	_In_ UINT cDefines,
	ID3D12PipelineState** ppProgram,
	ID3D12RootSignature** ppRootSig = nullptr)
{
	struct PSO_STREAM
	{
		CD3DX12_PIPELINE_STATE_STREAM_CS CS;
	} Stream;

	CComPtr<ID3DBlob> spCode;
	VERIFY_SUCCEEDED(CompileFromFile(pFileName, pEntry, L"cs_6_0", pDefines, cDefines, &spCode));

	Stream.CS = CD3DX12_SHADER_BYTECODE(spCode->GetBufferPointer(), spCode->GetBufferSize());
	D3D12_PIPELINE_STATE_STREAM_DESC StreamDesc;
	StreamDesc.pPipelineStateSubobjectStream = &Stream;
	StreamDesc.SizeInBytes = sizeof(Stream);

	VERIFY_SUCCEEDED(D3D.device->CreatePipelineState(&StreamDesc, IID_PPV_ARGS(ppProgram)));
	if (ppRootSig)
	{
		VERIFY_SUCCEEDED(D3D.device->CreateRootSignature(0, spCode->GetBufferPointer(), spCode->GetBufferSize(), IID_PPV_ARGS(ppRootSig)));
	}
}

//=================================================================================================================================
void PrintDebugMessagesToConsole(D3DContext& D3D)
{
	UINT NumMessages = (UINT)D3D.spInfoQueue->GetNumStoredMessages();
	for (UINT m = 0; m < NumMessages; m++)
	{
		size_t length = 0;
		D3D.spInfoQueue->GetMessage(m, NULL, &length);
		vector< BYTE> Storage;
		Storage.resize(length);
		D3D12_MESSAGE* pMessage = reinterpret_cast<D3D12_MESSAGE*>(&(Storage[0]));
		D3D.spInfoQueue->GetMessage(m, pMessage, &length);
		PRINT(pMessage->pDescription);
	}
}

//=================================================================================================================================
void PrintID(D3D12_NODE_ID ID)
{
	vector<char> name;
	name.resize(wcslen(ID.Name) + 1);
	WideCharToMultiByte(DXC_CP_ACP, 0, ID.Name, -1, name.data(), (int)name.size(), nullptr, nullptr);
	if (ID.ArrayIndex)
	{
		PRINT("(" << name.data() << "," << ID.ArrayIndex << ")");
	}
	else
	{
		PRINT(name.data());
	}
}

FrameContext* WaitForNextFrameResources(D3DContext& D3D)
{
	UINT nextFrameIndex = D3D.uFrameIndex + 1;
	D3D.uFrameIndex = nextFrameIndex;

	HANDLE waitableObjects[] = { D3D.hSwapChainWaitableObject, nullptr };
	DWORD numWaitableObjects = 1;

	FrameContext* frameCtx = &D3D.frameContext[nextFrameIndex % APP_NUM_FRAMES_IN_FLIGHT];
	UINT64 fenceValue = frameCtx->FenceValue;
	if (fenceValue != 0) // means no fence was signaled
	{
		frameCtx->FenceValue = 0;
		D3D.fence->SetEventOnCompletion(fenceValue, D3D.hEvent);
		waitableObjects[1] = D3D.hEvent;
		numWaitableObjects = 2;
	}

	WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);

	return frameCtx;
}

void WaitForLastSubmittedFrame(D3DContext& D3D)
{
	FrameContext* frameCtx = &D3D.frameContext[D3D.uFrameIndex % APP_NUM_FRAMES_IN_FLIGHT];

	UINT64 fenceValue = frameCtx->FenceValue;
	if (fenceValue == 0)
		return; // No fence was signaled

	frameCtx->FenceValue = 0;
	if (D3D.fence->GetCompletedValue() >= fenceValue)
		return;

	D3D.fence->SetEventOnCompletion(fenceValue, D3D.hEvent);
	WaitForSingleObject(D3D.hEvent, INFINITE);
}

void CreateRenderTarget(D3DContext& D3D)
{
	for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
	{
		ID3D12Resource* pBackBuffer = nullptr;
		D3D.swapchain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
		D3D.device->CreateRenderTargetView(pBackBuffer, nullptr, D3D.mainRenderTargetDescriptor[i]);
		D3D.main_render_target_resource[i] = pBackBuffer;
	}
}

void CleanupRenderTarget(D3DContext& D3D)
{
	WaitForLastSubmittedFrame(D3D);

	for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
	{
		if (D3D.main_render_target_resource[i])
		{
			D3D.main_render_target_resource[i]->Release();
			D3D.main_render_target_resource[i] = nullptr;
		}
	}
}

void MakeTextureSRVAndUAV(D3DContext& D3D, ID3D12Resource** ppResource, UINT image_width, UINT image_height, DXGI_FORMAT format,
	D3D12_RESOURCE_FLAGS flags, D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu_handle,
	D3D12_CPU_DESCRIPTOR_HANDLE clear_cpu_handle)
{

	auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, image_width, image_height, 1, 1, 1, 0, flags);
	auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	ID3D12Resource* pTexture = nullptr;
	VERIFY_SUCCEEDED(D3D.device->CreateCommittedResource(
		&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&pTexture)));
	
	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D.device->CreateUnorderedAccessView(pTexture, nullptr, &UAVDesc, uav_cpu_handle);
	D3D.device->CreateUnorderedAccessView(pTexture, nullptr, &UAVDesc, clear_cpu_handle);

	// Create a shader resource view for the texture
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	ZeroMemory(&srvDesc, sizeof(srvDesc));
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	D3D.device->CreateShaderResourceView(pTexture, &srvDesc, srv_cpu_handle);

	*ppResource = pTexture;
}
