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

class WorkGraphContext
{
public:
    void Init(D3DContext& D3D, CComPtr<ID3D12StateObject> pSO, LPCWSTR pWorkGraphName)
    {
        CComPtr<ID3D12StateObjectProperties1> spSOProps;
        spSOProps = pSO;
        hWorkGraph = spSOProps->GetProgramIdentifier(pWorkGraphName);
        spWGProps = pSO;
        WorkGraphIndex = spWGProps->GetWorkGraphIndex(pWorkGraphName);

        spWGProps->GetWorkGraphMemoryRequirements(WorkGraphIndex, &MemReqs);
        BackingMemory.SizeInBytes = MemReqs.MaxSizeInBytes;
        MakeBuffer(D3D, &spBackingMemory, BackingMemory.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        BackingMemory.StartAddress = spBackingMemory->GetGPUVirtualAddress();
        NumEntrypoints = spWGProps->GetNumEntrypoints(WorkGraphIndex);
        NumNodes = spWGProps->GetNumNodes(WorkGraphIndex);

        for (UINT i = 0; i < NumNodes; i++)
        {
            UINT LRATIndex = spWGProps->GetNodeLocalRootArgumentsTableIndex(WorkGraphIndex, i);
            if (LRATIndex != -1)
            {
                MaxLocalRootArgumentsTableIndex = max((int)LRATIndex, MaxLocalRootArgumentsTableIndex);
            }
        }

        if (MaxLocalRootArgumentsTableIndex >= 0)
        {
            UINT NumTableEntries = MaxLocalRootArgumentsTableIndex + 1;
            LocalRootArgumentsTable.SizeInBytes = NumTableEntries * sizeof(UINT);
            LocalRootArgsData.resize(NumTableEntries);
            for (auto& arg : LocalRootArgsData)
            {
                arg = -1; // setting unused entries to -1
            }
            for (UINT i = 0; i < NumNodes; i++)
            {
                UINT LRATIndex = spWGProps->GetNodeLocalRootArgumentsTableIndex(WorkGraphIndex, i);
                if (LRATIndex != -1)
                {
                    LocalRootArgsData[LRATIndex] = LRATIndex; // setting used entries to just store the table index
                }
            }
            MakeBufferAndInitialize(D3D, &spLocalRootArgumentsTable, LocalRootArgsData.data(),
                                    LocalRootArgumentsTable.SizeInBytes);
            LocalRootArgumentsTable.StartAddress = spLocalRootArgumentsTable->GetGPUVirtualAddress();
            LocalRootArgumentsTable.StrideInBytes = sizeof(UINT);
        }
    }
    CComPtr<ID3D12WorkGraphProperties> spWGProps;
    CComPtr<ID3D12Resource> spBackingMemory;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE BackingMemory = {};
    vector<UINT> LocalRootArgsData;
    CComPtr<ID3D12Resource> spLocalRootArgumentsTable;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE LocalRootArgumentsTable = {};
    D3D12_PROGRAM_IDENTIFIER hWorkGraph = {};
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS MemReqs = {};
    int MaxLocalRootArgumentsTableIndex = -1;
    UINT NumEntrypoints = 0;
    UINT NumNodes = 0;
    UINT WorkGraphIndex = 0;
};

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

//=================================================================================================================================
void PrintHelp()
{
    PRINT("Optional arguments: [-c] [hlsl filename]");
    PRINT("If filename not used, the WorkGraphs.hlsl is assumed by default.");
    PRINT("'-c' uses collections (see comments in code).");
}

//=================================================================================================================================
int main(int argc, char* argv[])
{
    try
    {
        PRINT("\n" <<
            "==================================================================================\n" <<
            " D3D12 Work Graphs Sandbox \n" <<
            "==================================================================================");
        const char* pFile = g_File;
        if (argc > 1)
        {
            if (argc == 2)
            {
                if ((strcmp(argv[1], "-?") == 0) || (strcmp(argv[1], "/?") == 0))
                {
                    PrintHelp();
                    return 0;
                }
                else if ((strcmp(argv[1], "-c") == 0) || (strcmp(argv[1], "/c") == 0))
                {
                    g_bUseCollections = true;
                }
                else
                {
                    // assume file name
                    pFile = argv[1];
                }
            }
            else if (argc == 3)
            {
                for (INT i = 1; i < argc; i++)
                {
                    if ((strcmp(argv[i], "-c") == 0) || (strcmp(argv[i], "/c") == 0))
                    {
                        g_bUseCollections = true;
                    }
                    else
                    {
                        pFile = argv[i];
                    }
                }
                if (!g_bUseCollections)
                {
                    PRINT("ERROR: Bad arguments.");
                    PrintHelp();
                    return 0;
                }
            }
            else
            {
                PrintHelp();
                return 0;
            }
        }

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
        InitDeviceAndContext(D3D);

        D3D12_FEATURE_DATA_D3D12_OPTIONS21 Options;
        VERIFY_SUCCEEDED(D3D.spDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &Options, sizeof(Options)));
        if (Options.WorkGraphsTier == D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED)
        {
            PRINT("Device does not report support for work graphs.");
            return -1;
        }

        // Initialize GPU buffers
        const UINT bufSizeInUints = 16777216;
        const UINT bufSize = bufSizeInUints * sizeof(UINT);
        CComPtr<ID3D12Resource> spGPUBuffer;
        vector<UINT> initialData(bufSizeInUints, 0);
        MakeBufferAndInitialize(D3D, &spGPUBuffer, initialData.data(), bufSize, nullptr, true,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CComPtr<ID3D12Resource> spReadbackBuffer;
        MakeBuffer(D3D, &spReadbackBuffer, bufSize, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_READBACK);

        // Timestamp query heap
        CComPtr<ID3D12QueryHeap> spQueryHeap;
        D3D12_QUERY_HEAP_DESC QueryHeapDesc = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
        VERIFY_SUCCEEDED(D3D.spDevice->CreateQueryHeap(&QueryHeapDesc, IID_PPV_ARGS(&spQueryHeap)));
        CComPtr<ID3D12Resource> spQueryReadbackBuffer;
        size_t queryHeapReadbackSize = 2 * sizeof(UINT64);
        MakeBuffer(D3D, &spQueryReadbackBuffer, queryHeapReadbackSize, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_READBACK);

        // Create pipelines
        PRINT(">>> Creating test argument readback shader from " << pFile << "...\n");
        CComPtr<ID3D12PipelineState> spArgsProgram;
        CComPtr<ID3D12RootSignature> spArgsRS;

        // This part isn't work graphs related.  It's using a vanilla compute shader just to read config constants from 
        // the HLSL file.  This way the app's behavior can be tweaked just by editing the hlsl file without recompiling the app.
        DxcDefine define;
        define.Name = L"EXECUTION_PARAMETERS";
        define.Value = L"1";
        D3D.spInfoQueue->SetMuteDebugOutput(true); // Mute unsigned error, not using dxil.dll
        CreateComputeProgram(D3D, g_wFile.c_str(), L"executionParameters", &define, 1, &spArgsProgram, &spArgsRS);
        D3D.spInfoQueue->SetMuteDebugOutput(false);

        // Run args program to grab dispatch and test params
        D3D.spCL->SetComputeRootSignature(spArgsRS);
        D3D.spCL->SetComputeRootUnorderedAccessView(0, spGPUBuffer->GetGPUVirtualAddress());
        D3D.spCL->SetPipelineState(spArgsProgram);
        D3D.spCL->Dispatch(1, 1, 1);
        PRINT(">>> Dispatching \"executionParameters\" compute shader\n    to fetch execution parameters out of " << pFile << ".\n");
        FlushAndFinish(D3D);

        // Readback GPU buffer
        Transition(D3D.spCL, spGPUBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D.spCL->CopyResource(spReadbackBuffer, spGPUBuffer);
        Transition(D3D.spCL, spGPUBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UploadData(D3D, spGPUBuffer, initialData.data(), bufSize, nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

        // Fetch execution parameters
        D3D12_RANGE range = { 0, bufSize };
        UINT* pDataOutput;
        VERIFY_SUCCEEDED(spReadbackBuffer->Map(0, &range, reinterpret_cast<void**>(&pDataOutput)));
        UINT numRecordsPerEntrypoint = pDataOutput[0];
        bool bFeedGraphInputsFromGPUMemory = pDataOutput[1];
        UINT numUintsToPrintToConsole = pDataOutput[2];
        spReadbackBuffer->Unmap(0, nullptr);

        PRINT(">>> Execution parameters:\n");
        PRINT("    NUM_RECORDS_PER_ENTRYPOINT        = " << numRecordsPerEntrypoint);
        PRINT("    FEED_GRAPH_INPUTS_FROM_GPU_MEMORY = " << (bFeedGraphInputsFromGPUMemory ? "true" : "false"));
        PRINT("    NUM_UINTS_TO_PRINT_TO_CONSOLE    = " << numUintsToPrintToConsole);
        PRINT("");

        // Create work graph
        PRINT(">>> Creating work graph...\n");
        CComPtr<ID3D12StateObject> spSO;
        CComPtr<ID3D12StateObject> spCollectionSO;
        CComPtr<ID3D12RootSignature> spRS;
        WorkGraphContext WG;
        {
            PRINT("    Enable collections flag = " << (g_bUseCollections ? "true" : "false"));
            PRINT("");

            CD3DX12_STATE_OBJECT_DESC SO(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);
            CD3DX12_SHADER_BYTECODE libCode(library);
            VERIFY_SUCCEEDED(D3D.spDevice->CreateRootSignatureFromSubobjectInLibrary(
                0, libCode.pShaderBytecode, libCode.BytecodeLength, L"globalRS", IID_PPV_ARGS(&spRS)));
            if (g_bUseCollections)
            {
                // This path illustrates compiling shaders in a collection and then including that
                // in an executable.  For this sample it isn't useful, but in general it can be a way to
                // spread costs by creating collections on different threads.
                // Individual node shaders can be created in collections, but work graph objects
                // must be defined in executables.  It is possible that depending on the node configuration,
                // even compiling nodes separately in collections will still result in driver work when the
                // work graph is created (might not actually save much depending on the hardware and the situation).
                CD3DX12_STATE_OBJECT_DESC SO_COLLECTION(D3D12_STATE_OBJECT_TYPE_COLLECTION);
                auto pCollectionLib = SO_COLLECTION.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
                pCollectionLib->SetDXILLibrary(&libCode);

                D3D.spInfoQueue->ClearStoredMessages();
                HRESULT hrCollection = D3D.spDevice->CreateStateObject(SO_COLLECTION, IID_PPV_ARGS(&spCollectionSO));
                PrintDebugMessagesToConsole(D3D);
                VERIFY_SUCCEEDED(hrCollection);

                auto ExistingCollection = SO.CreateSubobject<CD3DX12_EXISTING_COLLECTION_SUBOBJECT>();
                ExistingCollection->SetExistingCollection(spCollectionSO);
            }
            else
            {
                auto pLib = SO.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
                pLib->SetDXILLibrary(&libCode);
            }

            auto pWG = SO.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
            pWG->IncludeAllAvailableNodes();
            LPCWSTR workGraphName = L"WorkGraphTest";
            pWG->SetProgramName(workGraphName);

            D3D.spInfoQueue->ClearStoredMessages();
            HRESULT hr = D3D.spDevice->CreateStateObject(SO, IID_PPV_ARGS(&spSO));
            PrintDebugMessagesToConsole(D3D);
            VERIFY_SUCCEEDED(hr);

            WG.Init(D3D, spSO, workGraphName);

            UINT workGraphIndex = WG.spWGProps->GetWorkGraphIndex(workGraphName);
            PRINT(">>> Work graph contents:\n");
            PRINT("    NumNodes: " << WG.NumNodes << "\n");
            if (WG.NumNodes)
            {
                for (UINT n = 0; n < WG.NumNodes; n++)
                {
                    D3D12_NODE_ID ID = WG.spWGProps->GetNodeID(workGraphIndex, n);
                    PRINT_NO_NEWLINE("    Node[" << n << "] = ");
                    PrintID(ID);
                    UINT LRATIndex = WG.spWGProps->GetNodeLocalRootArgumentsTableIndex(WG.WorkGraphIndex, n);
                    if (LRATIndex != -1)
                    {
                        PRINT("        NodeLocalRootArgumentsTableIndex = " << LRATIndex << ", value = " << LRATIndex << " (matches index)");
                    }
                    PRINT("");
                }
            }

            PRINT("    NumEntrypoints: " << WG.NumEntrypoints << "\n");
            if (WG.NumEntrypoints)
            {
                for (UINT e = 0; e < WG.NumEntrypoints; e++)
                {
                    D3D12_NODE_ID ID = WG.spWGProps->GetEntrypointID(workGraphIndex, e);
                    PRINT_NO_NEWLINE("    Entrypoint[" << e << "] = ");
                    PrintID(ID);
                    PRINT("");
                }
                PRINT("");
            }
        }

        // Generate graph inputs
        D3D12_DISPATCH_GRAPH_DESC DSDesc = {};
        vector<D3D12_NODE_CPU_INPUT> inputs;
        inputs.resize(WG.NumEntrypoints);
        vector<vector<UINT>> inputData;
        vector<CComPtr<ID3D12Resource>> inputDataFromGPU;
        inputData.resize(WG.NumEntrypoints);
        vector<CComPtr<ID3D12Resource>> pStagingResource;
        if (bFeedGraphInputsFromGPUMemory)
        {
            pStagingResource.resize(WG.NumEntrypoints);
            inputDataFromGPU.resize(WG.NumEntrypoints);
        }
        for (UINT entry = 0; entry < WG.NumEntrypoints; entry++)
        {
            UINT recSizeInBytes = WG.spWGProps->GetEntrypointRecordSizeInBytes(0, entry);
            UINT recSizeInUints = (recSizeInBytes + 3) / 4;
            inputData[entry].resize(numRecordsPerEntrypoint * recSizeInUints);
            for (UINT recordIndex = 0; recordIndex < numRecordsPerEntrypoint; recordIndex++)
            {
                for (UINT offset = 0; offset < recSizeInUints; offset++)
                {
                    auto& record = inputData[entry][recordIndex * recSizeInUints + offset];
                    if ((offset == 0) && (recSizeInBytes>=4))
                    {
                        record = recordIndex; // first uint is incrementing count
                    }
                    else
                    {
                        record = 3;
                    }
                }
            }
            if (bFeedGraphInputsFromGPUMemory)
            {
                size_t bufSize = inputData[entry].size() * sizeof(UINT);
                MakeBufferAndInitialize(D3D, &inputDataFromGPU[entry], inputData[entry].data(), bufSize, &pStagingResource[entry],
                                        false);
            }
            auto& input = inputs[entry];
            input.EntrypointIndex = entry;
            input.NumRecords = numRecordsPerEntrypoint;
            input.RecordStrideInBytes = recSizeInUints * 4;
            input.pRecords = inputData[entry].data();
        }
        CComPtr<ID3D12Resource> multiNodeInputDescsFromGPU;
        CComPtr<ID3D12Resource> rootInputDescFromGPU;
        CComPtr<ID3D12Resource> pStagingResource2;
        CComPtr<ID3D12Resource> pStagingResource3;
        if (WG.NumEntrypoints > 1)
        {
            if (bFeedGraphInputsFromGPUMemory)
            {
                vector<D3D12_NODE_GPU_INPUT> nodeInputDescsInitialData;
                nodeInputDescsInitialData.resize(WG.NumEntrypoints);
                for (UINT entry = 0; entry < WG.NumEntrypoints; entry++)
                {
                    UINT recSizeInBytes = WG.spWGProps->GetEntrypointRecordSizeInBytes(0, entry);
                    UINT recSizeInUints = (recSizeInBytes + 3) / 4;
                    auto& input = nodeInputDescsInitialData[entry];
                    input.EntrypointIndex = entry;
                    input.NumRecords = numRecordsPerEntrypoint;
                    input.Records = { inputDataFromGPU[entry]->GetGPUVirtualAddress(),recSizeInUints * 4 };
                }
                size_t bufSize = nodeInputDescsInitialData.size() * sizeof(D3D12_NODE_GPU_INPUT);
                MakeBufferAndInitialize(D3D, &multiNodeInputDescsFromGPU, nodeInputDescsInitialData.data(), bufSize,
                                        &pStagingResource2, false);
                D3D12_MULTI_NODE_GPU_INPUT MultiNodeGPUInput;
                MultiNodeGPUInput.NumNodeInputs = WG.NumEntrypoints;
                MultiNodeGPUInput.NodeInputs = { multiNodeInputDescsFromGPU->GetGPUVirtualAddress(),sizeof(D3D12_NODE_GPU_INPUT) };
                MakeBufferAndInitialize(D3D, &rootInputDescFromGPU, &MultiNodeGPUInput, sizeof(MultiNodeGPUInput),
                                        &pStagingResource3, false);

                DSDesc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
                DSDesc.MultiNodeGPUInput = rootInputDescFromGPU->GetGPUVirtualAddress();
            }
            else
            {
                DSDesc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT;
                DSDesc.MultiNodeCPUInput.NodeInputStrideInBytes = sizeof(D3D12_NODE_CPU_INPUT);
                DSDesc.MultiNodeCPUInput.NumNodeInputs = (UINT)inputs.size();
                DSDesc.MultiNodeCPUInput.pNodeInputs = inputs.data();
            }
        }
        else
        {
            if (bFeedGraphInputsFromGPUMemory)
            {
                UINT recSizeInBytes = WG.spWGProps->GetEntrypointRecordSizeInBytes(0, 0);
                UINT recSizeInUints = (recSizeInBytes + 3) / 4;
                D3D12_NODE_GPU_INPUT GPUInput;
                GPUInput.EntrypointIndex = inputs[0].EntrypointIndex;
                GPUInput.NumRecords = inputs[0].NumRecords;
                GPUInput.Records = { inputDataFromGPU[0]->GetGPUVirtualAddress(),recSizeInUints * 4 };
                MakeBufferAndInitialize(D3D, &rootInputDescFromGPU, &GPUInput, sizeof(GPUInput),
                                        &pStagingResource2, false);

                DSDesc.Mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
                DSDesc.NodeGPUInput = rootInputDescFromGPU->GetGPUVirtualAddress();
            }
            else
            {
                DSDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
                DSDesc.NodeCPUInput = inputs[0];
            }
        }
        FlushAndFinish(D3D);

        // Spawn work
        D3D.spCL->EndQuery(spQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0);

        D3D.spCL->SetComputeRootSignature(spRS);
        D3D.spCL->SetComputeRootUnorderedAccessView(0, spGPUBuffer->GetGPUVirtualAddress());

        D3D12_SET_PROGRAM_DESC setProg = {};
        setProg.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        setProg.WorkGraph.ProgramIdentifier = WG.hWorkGraph;
        setProg.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        setProg.WorkGraph.BackingMemory = WG.BackingMemory;
        setProg.WorkGraph.NodeLocalRootArgumentsTable = WG.LocalRootArgumentsTable;
        D3D.spCL->SetProgram(&setProg);

        D3D.spCL->DispatchGraph(&DSDesc);

        D3D.spCL->EndQuery(spQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        D3D.spCL->ResolveQueryData(spQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, spQueryReadbackBuffer, 0);

        PRINT(">>> Dispatching work graph from " << pFile << "...\n");
        FlushAndFinish(D3D);    

        // Readback GPU buffer
        Transition(D3D.spCL, spGPUBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D.spCL->CopyResource(spReadbackBuffer, spGPUBuffer);
        FlushAndFinish(D3D);

        VERIFY_SUCCEEDED(spReadbackBuffer->Map(0, &range, reinterpret_cast<void**>(&pDataOutput)));
        if (numUintsToPrintToConsole)
        {
            PRINT(">>> Dumping first " << numUintsToPrintToConsole << " uints from UAV:\n");
        }
        for (UINT i = 0; i < numUintsToPrintToConsole; i++)
        {
            PRINT("    UAV[" << i << "] = 0x" << hex << pDataOutput[i]);
        }
        spReadbackBuffer->Unmap(0, nullptr);

        D3D12_RANGE queryRange = { 0,queryHeapReadbackSize };
        UINT64* pTimestamps = 0;
        VERIFY_SUCCEEDED(spQueryReadbackBuffer->Map(0, &queryRange, reinterpret_cast<void**>(&pTimestamps)));
        UINT64 freq = 1;
        D3D.spCQ->GetTimestampFrequency(&freq);
        double time = (double)(pTimestamps[1] - pTimestamps[0]) * 1000 / freq;
        PRINT("\n>>> GPU time: " << time << " milliseconds");
        spQueryReadbackBuffer->Unmap(0, nullptr);

        PRINT("\n" <<
            "==================================================================================\n" <<
            " Execution complete\n" <<
            "==================================================================================");
    }
    catch (HRESULT)
    {
        PRINT("Aborting.");
        return -1;
    }
    return 0;
}
