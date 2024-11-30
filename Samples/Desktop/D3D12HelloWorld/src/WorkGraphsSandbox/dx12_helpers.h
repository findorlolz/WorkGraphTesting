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

//=================================================================================================================================
class D3DContext
{
public:
	CComPtr<ID3D12Device14> spDevice;
	CComPtr<ID3D12InfoQueue> spInfoQueue;
	CComPtr<ID3D12GraphicsCommandList10> spCL;
	CComPtr<ID3D12CommandQueue> spCQ;
	CComPtr<ID3D12CommandAllocator> spCA;
	CComPtr<ID3D12Fence> spFence;
	UINT64 FenceValue;
	HANDLE hEvent;
};

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
void InitDeviceAndContext(D3DContext& D3D)
{
	D3D.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	CComPtr<ID3D12Debug1> pDebug;
	VERIFY_SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug)));
	pDebug->EnableDebugLayer();

	D3D_FEATURE_LEVEL FL = D3D_FEATURE_LEVEL_11_0;
	CComPtr<ID3D12Device> spDevice;

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
	D3D.spDevice = spDevice;

	D3D.spInfoQueue = spDevice;

	VERIFY_SUCCEEDED(D3D.spDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&D3D.spFence)));

	VERIFY_SUCCEEDED(D3D.spDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&D3D.spCA)
	));

	D3D12_COMMAND_QUEUE_DESC CQD = {};
	CQD.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	VERIFY_SUCCEEDED(D3D.spDevice->CreateCommandQueue(&CQD, IID_PPV_ARGS(&D3D.spCQ)));

	CComPtr<ID3D12CommandList> spCL;
	VERIFY_SUCCEEDED(D3D.spDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		D3D.spCA,
		nullptr,
		IID_PPV_ARGS(&spCL)));
	D3D.spCL = spCL;
}

//=================================================================================================================================
void FlushAndFinish(D3DContext& D3D)
{
	VERIFY_SUCCEEDED(D3D.spCL->Close());
	D3D.spCQ->ExecuteCommandLists(1, CommandListCast(&D3D.spCL.p));

	VERIFY_SUCCEEDED(D3D.spCQ->Signal(D3D.spFence, ++D3D.FenceValue));
	VERIFY_SUCCEEDED(D3D.spFence->SetEventOnCompletion(D3D.FenceValue, D3D.hEvent));

	DWORD waitResult = WaitForSingleObject(D3D.hEvent, INFINITE);
	if (waitResult != WAIT_OBJECT_0)
	{
		PRINT("Flush and finish wait failed");
		throw E_FAIL;
	}
	VERIFY_SUCCEEDED(D3D.spDevice->GetDeviceRemovedReason());

	VERIFY_SUCCEEDED(D3D.spCA->Reset());
	VERIFY_SUCCEEDED(D3D.spCL->Reset(D3D.spCA, nullptr));
}

//=================================================================================================================================
void Transition(ID3D12GraphicsCommandList* pCL, ID3D12Resource* pResource, D3D12_RESOURCE_STATES StateBefore, D3D12_RESOURCE_STATES StateAfter)
{
	if (StateBefore != StateAfter)
	{
		D3D12_RESOURCE_BARRIER RB = {};
		RB.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		RB.Transition.pResource = pResource;
		RB.Transition.Subresource = 0;
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

	VERIFY_SUCCEEDED(D3D.spDevice->CreateCommittedResource(
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
	D3D12_RESOURCE_STATES CurrentState,
	bool doFlush)
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
	VERIFY_SUCCEEDED(D3D.spDevice->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &BufferDesc,
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
		D3D.spCL->ResourceBarrier(1, &BarrierDesc);
		swap(BarrierDesc.Transition.StateBefore, BarrierDesc.Transition.StateAfter); // ensure StateBefore represents current state
	}

	// Execute upload
	D3D12_SUBRESOURCE_DATA SubResourceData = { pData, static_cast<LONG_PTR>(Size), static_cast<LONG_PTR>(Size) };
	if (Size != UpdateSubresources(D3D.spCL, pResource, *ppStagingResource, 0, 0, 1, &SubResourceData))
	{
		PRINT("UpdateSubresources returns the number of bytes updated, so 0 if nothing was updated");
		throw E_FAIL;
	}
	if (NeedTransition)
	{
		// Transition back to whatever the app had
		D3D.spCL->ResourceBarrier(1, &BarrierDesc);
		swap(BarrierDesc.Transition.StateBefore, BarrierDesc.Transition.StateAfter); // ensure StateBefore represents current state
	}
	if (doFlush == true)
	{
		// Finish Upload
		FlushAndFinish(D3D);
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
	UploadData(D3D, *ppResource, pInitialData, SizeInBytes, ppStagingResource, D3D12_RESOURCE_STATE_COMMON, doFlush);
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

	VERIFY_SUCCEEDED(D3D.spDevice->CreatePipelineState(&StreamDesc, IID_PPV_ARGS(ppProgram)));
	if (ppRootSig)
	{
		VERIFY_SUCCEEDED(D3D.spDevice->CreateRootSignature(0, spCode->GetBufferPointer(), spCode->GetBufferSize(), IID_PPV_ARGS(ppRootSig)));
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