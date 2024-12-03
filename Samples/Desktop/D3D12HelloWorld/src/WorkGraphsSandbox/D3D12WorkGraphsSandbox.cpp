//=================================================================================================================================
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//=================================================================================================================================

//=================================================================================================================================
//
// D3D12 Work Graph Sandbox
//
// This toy app reads a work graph defined in D3D12WorkGraphsSandbox.hlsl (or any filename provided as an argument), and executes it.
//
//=================================================================================================================================

#include "dx12_helpers.h"
#include "image_loading.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h" 
namespace 
{
    D3DContext* gD3DContext = nullptr;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

class WorkGraphContext
{
public:
	void Init(D3DContext& D3D, LPCWSTR pWorkGraphName)
	{
		CComPtr<ID3D12StateObjectProperties1> spSOProps;
		spSOProps = state_object;
		hWorkGraph = spSOProps->GetProgramIdentifier(pWorkGraphName);
		CComPtr<ID3D12WorkGraphProperties> spWGProps;
		spWGProps = state_object;
		UINT WorkGraphIndex = spWGProps->GetWorkGraphIndex(pWorkGraphName);
        spWGProps->GetWorkGraphMemoryRequirements(WorkGraphIndex, &MemReqs);
		BackingMemory.SizeInBytes = MemReqs.MaxSizeInBytes;
		MakeBuffer(D3D, &backing_memory, BackingMemory.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		BackingMemory.StartAddress = backing_memory->GetGPUVirtualAddress();
	}

	ID3D12Resource* backing_memory = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS_RANGE BackingMemory = {};
	D3D12_PROGRAM_IDENTIFIER hWorkGraph = {};
	D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS MemReqs = {};

    CComPtr<ID3D12StateObject> state_object;
	ID3D12RootSignature* root_signature = nullptr;
};

void initialize_work_graph(D3DContext& D3D, WorkGraphContext& wg_context, ID3DBlob* library)
{
    PRINT(">>> Creating work graph...\n");
	{
		CD3DX12_STATE_OBJECT_DESC SO(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);
		auto pLib = SO.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
		CD3DX12_SHADER_BYTECODE libCode(library);
		pLib->SetDXILLibrary(&libCode);
		VERIFY_SUCCEEDED(D3D.device->CreateRootSignatureFromSubobjectInLibrary(
			0, libCode.pShaderBytecode, libCode.BytecodeLength, L"globalRS", IID_PPV_ARGS(&wg_context.root_signature)));

		auto pWG = SO.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
		pWG->IncludeAllAvailableNodes(); // Auto populate the graph
		LPCWSTR workGraphName = L"HelloWorkGraphs";
		pWG->SetProgramName(workGraphName);

		VERIFY_SUCCEEDED(D3D.device->CreateStateObject(SO, IID_PPV_ARGS(&wg_context.state_object)));
        wg_context.Init(D3D, workGraphName);
	}
}

void run_work_graph(D3DContext& D3D, WorkGraphContext& wg_context, image_data const& result, image_data const& input)
{
    Transition(D3D.command_list, result.texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D.command_list->SetDescriptorHeaps(1, &D3D.srv_desc_heap);
    const float clear_color[4] = { 1.0f, .0f, .0f, 1.0f };
    D3D.command_list->ClearUnorderedAccessViewFloat(result.uav_gpu_handle, result.clear_cpu_handle, result.texture, clear_color, 0, nullptr);

	D3D.command_list->SetComputeRootSignature(wg_context.root_signature);
    D3D.command_list->SetComputeRootDescriptorTable(0, result.uav_gpu_handle);
    D3D.command_list->SetComputeRootDescriptorTable(1, input.srv_gpu_handle);

	D3D12_SET_PROGRAM_DESC setProg = {};
	setProg.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
	setProg.WorkGraph.ProgramIdentifier = wg_context.hWorkGraph;
	setProg.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
	setProg.WorkGraph.BackingMemory = wg_context.BackingMemory;
	D3D.command_list->SetProgram(&setProg);

	struct entryRecord
	{
		UINT gridSize[3];
		UINT recordIndex;
	};
	vector<entryRecord> inputData;
	UINT numRecords = 1;
	inputData.resize(numRecords);
	for (UINT recordIndex = 0; recordIndex < numRecords; recordIndex++)
	{
		inputData[recordIndex].gridSize[0] = result.width / 16;
        inputData[recordIndex].gridSize[1] = result.height / 16;
        inputData[recordIndex].gridSize[2] = 1u;
		inputData[recordIndex].recordIndex = recordIndex;
	}

	D3D12_DISPATCH_GRAPH_DESC DSDesc = {};
	DSDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
	DSDesc.NodeCPUInput.EntrypointIndex = 0;
	DSDesc.NodeCPUInput.NumRecords = numRecords;
	DSDesc.NodeCPUInput.RecordStrideInBytes = sizeof(entryRecord);
	DSDesc.NodeCPUInput.pRecords = inputData.data();
	D3D.command_list->DispatchGraph(&DSDesc);

    Transition(D3D.command_list, result.texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

int main(int, char**)
{
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX12 Example", WS_OVERLAPPEDWINDOW, 100, 100, 1920, 1080, nullptr, nullptr, wc.hInstance, nullptr);

	const char* pFile = g_File;
	const size_t cSize = strlen(pFile) + 1;
	g_wFile.resize(cSize);
	size_t converted = 0;
	mbstowcs_s(&converted, &g_wFile[0], cSize, pFile, cSize);
	if (converted != cSize)
	{
		PRINT("Failed to convert filename to WCHAR.");
		return -1;
	}
	PRINT(">>> Compiling library...\n");
	CComPtr<ID3DBlob> library;
	VERIFY_SUCCEEDED(CompileDxilLibraryFromFile(g_wFile.c_str(), L"lib_6_8", nullptr, 0, &library));

	PRINT(">>> Device init...\n");
	D3DContext D3D;
	InitDeviceAndContext(D3D, hwnd);
    gD3DContext = &D3D;

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(hwnd);

	ImGui_ImplDX12_InitInfo init_info = {};
	init_info.Device = D3D.device;
	init_info.CommandQueue = D3D.command_queue;
	init_info.NumFramesInFlight = APP_NUM_FRAMES_IN_FLIGHT;
	init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	init_info.SrvDescriptorHeap = D3D.srv_desc_heap;
	init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) { return gD3DContext->srv_desc_heap_alloc.Alloc(out_cpu_handle, out_gpu_handle); };
	init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) { return gD3DContext->srv_desc_heap_alloc.Free(cpu_handle, gpu_handle); };
	ImGui_ImplDX12_Init(&init_info);

	// Our state
	bool show_demo_window = true;
	bool show_another_window = false;
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    image_data image;
    {
		D3D.srv_desc_heap_alloc.Alloc(&image.srv_cpu_handle, &image.srv_gpu_handle);
		bool ret = LoadTextureFromFile("data/albert.jpg", D3D.device, image.srv_cpu_handle, &image.texture, &image.width, &image.height,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    WorkGraphContext wg_context;
    initialize_work_graph(D3D, wg_context, library);

    image_data result;
    {
		result.width = image.width;
		result.height = image.height;
        auto flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D.srv_desc_heap_alloc.Alloc(&result.srv_cpu_handle, &result.srv_gpu_handle);
        D3D.srv_desc_heap_alloc.Alloc(&result.uav_cpu_handle, &result.uav_gpu_handle);
		D3D.clear_desc_heap_alloc.Alloc(&result.clear_cpu_handle, &result.clear_gpu_handle);
        MakeTextureSRVAndUAV(D3D, &result.texture, result.width, result.height, DXGI_FORMAT_R16G16B16A16_FLOAT, flags, 
            result.srv_cpu_handle, result.uav_cpu_handle, result.clear_cpu_handle);
    }

	// Main loop
	bool done = false;
    while (!done)
    {
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;

		if (D3D.SwapChainOccluded && D3D.swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
		{
			::Sleep(10);
			continue;
		}
        D3D.SwapChainOccluded = false;

		// Start the Dear ImGui frame
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		FrameContext* frameCtx = WaitForNextFrameResources(D3D);
		UINT backBufferIdx = D3D.swapchain->GetCurrentBackBufferIndex();
		frameCtx->CommandAllocator->Reset();

        D3D.command_list->Reset(frameCtx->CommandAllocator, nullptr);
        run_work_graph(D3D, wg_context, result, image);
        {
			ImGui::Begin("DirectX12 Work Graph Test");
			ImGui::Image((ImTextureID)result.srv_gpu_handle.ptr, ImVec2((float)result.width, (float)result.height));
			ImGui::End();
		}
		ImGui::Render();

        Transition(D3D.command_list, D3D.main_render_target_resource[backBufferIdx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

		// Render Dear ImGui graphics
		const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
		D3D.command_list->ClearRenderTargetView(D3D.mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
		D3D.command_list->OMSetRenderTargets(1, &D3D.mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
		D3D.command_list->SetDescriptorHeaps(1, &D3D.srv_desc_heap);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), D3D.command_list);

        Transition(D3D.command_list, D3D.main_render_target_resource[backBufferIdx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		D3D.command_list->Close();

        D3D.command_queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&D3D.command_list);

		// Present
		HRESULT hr = D3D.swapchain->Present(1, 0);   // Present with vsync
		//HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
		D3D.SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

		UINT64 fenceValue = D3D.FenceValue + 1;
		D3D.command_queue->Signal(D3D.fence, fenceValue);
        D3D.FenceValue = fenceValue;
		frameCtx->FenceValue = fenceValue;
    }

    WaitForLastSubmittedFrame(D3D);

	// Cleanup
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

    CleanDeviceAndContext(D3D);
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (gD3DContext->device != nullptr && wParam != SIZE_MINIMIZED)
		{
			WaitForLastSubmittedFrame(*gD3DContext);
			CleanupRenderTarget(*gD3DContext);
			HRESULT result = gD3DContext->swapchain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
			assert(SUCCEEDED(result) && "Failed to resize swapchain.");
			CreateRenderTarget(*gD3DContext);
		}
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}