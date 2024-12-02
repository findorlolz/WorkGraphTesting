//=================================================================================================================================
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//=================================================================================================================================

// ================================================================================================================================
// D3D12 Hello Work Graphs shaders
// 
// Defines a work graph that is a chain of 3 nodes with different launch modes.
// The nodes log output to a UAV that's just an array of uints.
// 
// The C++ code seeds the graph with 4 records "entryRecord".
// The second node writes to location UAV[entryRecordIndex] and the third node writes to location UAV[4 + entryNodeIndex],
// so 8 uints modified in total.  These are printed to the console by the calling C++ code.  A simple tweak you can do
// in the C++ code is make it log more uints to the console if you want to play around with making the graph do more.
// 
// The app asks D3D to autopopulate the graph based on all nodes available, so you can play around with adding
// and changing nodes without having to change the C++ code, unless you want to tweak how the graph is seeded or how
// results are printed to console.
// 
// ================================================================================================================================
GlobalRootSignature globalRS = 
{
    "DescriptorTable( UAV( u0 ) )"
};
RWTexture2D<float4> UAV : register(u0);

struct entryRecord
{
    uint3 gridSize : SV_DispatchGrid;
    uint recordIndex;
};

#if 0
struct secondNodeInput
{
    uint entryRecordIndex;
    uint incrementValue;
};

struct thirdNodeInput
{
    uint entryRecordIndex;
};
#endif

static const uint c_numEntryRecords = 1;


[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(256,256,1)]
[NumThreads(32,32,1)]
void firstNode(
    DispatchNodeInputRecord<entryRecord> inputData,
#if 0
    [MaxRecords(2)] NodeOutput<secondNodeInput> secondNode,
#endif
    uint threadIndex : SV_GroupIndex,
    uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 i = dispatchThreadID.xy;
    float4 r = float4(1.0f, .0f, .0f, 1.0f);
    UAV[i] = r;
#if 0
    // Methods for allocating output records must be called at thread group scope (uniform call across the group)
    // Allocations can be per thread as well: GetThreadNodeOutputRecords(...), but the call still has to be
    // group uniform albeit with thread-varying arguments.  Thread execution doesn't have to be synchronized (Barrier call not needed).
    GroupNodeOutputRecords<secondNodeInput> outRecs =
        secondNode.GetGroupNodeOutputRecords(2);

    // In a future language version, "->" will be available instead of ".Get()" below to access record members
    outRecs[threadIndex].entryRecordIndex = inputData.Get().recordIndex; // inputData is constant for all threads in a dispatch grid, 
                                                                         // broadcast from input record
    outRecs[threadIndex].incrementValue = dispatchThreadID*2 + threadIndex + 1; // tell consumer how much to increment UAV[entryRecordIndex]
    outRecs.OutputComplete(); // Call must be group uniform.  Thread execution doesn't have to be synchronized (Barrier call not needed).
#endif
}

// --------------------------------------------------------------------------------------------------------------------------------
// secondNode is thread launch, so one thread per input record.
// 
// Logs to the UAV and then sends a task to thirdNode
// --------------------------------------------------------------------------------------------------------------------------------
#if 0
[Shader("node")]
[NodeLaunch("thread")]
void secondNode(
    ThreadNodeInputRecord<secondNodeInput> inputData,
    [MaxRecords(1)] NodeOutput<thirdNodeInput> thirdNode)
{
    // In a future language version, "->" will be available instead of ".Get()" to access record members

    // UAV[entryRecordIndex] (as uint) is the sum of all outputs from upstream node for graph entry [entryRecordIndex]
    InterlockedAdd(UAV[inputData.Get().entryRecordIndex], inputData.Get().incrementValue);

    // For every thread send a task to thirdNode
    ThreadNodeOutputRecords<thirdNodeInput> outRec = thirdNode.GetThreadNodeOutputRecords(1);
    outRec.Get().entryRecordIndex = inputData.Get().entryRecordIndex;
    outRec.OutputComplete();
}

groupshared uint g_sum[c_numEntryRecords];

// --------------------------------------------------------------------------------------------------------------------------------
// thirdNode is coalescing launch, so thread groups are launched with up to 32 records as input in an array.
// 
// The thread group size happens to match this max input array size of 32, but doesn't have to.
// --------------------------------------------------------------------------------------------------------------------------------
[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(32,1,1)]
void thirdNode(
    [MaxRecords(32)] GroupNodeInputRecords<thirdNodeInput> inputData,
    uint threadIndex : SV_GroupIndex)
{
    // Check how many records we got
    // It could be less than the max declared if the system doesn't have that many
    // work items left, or if it doesn't want to wait for more records to arrive at this node before
    // flushing the current work.
    if (threadIndex >= inputData.Count())
        return;
         
    for (uint i = 0; i < c_numEntryRecords; i++)
    {
        g_sum[i] = 0;
    }

    // New way to do barriers by parameter.
    // This instance is like GroupMemoryBarrierWithGroupSync();
    Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE|GROUP_SYNC);

    InterlockedAdd(g_sum[inputData[threadIndex].entryRecordIndex],1);

    Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE|GROUP_SYNC);

    if (threadIndex > 0)
        return;

    for (uint l = 0; l < c_numEntryRecords; l++)
    {
        uint recordIndex = c_numEntryRecords + l;
        InterlockedAdd(UAV[recordIndex],g_sum[l]);
    }
}
#endif