/*
 * SPDX-FileCopyrightText: Copyright (c) 2011-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "core/core.h"
#include "core/locks.h"
#include <rmp2pdefines.h>
#include "gpu/gpu.h"
#include "gpu/subdevice/subdevice.h"
#include "gpu/mem_sys/kern_mem_sys.h"
#include "gpu/mem_mgr/mem_mgr.h"
#include "kernel/mem_mgr/p2p.h"
#include "os/os.h"
#include "mem_mgr/vaspace.h"
#include "gpu/bus/third_party_p2p.h"
#include "gpu/device/device.h"
#include "rmapi/rs_utils.h"
#include "rmapi/rmapi.h"
#include "rmapi/client.h"
#include "rmapi/mapping_list.h"
#include "mem_mgr/mem.h"
#include "mem_mgr/virtual_mem.h"
#include "gpu/mem_mgr/mem_desc.h"
#include "containers/btree.h"
#include "vgpu/rpc.h"
#include "vgpu/vgpu_events.h"
#include "gpu/bus/kern_bus.h"
#include "class/cl503c.h"


static NvBool _isSpaceAvailableForBar1P2PMapping(OBJGPU *, Subdevice *, RsClient *, NvU64);

static
NV_STATUS RmP2PValidateSubDevice
(
    ThirdPartyP2P *pThirdPartyP2P,
    OBJGPU **ppGpu
)
{
    OBJGPU *pGpu = GPU_RES_GET_GPU(pThirdPartyP2P->pSubdevice);
    if (pGpu == NULL)
    {
        return NV_ERR_INVALID_OBJECT_HANDLE;
    }

    API_GPU_FULL_POWER_SANITY_CHECK(pGpu, NV_TRUE, NV_FALSE);

    *ppGpu = pGpu;
    return NV_OK;
}

/*!
 * @brief frees given third party p2p memory extent
 */
static
void _freeMappingExtentInfo
(
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfo
)
{
    if (pExtentInfo == NULL)
        return;

    memdescDestroy(pExtentInfo->pMemDesc);

    portMemFree(pExtentInfo);
}

/*!
 * @brief Constructs a new third party p2p memory extent
 */
static
NV_STATUS _constructMappingExtentInfo
(
    NvU64       address,
    NvU64       offset,
    NvU64       length,
    MEMORY_DESCRIPTOR *pMemDesc,
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO *ppExtentInfo
)
{
    NV_STATUS status;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfo;
    MEMORY_DESCRIPTOR *pNewMemDesc;

    NV_ASSERT_OR_RETURN((ppExtentInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMemDesc != NULL), NV_ERR_INVALID_ARGUMENT);

    *ppExtentInfo = NULL;

    pExtentInfo = portMemAllocNonPaged(
        sizeof(CLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO));
    if (pExtentInfo == NULL)
    {
        status = NV_ERR_NO_MEMORY;
        goto out;
    }

    status = memdescCreateSubMem(&pNewMemDesc, pMemDesc, NULL, offset, length);
    if (status != NV_OK)
    {
        goto out;
    }

    portMemSet(pExtentInfo, 0, sizeof(*pExtentInfo));

    pExtentInfo->address = address;
    pExtentInfo->length = length;
    pExtentInfo->memArea.numRanges = 0;
    pExtentInfo->pMemDesc = pNewMemDesc;
    pExtentInfo->refCount = 1;

    *ppExtentInfo = pExtentInfo;

out:
    if (status != NV_OK)
        _freeMappingExtentInfo(pExtentInfo);

    return status;
}

/*!
 * @brief Creates a new third party p2p memory extent
 */
static
NV_STATUS _createThirdPartyP2PMappingExtent
(
    NvU64       address,
    NvU64       length,
    NvU64       offset,
    RsClient   *pClient,
    PCLI_THIRD_PARTY_P2P_VIDMEM_INFO pVidmemInfo,
    CLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO_LIST *pList,
    MEMORY_DESCRIPTOR *pMemDesc,
    OBJGPU     *pGpu,
    Subdevice  *pSubDevice,
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO *ppExtentInfo,
    NvU64      *pMappingStart,
    NvU64      *pMappingLength
)
{
    MemoryArea memArea;
    NvU64 fbApertureMapLength = RM_ALIGN_UP(length, NVRM_P2P_PAGESIZE_BIG_64K);
    NV_STATUS status = NV_OK;
    KernelBus *pKernelBus;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfoTmp;
    Device *pDevice = GPU_RES_GET_DEVICE(pSubDevice);
    NvBool bGpuLockTaken = (rmDeviceGpuLockIsOwner(gpuGetInstance(pGpu)) ||
                            rmGpuLockIsOwner());

    NV_PRINTF(LEVEL_INFO, "New allocation for address: 0x%llx\n", address);

    NV_ASSERT_OR_RETURN((pDevice != NULL), NV_ERR_INVALID_STATE);
    NV_ASSERT_OR_RETURN((ppExtentInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pList != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMappingStart != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMappingLength != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMemDesc != NULL), NV_ERR_INVALID_ARGUMENT);

    if (IS_VIRTUAL(pGpu))
    {
        VGPU_STATIC_INFO *pVSI = GPU_GET_STATIC_INFO(pGpu);

        if (FLD_TEST_DRF(A080, _CTRL_CMD_VGPU_GET_CONFIG, _PARAMS_VGPU_DEV_CAPS_GPU_DIRECT_RDMA_ENABLED,
                      _FALSE, pVSI->vgpuConfig.vgpuDeviceCapsBits))
        {
            return NV_ERR_NOT_SUPPORTED;
        }
    }

    *ppExtentInfo = NULL;

    pKernelBus = GPU_GET_KERNEL_BUS(pGpu);

    NV_ASSERT_OK_OR_RETURN(_constructMappingExtentInfo(address, offset,
                fbApertureMapLength, pMemDesc, ppExtentInfo));

    if (IS_VIRTUAL(pGpu) && gpuIsWarBug200577889SriovHeavyEnabled(pGpu))
    {
        memArea.numRanges = 1;
        memArea.pRanges = &(*ppExtentInfo)->vgpuRange;
        memArea.pRanges[0].size = fbApertureMapLength;
        NV_RM_RPC_MAP_MEMORY(pGpu, pClient->hClient,
                             RES_GET_HANDLE(pDevice),
                             pVidmemInfo->hMemory,
                             offset,
                             fbApertureMapLength,
                             0,
                             &memArea.pRanges[0].start, status);
        NV_ASSERT_OR_GOTO(status == NV_OK, cleanup);
    }
    else
    {
        if (!bGpuLockTaken)
        {
            NV_ASSERT_OK_OR_GOTO(status, rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE,
                                                                 RM_LOCK_MODULES_P2P), cleanup);
        }

        status = kbusMapFbAperture_HAL(pGpu, pKernelBus,
                                        (*ppExtentInfo)->pMemDesc,
                                        mrangeMake(0, fbApertureMapLength),
                                        &memArea,
                                        BUS_MAP_FB_FLAGS_MAP_UNICAST | BUS_MAP_FB_FLAGS_ALLOW_DISCONTIG,
                                        pDevice);

        if (!bGpuLockTaken)
        {
            rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
        }

        NV_ASSERT_OR_GOTO(status == NV_OK, cleanup);
    }

    (*ppExtentInfo)->memArea = memArea;

    for (pExtentInfoTmp = listHead(pList);
         pExtentInfoTmp != NULL;
         pExtentInfoTmp = listNext(pList, pExtentInfoTmp))
    {
       if (pExtentInfoTmp->address > address)
           break;
    }

    if (pExtentInfoTmp == NULL)
        listAppendExisting(pList, *ppExtentInfo);
    else
        listInsertExisting(pList, pExtentInfoTmp, *ppExtentInfo);

    pSubDevice->P2PfbMappedBytes += fbApertureMapLength;
    *pMappingLength = length;
    *pMappingStart = 0; // starts at zero in the current allocation.

    return NV_OK;
cleanup:
    _freeMappingExtentInfo(*ppExtentInfo);
    return status;
}

/*!
 * @brief Reuse an existing third party p2p allocation.
 *
 *  Determines offset in the current allocation and its size that can
 *  be reused in the new mapping.
 */
static
NV_STATUS _reuseThirdPartyP2PMappingExtent
(
    NvU64       address,
    NvU64       length,
    CLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO_LIST *pList,
    MEMORY_DESCRIPTOR *pMemDesc,
    OBJGPU     *pGpu,
    Subdevice  *pSubDevice,
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO *ppExtentInfo,
    NvU64      *pMappingStart,
    NvU64      *pMappingLength
)
{
    NvU64 mappingStart;
    NvU64 mappingLength;
    NV_STATUS status = NV_OK;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfo  = NULL;

    NV_ASSERT_OR_RETURN((ppExtentInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pList != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMappingStart != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMappingLength != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMemDesc != NULL), NV_ERR_INVALID_ARGUMENT);

    NV_PRINTF(LEVEL_INFO, "Reuse allocation for address: 0x%llx\n", address);

    pExtentInfo = *ppExtentInfo;

    mappingStart = address - pExtentInfo->address;
    mappingLength = NV_MIN((pExtentInfo->length - mappingStart), length);

    *pMappingLength = mappingLength;
    *pMappingStart = mappingStart;

    pExtentInfo->refCount++;

    return status;
}

/*!
 * @brief Frees an existing third party P2P mapping
 *
 *  Iterates over all the p2p allocations that are used in the mapping and
 *  decrements its refcount. If P2p allocation's refcount has reached zero,
 *  it is freed and usage of FB for p2p is appropriately adjusted.
 */
static
NV_STATUS RmThirdPartyP2PMappingFree
(
    RsClient   *pClient,
    OBJGPU     *pGpu,
    PCLI_THIRD_PARTY_P2P_VIDMEM_INFO pVidmemInfo,
    PCLI_THIRD_PARTY_P2P_INFO pThirdPartyP2PInfo,
    Subdevice  *pSubDevice,
    PCLI_THIRD_PARTY_P2P_MAPPING_INFO pMappingInfo
)
{
    NV_STATUS status = NV_OK;
    KernelBus                          *pKernelBus;
    NvU64                               length;
    NvU64                               mappingLength;
    NvU64                               address;
    NvU64                               startOffset;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfo = NULL;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfoNext = NULL;
    Device                             *pDevice = GPU_RES_GET_DEVICE(pSubDevice);
    NvBool                              bGpuLockTaken;
    NvBool                              bVgpuRpc;

    bGpuLockTaken = (rmDeviceGpuLockIsOwner(gpuGetInstance(pGpu)) ||
                     rmGpuLockIsOwner());

    NV_ASSERT_OR_RETURN((pGpu != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMappingInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pSubDevice != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pThirdPartyP2PInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pDevice != NULL), NV_ERR_INVALID_STATE);

    if (IS_VIRTUAL(pGpu))
    {
        VGPU_STATIC_INFO *pVSI = GPU_GET_STATIC_INFO(pGpu);

        if (FLD_TEST_DRF(A080, _CTRL_CMD_VGPU_GET_CONFIG, _PARAMS_VGPU_DEV_CAPS_GPU_DIRECT_RDMA_ENABLED,
                      _FALSE, pVSI->vgpuConfig.vgpuDeviceCapsBits))
        {
            return NV_ERR_NOT_SUPPORTED;
        }
    }

    pKernelBus = GPU_GET_KERNEL_BUS(pGpu);

    length = pMappingInfo->length;
    address = pMappingInfo->address;

    bVgpuRpc = IS_VIRTUAL(pGpu) && gpuIsWarBug200577889SriovHeavyEnabled(pGpu);

    if (!bGpuLockTaken && !bVgpuRpc)
    {
        status = rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE,
                                         RM_LOCK_MODULES_P2P);
        NV_ASSERT_OK_OR_RETURN(status);
    }

    for(pExtentInfo = pMappingInfo->pStart; (pExtentInfo != NULL) && (length != 0);
        pExtentInfo = pExtentInfoNext)
    {
        pExtentInfoNext = listNext(&pVidmemInfo->mappingExtentList, pExtentInfo);
        startOffset = address - pExtentInfo->address;
        mappingLength = NV_MIN(length, (pExtentInfo->length - startOffset));

        address += mappingLength;
        length -= mappingLength;
        pExtentInfo->refCount--;
        if (pExtentInfo->refCount == 0)
        {
            if (bVgpuRpc)
            {
                NV_RM_RPC_UNMAP_MEMORY(pGpu, pClient->hClient,
                                       RES_GET_HANDLE(pDevice),
                                       pVidmemInfo->hMemory,
                                       0,
                                       pExtentInfo->memArea.pRanges[0].start, status);
            }
            else
            {
                status = kbusUnmapFbAperture_HAL(pGpu, pKernelBus,
                                                 pExtentInfo->pMemDesc,
                                                 pExtentInfo->memArea,
                                                 BUS_MAP_FB_FLAGS_MAP_UNICAST);
            }
            NV_ASSERT(status == NV_OK);

            listRemove(&pVidmemInfo->mappingExtentList, pExtentInfo);

            pSubDevice->P2PfbMappedBytes -= pExtentInfo->length;
            _freeMappingExtentInfo(pExtentInfo);
        }
    }

    if (!bGpuLockTaken && !bVgpuRpc)
    {
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    }

    NV_ASSERT(length == 0);

    pMappingInfo->pStart = NULL;
    pMappingInfo->length = 0;

    return status;
}

static void _thirdpartyp2pFillEntries(NvU64 **,NvU32 *, NvU64, MemoryArea, MemoryRange);

static void
_thirdpartyp2pFillEntries
(
    NvU64     **ppPhysicalAddresses,
    NvU32      *pEntries,
    NvU64       physicalFbAddress,
    MemoryArea  memArea,
    MemoryRange memRange
)
{
    NvU64 idx;
    NvU64 idy = *pEntries;
    NvU64 rangeOffset = 0;
    NvU64 lastAddr = mrangeLimit(memRange);
    NvBool bDone = NV_FALSE;

    //
    // TODO: replace this logic when MemoryArea iterators are introduced
    // Initial loop to find which range the starting offset is in
    //
    for (idx = 0; idx < memArea.numRanges; idx++)
    {
        NvU64 size = memArea.pRanges[idx].size;

        // Check if this range contains the starting offset
        if (mrangeContains(mrangeMake(rangeOffset, size), mrangeMake(memRange.start, 1)))
        {
            rangeOffset = memRange.start - rangeOffset;
            break;
        }
        rangeOffset += size;
    }

    // Now we start mapping - start with the idx corresponding to the correct range
    for (; idx < memArea.numRanges && (!bDone); idx++)
    {
        //
        // Add rangeOffset on the first iteration to get the correct offset into
        // the first range. Set to 0 after the first iteration. Get the next mapping
        // offset (into the memArea) and check whether we're already at the last range
        // by checking if current range contains end address.
        //
        NvU64 beginRange = rangeOffset + memArea.pRanges[idx].start;
        NvU64 nextMap = memRange.start + memArea.pRanges[idx].size - rangeOffset;
        NvU64 endRange = mrangeLimit(memArea.pRanges[idx]);

        bDone = nextMap >= lastAddr;
        endRange -= bDone ? (nextMap - lastAddr) : 0;
        rangeOffset = 0;

        // Fill the ppPhysicalAddresses array with pages from the range.
        while (beginRange < endRange)
        {
            (*ppPhysicalAddresses)[idy] = physicalFbAddress + beginRange;
            idy++;
            beginRange += NVRM_P2P_PAGESIZE_BIG_64K;
        }

        // Set the next range starting offset (used for tracking when we need to exit)
        memRange.start = nextMap;
    }

    // Store current total entries.
    *pEntries = idy;
}

/*!
 *  @brief Gets BAR1 mapped pages.
 *
 *  The function creates mappings from BAR1 VASpace for registered third party
 *  P2P allocations, so the pages returned by this function are BAR1 addresses,
 *  BAR1 base + BAR1 VAs returned by RM.
 *  Note that PCLI_THIRD_PARTY_P2P_MAPPING_INFO is also updated to track these
 *  BAR1 addresses in order to reuse them across multiple allocations.
 */
static
NV_STATUS RmThirdPartyP2PBAR1GetPages
(
    NvU64       address,
    NvU64       length,
    NvU64       offset,
    NvBool      bForcePcie,
    RsClient   *pClient,
    PCLI_THIRD_PARTY_P2P_VIDMEM_INFO pVidmemInfo,
    NvU64     **ppPhysicalAddresses,
    NvU32     **ppWreqMbH,
    NvU32     **ppRreqMbH,
    NvU32      *pEntries,
    NvBool     *pbMemCpuCacheable,
    OBJGPU     *pGpu,
    Subdevice  *pSubDevice,
    PCLI_THIRD_PARTY_P2P_MAPPING_INFO pMappingInfo,
    PCLI_THIRD_PARTY_P2P_INFO pThirdPartyP2PInfo
)
{
    NV_STATUS status = NV_OK;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfoLoop = NULL;
    PCLI_THIRD_PARTY_P2P_MAPPING_EXTENT_INFO pExtentInfo     = NULL;
    MEMORY_DESCRIPTOR *pMemDesc;
    KernelBus *pKernelBus;
    NvU64 mappingLength = 0;
    NvU64 mappingOffset = 0;
    NvU64 lengthReq = 0;
    NvBool bFound;
    NvU64 physicalFbAddress;

    NV_ASSERT_OR_RETURN((pGpu != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pMappingInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pSubDevice != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pThirdPartyP2PInfo != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((ppPhysicalAddresses != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pEntries != NULL), NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN((pbMemCpuCacheable != NULL), NV_ERR_INVALID_ARGUMENT);

    pKernelBus = GPU_GET_KERNEL_BUS(pGpu);

    NV_ASSERT_OK_OR_RETURN(kbusGetGpuFbPhysAddressForRdma(pGpu, pKernelBus,
                                               bForcePcie, &physicalFbAddress));

    NV_PRINTF(LEVEL_INFO,
              "Requesting Bar1 mappings for address: 0x%llx, length: 0x%llx, BAR1 base: 0x%llx\n",
              address, length, physicalFbAddress);
    *pEntries = 0;

    pMappingInfo->length = 0;
    pMappingInfo->address = address;

    pExtentInfoLoop = listHead(&pVidmemInfo->mappingExtentList);

    while (length > 0)
    {
        bFound = NV_FALSE;
        lengthReq = length;
        for(; pExtentInfoLoop != NULL; pExtentInfoLoop = listNext(&pVidmemInfo->mappingExtentList, pExtentInfoLoop))
        {
           if ((address >= pExtentInfoLoop->address) &&
               (address <
                (pExtentInfoLoop->address + pExtentInfoLoop->length)))
           {
               bFound = NV_TRUE;
               break;
           }
           else if (address < pExtentInfoLoop->address)
           {
               //
               // create new allocation for addresses that are not overlapping
               // with the next allocation.
               //
               if ((address + length) > pExtentInfoLoop->address)
               {
                   lengthReq = pExtentInfoLoop->address - address;
               }
               break;
           }
        }

        pExtentInfo = pExtentInfoLoop;

        if (!bFound)
        {
            // Check if there is still space in BAR1 to map this length
            if (!_isSpaceAvailableForBar1P2PMapping(pGpu, pSubDevice, pClient, lengthReq))
            {
                NV_PRINTF(LEVEL_ERROR,
                          "no space for BAR1 mappings, length: 0x%llx \n", lengthReq);

                status = NV_ERR_INSUFFICIENT_RESOURCES;
                goto out;
            }

            pMemDesc = pVidmemInfo->pMemDesc;
            status = _createThirdPartyP2PMappingExtent(
                        address, lengthReq, offset, pClient,
                        pVidmemInfo,
                        &pVidmemInfo->mappingExtentList, pMemDesc, pGpu,
                        pSubDevice, &pExtentInfo,
                        &mappingOffset, &mappingLength);
            if (NV_OK != status)
            {
                goto out;
            }
        }
        else
        {
            pMemDesc = pExtentInfo->pMemDesc;
            status = _reuseThirdPartyP2PMappingExtent(
                        address, lengthReq, &pVidmemInfo->mappingExtentList, pMemDesc,
                        pGpu, pSubDevice, &pExtentInfo, &mappingOffset, &mappingLength);
            if (NV_OK != status)
            {
                goto out;
            }
        }

        if (pMappingInfo->pStart == NULL)
            pMappingInfo->pStart = pExtentInfo;

        _thirdpartyp2pFillEntries(ppPhysicalAddresses,
                                  pEntries,
                                  physicalFbAddress,
                                  pExtentInfo->memArea,
                                  mrangeMake(mappingOffset, mappingLength));

        length -= mappingLength;
        pMappingInfo->length += mappingLength;
        address += mappingLength;
        offset += mappingLength;

    }

    if (ppWreqMbH != NULL && ppRreqMbH != NULL)
    {
        portMemSet(*ppWreqMbH, 0, sizeof((*ppWreqMbH)[0]) * (*pEntries));
        portMemSet(*ppRreqMbH, 0, sizeof((*ppRreqMbH)[0]) * (*pEntries));
    }

    // BAR1 mappings are not CPU-cacheable
    *pbMemCpuCacheable = NV_FALSE;

    return NV_OK;

out:
    RmThirdPartyP2PMappingFree(pClient, pGpu, pVidmemInfo, pThirdPartyP2PInfo,
                               pSubDevice, pMappingInfo);
    return status;
}

/*!
 *  @brief Gets pages adjusted by NVLink aperture base (GPAs).
 */
static
NV_STATUS RmThirdPartyP2PNVLinkGetPages
(
    OBJGPU            *pGpu,
    NvU64              address,
    NvU64              length,
    NvU64              offset,
    MEMORY_DESCRIPTOR *pMemDesc,
    NvU32            **ppWreqMbH,
    NvU32            **ppRreqMbH,
    NvU64            **ppPhysicalAddresses,
    NvU32             *pEntries,
    NvBool            *pbMemCpuCacheable
)
{
    NvU64 lastAddress;
    NvU32 entries = 0;
    RmPhysAddr physAddr;
    KernelMemorySystem *pKernelMemorySystem = GPU_GET_KERNEL_MEMORY_SYSTEM(pGpu);

    // On localized allocations over C2C/nvlink mappings, RDMA is not supported
    if (memdescGetFlag(pMemDesc, MEMDESC_FLAGS_ALLOC_AS_LOCALIZED))
    {
        NV_PRINTF(LEVEL_ERROR, "RDMA is not supported for localized memory"
                              " over coherent mappings\n");
        return NV_ERR_NOT_SUPPORTED;
    }

    if (memdescGetPageSize(pMemDesc, AT_CPU) < NVRM_P2P_PAGESIZE_BIG_64K)
    {
        return NV_ERR_INVALID_STATE;
    }

    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(address, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(length, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(offset, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);

    lastAddress = (address + length - 1);
    while (address < lastAddress)
    {
        physAddr = memdescGetPhysAddr(pMemDesc, AT_CPU, offset);

        if ((ppWreqMbH != NULL) && (ppRreqMbH != NULL))
        {
            (*ppWreqMbH)[entries] = 0;
            (*ppRreqMbH)[entries] = 0;
        }

        (*ppPhysicalAddresses)[entries] = pKernelMemorySystem->coherentCpuFbBase + physAddr;

        address += NVRM_P2P_PAGESIZE_BIG_64K;
        offset += NVRM_P2P_PAGESIZE_BIG_64K;
        entries++;
    }

    *pEntries = entries;

    // Mappings over nvlink/c2c are CPU-cacheable
    *pbMemCpuCacheable = NV_TRUE;

    return NV_OK;
}

/*!
 *  @brief Gets pages for the given VidmemInfo
 */
static
NV_STATUS RmP2PGetPagesUsingVidmemInfo
(
    NvU64                             address,
    NvU64                             length,
    NvU64                             offset,
    NvBool                            bForcePcie,
    ThirdPartyP2P                    *pThirdPartyP2P,
    NvU64                           **ppPhysicalAddresses,
    NvU32                           **ppWreqMbH,
    NvU32                           **ppRreqMbH,
    NvU32                            *pEntries,
    NvBool                           *pbMemCpuCacheable,
    void                             *pPlatformData,
    void                            (*pFreeCallback)(void *pData),
    void                             *pData,
    OBJGPU                           *pGpu,
    Subdevice                        *pSubDevice,
    CLI_THIRD_PARTY_P2P_VASPACE_INFO *pVASpaceInfo,
    ThirdPartyP2P                    *pThirdPartyP2PInfo,
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO  *pVidmemInfo
)
{
    NV_STATUS status;
    MEMORY_DESCRIPTOR *pMemDesc;
    RsClient *pClient = RES_GET_CLIENT(pThirdPartyP2P);
    CLI_THIRD_PARTY_P2P_MAPPING_INFO *pMappingInfo = NULL;

    pMemDesc = pVidmemInfo->pMemDesc;

    status = CliGetThirdPartyP2PMappingInfoFromKey(pThirdPartyP2P,
                pVidmemInfo->hMemory, pPlatformData, &pMappingInfo);
    if (status == NV_ERR_OBJECT_NOT_FOUND)
    {
        status = CliAddThirdPartyP2PMappingInfo(pThirdPartyP2P, pVidmemInfo->hMemory,
                pPlatformData, pFreeCallback, pData, &pMappingInfo);
    }
    if (status != NV_OK)
    {
        return status;
    }

    //
    // For coherent platforms supporting BAR1 mappings, the third party object
    // of type CLI_THIRD_PARTY_P2P_TYPE_NVLINK is overloaded to also track BAR1
    // mappings.
    //
    // Object type BAR1 is not supported with forced PCIe mappings and
    // is already sanity-checked at this point.
    //
    if ((!bForcePcie) &&
        (pThirdPartyP2PInfo->type == CLI_THIRD_PARTY_P2P_TYPE_NVLINK))
    {
        status = RmThirdPartyP2PNVLinkGetPages(pGpu, address, length,
                                               offset, pMemDesc, ppWreqMbH,
                                               ppRreqMbH, ppPhysicalAddresses,
                                               pEntries, pbMemCpuCacheable);
    }
    else
    {
        status = RmThirdPartyP2PBAR1GetPages(address, length, offset, bForcePcie,
                                             pClient, pVidmemInfo, ppPhysicalAddresses,
                                             ppWreqMbH, ppRreqMbH,
                                             pEntries, pbMemCpuCacheable,
                                             pGpu, pSubDevice, pMappingInfo,
                                             pThirdPartyP2PInfo);
    }

    return status;
}

/*!
 *  @brief Gets pages or validates address range.
 *
 *  If the argument "ppPhysicalAddresses" is NULL,
 *  the function just validates the address range.
 */
static
NV_STATUS RmP2PValidateAddressRangeOrGetPages
(
    NvU64          address,
    NvU64          length,
    ThirdPartyP2P *pThirdPartyP2P,
    NvU64        **ppPhysicalAddresses,
    NvU32        **ppWreqMbH,
    NvU32        **ppRreqMbH,
    NvU32         *pEntries,
    NvBool        *pbMemCpuCacheable,
    void          *pPlatformData,
    void         (*pFreeCallback)(void *pData),
    void          *pData,
    OBJGPU        *pGpu,
    Subdevice     *pSubDevice,
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO pVASpaceInfo,
    PCLI_THIRD_PARTY_P2P_INFO pThirdPartyP2PInfo
)
{
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO *pVidmemInfo;
    NV_STATUS status;
    NvU64 offset;

    status = CliGetThirdPartyP2PVidmemInfoFromAddress(pThirdPartyP2P,
                address, length, &offset, &pVidmemInfo);
    if (status != NV_OK)
    {
        return status;
    }

    // Range validation is done at this point, so return if only validation was requested
    if (ppPhysicalAddresses == NULL)
    {
        return NV_OK;
    }

    status = RmP2PGetPagesUsingVidmemInfo(address, length, offset, NV_FALSE,
                                          pThirdPartyP2P, ppPhysicalAddresses,
                                          ppWreqMbH, ppRreqMbH,
                                          pEntries, pbMemCpuCacheable,pPlatformData,
                                          pFreeCallback, pData, pGpu, pSubDevice,
                                          pVASpaceInfo, pThirdPartyP2PInfo, pVidmemInfo);
    if (status != NV_OK)
    {
        return status;
    }

    return NV_OK;
}

//
// GeForce / consumer CUDA never sends NV503C REGISTER_VA_SPACE / REGISTER_VIDMEM.
// nvidia_p2p_get_pages() then fails with NV_ERR_OBJECT_NOT_FOUND (0x57) even
// though a ThirdPartyP2P object exists (often the MemoryManager internal one).
// Restore the old GPU-VA reverse lookup as a lazy registration fallback.
//
static NvBool
_rmP2PIsInternalClient(OBJGPU *pGpu, NvHandle hClient)
{
    MemoryManager *pMemoryManager;

    if (pGpu == NULL)
        return NV_FALSE;

    pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    return (pMemoryManager != NULL) && (pMemoryManager->hClient == hClient);
}

static NvBool
_rmP2PIsInternalObject(ThirdPartyP2P *pThirdPartyP2P)
{
    return _rmP2PIsInternalClient(GPU_RES_GET_GPU(pThirdPartyP2P),
                                  pThirdPartyP2P->hClient);
}

static void
_rmP2PDumpThirdPartyP2PInventory(NvU32 pid, NvU64 address, NvU64 length)
{
    RS_SHARE_ITERATOR it = serverutilShareIter(classId(P2PTokenShare));
    NvU32 count = 0;

    NV_PRINTF(LEVEL_ERROR,
              "GDRP2P inventory pid=%u address=0x%llx length=0x%llx\n",
              pid, address, length);

    while (serverutilShareIterNext(&it))
    {
        P2PTokenShare *pShare = dynamicCast(it.pShared, P2PTokenShare);
        ThirdPartyP2P *pThirdPartyP2P;
        OBJGPU *pGpu;
        NvU32 vasCount = 0;
        NvU32 vidCount = 0;
        CLI_THIRD_PARTY_P2P_VASPACE_INFO_MAPIter vasIt;
        CLI_THIRD_PARTY_P2P_VIDMEM_INFO_MAPIter vidIt;

        if (pShare == NULL)
            continue;

        pThirdPartyP2P = pShare->pThirdPartyP2P;
        if (pThirdPartyP2P == NULL)
            continue;

        pGpu = GPU_RES_GET_GPU(pThirdPartyP2P);
        vasIt = mapIterAll(&pThirdPartyP2P->vaSpaceInfoMap);
        while (mapIterNext(&vasIt))
            vasCount++;
        vidIt = mapIterAll(&pThirdPartyP2P->vidmemInfoMap);
        while (mapIterNext(&vidIt))
            vidCount++;

        NV_PRINTF(LEVEL_ERROR,
                  "GDRP2P  tpp[%u] hClient=0x%x hTPP=0x%x type=%u token=0x%llx "
                  "internal=%u pidMatch=%u vas=%u vidmem=%u gpu=%u\n",
                  count,
                  pThirdPartyP2P->hClient,
                  pThirdPartyP2P->hThirdPartyP2P,
                  pThirdPartyP2P->type,
                  pThirdPartyP2P->p2pToken,
                  _rmP2PIsInternalObject(pThirdPartyP2P),
                  thirdpartyp2pIsValidClientPid(pThirdPartyP2P, pid, 0),
                  vasCount,
                  vidCount,
                  (pGpu != NULL) ? gpuGetInstance(pGpu) : 0xffffffffU);
        count++;
    }

    NV_PRINTF(LEVEL_ERROR, "GDRP2P inventory count=%u\n", count);
}

static void
_rmP2PDumpUserFbAllocs(NvU32 pid, OBJGPU *pGpu, NvU64 length)
{
    RmClient **ppClient;

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient *pRmClient = *ppClient;
        RsClient *pClient = staticCast(pRmClient, RsClient);
        Device *pDevice = NULL;
        NODE *pNode;

        if (pRmClient->ProcID != pid)
            continue;

        if (pGpu != NULL)
        {
            if (_rmP2PIsInternalClient(pGpu, pClient->hClient))
                continue;
            if (deviceGetByGpu(pClient, pGpu, NV_TRUE, &pDevice) != NV_OK)
                continue;
        }
        else
        {
            RS_ITERATOR it = clientRefIter(pClient, NULL, classId(Device),
                                           RS_ITERATE_CHILDREN, NV_TRUE);
            if (!clientRefIterNext(pClient, &it))
                continue;
            pDevice = dynamicCast(it.pResourceRef->pResource, Device);
        }

        if (pDevice == NULL)
            continue;

        btreeEnumStart(0, &pNode, pDevice->DevMemoryTable);
        while (pNode != NULL)
        {
            Memory *pMemory = pNode->Data;
            NvU64 memSize;

            btreeEnumNext(&pNode, pDevice->DevMemoryTable);
            if ((pMemory == NULL) || (pMemory->pMemDesc == NULL))
                continue;
            if (dynamicCast(pMemory, VirtualMemory) != NULL)
                continue;
            if (memdescGetAddressSpace(pMemory->pMemDesc) != ADDR_FBMEM)
                continue;

            memSize = memdescGetSize(pMemory->pMemDesc);
            NV_PRINTF(LEVEL_ERROR,
                      "GDRP2P  fbmem hClient=0x%x hMemory=0x%x size=0x%llx need=0x%llx exact=%u\n",
                      pClient->hClient,
                      RES_GET_HANDLE(pMemory),
                      memSize,
                      length,
                      (memSize == length));
        }
    }
}

static Memory *
_rmP2PFindMemoryForMemDesc(Device *pDevice, MEMORY_DESCRIPTOR *pTarget)
{
    MEMORY_DESCRIPTOR *pRoot;
    NODE *pNode;

    if ((pDevice == NULL) || (pTarget == NULL))
        return NULL;

    pRoot = memdescGetRootMemDesc(pTarget, NULL);

    btreeEnumStart(0, &pNode, pDevice->DevMemoryTable);
    while (pNode != NULL)
    {
        Memory *pMemory = pNode->Data;

        btreeEnumNext(&pNode, pDevice->DevMemoryTable);
        if ((pMemory == NULL) || (pMemory->pMemDesc == NULL))
            continue;
        if (dynamicCast(pMemory, VirtualMemory) != NULL)
            continue;
        if ((pMemory->pMemDesc == pTarget) || (pMemory->pMemDesc == pRoot))
            return pMemory;
        if (memdescGetRootMemDesc(pMemory->pMemDesc, NULL) == pRoot)
            return pMemory;
    }

    return NULL;
}

static NV_STATUS
_rmP2PFindUserVidmemForAddress
(
    OBJGPU    *pGpu,
    NvU64      address,
    NvU64      length,
    RsClient **ppMemClient,
    Memory   **ppMemory,
    NvU64     *pOffset,
    NvHandle  *phVASpace
)
{
    RmClient **ppClient;
    NvU32 pid = osGetCurrentProcess();
    Memory *pExact = NULL;
    RsClient *pExactClient = NULL;
    NvU32 exactCount = 0;
    Memory *pBest = NULL;
    RsClient *pBestClient = NULL;
    NvU64 bestSize = ~((NvU64)0);
    NvU32 bestCount = 0;

    NV_ASSERT_OR_RETURN(ppMemClient != NULL, NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(ppMemory != NULL, NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(pOffset != NULL, NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(phVASpace != NULL, NV_ERR_INVALID_ARGUMENT);

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient *pRmClient = *ppClient;
        RsClient *pClient = staticCast(pRmClient, RsClient);
        Device *pDevice = NULL;
        NODE *pNode;

        if (pRmClient->ProcID != pid)
            continue;

        if (pGpu != NULL)
        {
            if (_rmP2PIsInternalClient(pGpu, pClient->hClient))
                continue;
            if (deviceGetByGpu(pClient, pGpu, NV_TRUE, &pDevice) != NV_OK)
                continue;
        }
        else
        {
            RS_ITERATOR it = clientRefIter(pClient, NULL, classId(Device),
                                           RS_ITERATE_CHILDREN, NV_TRUE);
            if (!clientRefIterNext(pClient, &it))
                continue;
            pDevice = dynamicCast(it.pResourceRef->pResource, Device);
        }

        if (pDevice == NULL)
            continue;

        btreeEnumStart(0, &pNode, pDevice->DevMemoryTable);
        while (pNode != NULL)
        {
            Memory *pMemory = pNode->Data;
            VirtualMemory *pVirt;
            NODE *pMapNode;

            btreeEnumNext(&pNode, pDevice->DevMemoryTable);
            if (pMemory == NULL)
                continue;

            pVirt = dynamicCast(pMemory, VirtualMemory);
            if ((pVirt == NULL) || (pVirt->pDmaMappingList == NULL))
                continue;

            if (btreeSearch(address, &pMapNode, pVirt->pDmaMappingList) == NV_OK)
            {
                CLI_DMA_MAPPING_INFO *pDma = pMapNode->Data;
                Memory *pPhysMem;
                NvU64 rootOffset = 0;
                NvU64 mapVa;
                NvU64 offset;

                if ((pDma == NULL) || (pDma->pMemDesc == NULL))
                    continue;
                if (memdescGetAddressSpace(pDma->pMemDesc) != ADDR_FBMEM)
                    continue;

                mapVa = (pDma->DmaOffset != 0) ? pDma->DmaOffset : pMapNode->keyStart;
                if (address < mapVa)
                    continue;

                memdescGetRootMemDesc(pDma->pMemDesc, &rootOffset);
                offset = rootOffset + (address - mapVa);

                pPhysMem = _rmP2PFindMemoryForMemDesc(pDevice, pDma->pMemDesc);
                if ((pPhysMem == NULL) || (pPhysMem->pMemDesc == NULL))
                    continue;
                if ((offset + length) > memdescGetSize(pPhysMem->pMemDesc))
                    continue;

                NV_PRINTF(LEVEL_ERROR,
                          "GDRP2P matched DMA map va=0x%llx dmaOff=0x%llx hVASpace=0x%x hMemory=0x%x\n",
                          address, mapVa, pVirt->hVASpace, RES_GET_HANDLE(pPhysMem));

                *ppMemClient = pClient;
                *ppMemory = pPhysMem;
                *pOffset = offset;
                *phVASpace = pVirt->hVASpace;
                return NV_OK;
            }
        }

        btreeEnumStart(0, &pNode, pDevice->DevMemoryTable);
        while (pNode != NULL)
        {
            Memory *pMemory = pNode->Data;
            NvU64 memSize;

            btreeEnumNext(&pNode, pDevice->DevMemoryTable);
            if ((pMemory == NULL) || (pMemory->pMemDesc == NULL))
                continue;
            if (dynamicCast(pMemory, VirtualMemory) != NULL)
                continue;
            if (memdescGetAddressSpace(pMemory->pMemDesc) != ADDR_FBMEM)
                continue;

            memSize = memdescGetSize(pMemory->pMemDesc);
            if (memSize == length)
            {
                exactCount++;
                pExact = pMemory;
                pExactClient = pClient;
            }
            if (memSize >= length)
            {
                if (memSize < bestSize)
                {
                    bestSize = memSize;
                    pBest = pMemory;
                    pBestClient = pClient;
                    bestCount = 1;
                }
                else if (memSize == bestSize)
                {
                    bestCount++;
                }
            }
        }
    }

    if (exactCount == 1)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "GDRP2P matched unique FB size=0x%llx hMemory=0x%x\n",
                  length, RES_GET_HANDLE(pExact));
        *ppMemClient = pExactClient;
        *ppMemory = pExact;
        *pOffset = 0;
        *phVASpace = 0;
        return NV_OK;
    }

    if (bestCount == 1)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "GDRP2P matched unique min FB size=0x%llx (>=0x%llx) hMemory=0x%x\n",
                  bestSize, length, RES_GET_HANDLE(pBest));
        *ppMemClient = pBestClient;
        *ppMemory = pBest;
        *pOffset = 0;
        *phVASpace = 0;
        return NV_OK;
    }

    NV_PRINTF(LEVEL_ERROR,
              "GDRP2P no RM DMA mapping for va=0x%llx; exact=%u min-ge=%u\n",
              address, exactCount, bestCount);
    return NV_ERR_OBJECT_NOT_FOUND;
}

static NV_STATUS
_rmP2PEnsureVaSpace(ThirdPartyP2P *pThirdPartyP2P, NvHandle hVASpace)
{
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO pInfo = NULL;
    NvU32 token = 0;
    NV_STATUS status;

    if (thirdpartyp2pGetNextVASpaceInfo(pThirdPartyP2P, &pInfo) == NV_OK)
        return NV_OK;

    status = CliAddThirdPartyP2PVASpace(pThirdPartyP2P, hVASpace, &token);
    NV_PRINTF(LEVEL_ERROR,
              "GDRP2P lazy REGISTER_VA_SPACE hVASpace=0x%x token=0x%x status=0x%x\n",
              hVASpace, token, status);
    return status;
}

static NV_STATUS
_rmP2PRegisterFoundVidmem
(
    ThirdPartyP2P *pThirdPartyP2P,
    RsClient      *pMemClient,
    Memory        *pMemory,
    NvU64          address,
    NvU64          length,
    NvU64          offset
)
{
    RsClient *pTppClient = RES_GET_CLIENT(pThirdPartyP2P);
    Memory *pRegMemory = pMemory;
    NvHandle hMemory = RES_GET_HANDLE(pMemory);
    NV_STATUS status;

    if (pMemClient->hClient != pTppClient->hClient)
    {
        RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
        Device *pDevice = GPU_RES_GET_DEVICE(pThirdPartyP2P);
        Subdevice *pSubdevice = GPU_RES_GET_SUBDEVICE(pThirdPartyP2P);
        NvHandle hDuped = 0;

        status = pRmApi->DupObject(pRmApi,
                                   pTppClient->hClient,
                                   RES_GET_HANDLE(pDevice),
                                   &hDuped,
                                   pMemClient->hClient,
                                   hMemory,
                                   0);
        if ((status == NV_ERR_INVALID_OBJECT_PARENT) && (pSubdevice != NULL))
        {
            status = pRmApi->DupObject(pRmApi,
                                       pTppClient->hClient,
                                       RES_GET_HANDLE(pSubdevice),
                                       &hDuped,
                                       pMemClient->hClient,
                                       hMemory,
                                       0);
        }
        if (status != NV_OK)
        {
            NV_PRINTF(LEVEL_ERROR,
                      "GDRP2P dup hMemory=0x%x into tpp client=0x%x failed 0x%x\n",
                      hMemory, pTppClient->hClient, status);
            return status;
        }

        status = memGetByHandleAndDevice(pTppClient, hDuped,
                                         RES_GET_HANDLE(pDevice), &pRegMemory);
        if (status != NV_OK)
        {
            pRmApi->Free(pRmApi, pTppClient->hClient, hDuped);
            return status;
        }
        hMemory = hDuped;
    }

    if ((pRegMemory == NULL) || (pRegMemory->pMemDesc == NULL))
        return NV_ERR_INVALID_OBJECT;

    if ((offset + length) > memdescGetSize(pRegMemory->pMemDesc))
        return NV_ERR_INVALID_ARGUMENT;

    if ((address | length | offset) & (NVRM_P2P_PAGESIZE_BIG_64K - 1))
        return NV_ERR_INVALID_ARGUMENT;

    status = CliAddThirdPartyP2PVidmemInfo(pThirdPartyP2P, hMemory,
                                           address, length, offset, pRegMemory);
    if (status == NV_ERR_INSERT_DUPLICATE_NAME)
        status = NV_OK;

    NV_PRINTF(LEVEL_ERROR,
              "GDRP2P lazy REGISTER_VIDMEM hMemory=0x%x addr=0x%llx size=0x%llx off=0x%llx status=0x%x\n",
              hMemory, address, length, offset, status);
    return status;
}

static NV_STATUS
RmP2PLazyRegisterVidmem
(
    ThirdPartyP2P *pThirdPartyP2P,
    NvU64          address,
    NvU64          length
)
{
    OBJGPU *pGpu = GPU_RES_GET_GPU(pThirdPartyP2P);
    RsClient *pMemClient = NULL;
    Memory *pMemory = NULL;
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO *pVidmemInfo = NULL;
    NvU64 offset = 0;
    NvHandle hVASpace = 0;
    NV_STATUS status;

    if (CliGetThirdPartyP2PVidmemInfoFromAddress(pThirdPartyP2P, address, length,
                                                 &offset, &pVidmemInfo) == NV_OK)
    {
        return _rmP2PEnsureVaSpace(pThirdPartyP2P, 0);
    }

    status = _rmP2PFindUserVidmemForAddress(pGpu, address, length,
                                            &pMemClient, &pMemory, &offset, &hVASpace);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "GDRP2P lazy find failed status=0x%x va=0x%llx len=0x%llx tpp=0x%x\n",
                  status, address, length, pThirdPartyP2P->hThirdPartyP2P);
        return status;
    }

    status = _rmP2PRegisterFoundVidmem(pThirdPartyP2P, pMemClient, pMemory,
                                       address, length, offset);
    if (status != NV_OK)
        return status;

    return _rmP2PEnsureVaSpace(pThirdPartyP2P, hVASpace);
}

static NV_STATUS
_rmP2PFindUserClientForGpu(OBJGPU *pGpu, NvU32 pid, NvHandle *phClient)
{
    RmClient **ppClient;

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient *pRmClient = *ppClient;
        RsClient *pClient = staticCast(pRmClient, RsClient);
        Device *pDevice = NULL;

        if (pRmClient->ProcID != pid)
            continue;

        if (pGpu != NULL)
        {
            if (_rmP2PIsInternalClient(pGpu, pClient->hClient))
                continue;
            if (deviceGetByGpu(pClient, pGpu, NV_TRUE, &pDevice) != NV_OK)
                continue;
        }

        *phClient = pClient->hClient;
        return NV_OK;
    }

    return NV_ERR_OBJECT_NOT_FOUND;
}

static NV_STATUS
RmP2PAttachPidToInternalAndRegister
(
    NvU64   address,
    NvU64   length,
    OBJGPU *pGpu
)
{
    NvU32 pid = osGetCurrentProcess();
    RS_SHARE_ITERATOR it;
    NvHandle hUserClient = 0;
    ThirdPartyP2P *pUserTpp = NULL;
    ThirdPartyP2P *pInternalTpp = NULL;
    ThirdPartyP2P *pTarget;
    NV_STATUS status;

    status = _rmP2PFindUserClientForGpu(pGpu, pid, &hUserClient);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "GDRP2P no user RM client for pid=%u\n", pid);
        return status;
    }

    it = serverutilShareIter(classId(P2PTokenShare));
    while (serverutilShareIterNext(&it))
    {
        P2PTokenShare *pShare = dynamicCast(it.pShared, P2PTokenShare);
        ThirdPartyP2P *pThirdPartyP2P;
        OBJGPU *pTppGpu;

        if (pShare == NULL)
            continue;

        pThirdPartyP2P = pShare->pThirdPartyP2P;
        if (pThirdPartyP2P == NULL)
            continue;

        pTppGpu = GPU_RES_GET_GPU(pThirdPartyP2P);
        if ((pGpu != NULL) && (pTppGpu != pGpu))
            continue;

        if (_rmP2PIsInternalObject(pThirdPartyP2P))
        {
            if (pInternalTpp == NULL)
                pInternalTpp = pThirdPartyP2P;
        }
        else if (thirdpartyp2pIsValidClientPid(pThirdPartyP2P, pid, 0) ||
                 (pThirdPartyP2P->hClient == hUserClient))
        {
            if (pUserTpp == NULL)
                pUserTpp = pThirdPartyP2P;
        }
    }

    pTarget = (pUserTpp != NULL) ? pUserTpp : pInternalTpp;
    if (pTarget == NULL)
    {
        NV_PRINTF(LEVEL_ERROR, "GDRP2P no ThirdPartyP2P object to lazy-register\n");
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    if (!thirdpartyp2pIsValidClientPid(pTarget, pid, 0))
    {
        status = CliAddThirdPartyP2PClientPid(pTarget, pid, hUserClient);
        NV_PRINTF(LEVEL_ERROR,
                  "GDRP2P attach pid=%u hClient=0x%x to tpp=0x%x internal=%u status=0x%x\n",
                  pid, hUserClient, pTarget->hThirdPartyP2P,
                  _rmP2PIsInternalObject(pTarget), status);
        if (status != NV_OK)
            return status;
    }

    return RmP2PLazyRegisterVidmem(pTarget, address, length);
}

static
NV_STATUS RmP2PGetVASpaceInfoWithoutToken
(
    NvU64 address,
    NvU64 length,
    void  *pPlatformData,
    void  (*pFreeCallback)(void *pData),
    void  *pData,
    ThirdPartyP2P *pThirdPartyP2P,
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO *ppVASpaceInfo
)
{
    NV_STATUS status;
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO pVASpaceInfo = NULL;
    NvBool bFound = NV_FALSE;
    Subdevice *pSubdevice;
    OBJGPU *pGpu;

    pSubdevice = pThirdPartyP2P->pSubdevice;

    status = RmP2PValidateSubDevice(pThirdPartyP2P, &pGpu);
    if (NV_OK != status)
    {
        return status;
    }

    if ((pThirdPartyP2P->type == CLI_THIRD_PARTY_P2P_TYPE_PROPRIETARY) &&
        !(pThirdPartyP2P->flags & CLI_THIRD_PARTY_P2P_FLAGS_INITIALIZED))
    {
        return NV_ERR_INVALID_STATE;
    }

    while (1)
    {
        status = thirdpartyp2pGetNextVASpaceInfo(pThirdPartyP2P, &pVASpaceInfo);
        if (status != NV_OK)
        {
            if (bFound)
            {
                status = NV_OK;
            }
            return status;
        }

        //
        // Passing NULL for arguments to prevent looking up or
        // updating mapping info in range validation.
        //
        status = RmP2PValidateAddressRangeOrGetPages(address, length, pThirdPartyP2P,
                                                     NULL, NULL, NULL, NULL, NULL,
                                                     pPlatformData, pFreeCallback,
                                                     pData, pGpu, pSubdevice,
                                                     pVASpaceInfo, pThirdPartyP2P);
        if ((NV_OK == status) && bFound)
        {
            return NV_ERR_GENERIC;
        }
        else if (NV_OK == status)
        {
            bFound = NV_TRUE;
        }

        if (NULL != ppVASpaceInfo)
        {
            *ppVASpaceInfo = pVASpaceInfo;
        }
    }

    return status;
}

static
NV_STATUS RmP2PGetInfoWithoutToken
(
    NvU64 address,
    NvU64 length,
    void  *pPlatformData,
    PCLI_THIRD_PARTY_P2P_INFO *ppThirdPartyP2PInfo,
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO *ppVASpaceInfo,
    OBJGPU *pGpu
)
{
    NV_STATUS status;
    PCLI_THIRD_PARTY_P2P_INFO pThirdPartyP2PInfo = NULL;
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO pVASpaceInfo = NULL;
    NvBool bFound = NV_FALSE;
    NvBool bLazyTried = NV_FALSE;
    NvU32 processId = osGetCurrentProcess();

retry:
    while (1)
    {
        RmClient *pClient;
        status = CliNextThirdPartyP2PInfoWithPid(pGpu,
                                                 processId,
                                                 0,
                                                 &pClient,
                                                 &pThirdPartyP2PInfo);
        if (NV_OK != status)
        {
            if (bFound)
            {
                status = NV_OK;
            }
            break;
        }

        if ((pThirdPartyP2PInfo->type == CLI_THIRD_PARTY_P2P_TYPE_PROPRIETARY) &&
            !(pThirdPartyP2PInfo->flags & CLI_THIRD_PARTY_P2P_FLAGS_INITIALIZED))
        {
            continue;
        }

        if (0 == length)
        {
            // PutPages
            status = CliGetThirdPartyP2PPlatformData(pThirdPartyP2PInfo,
                                                     pPlatformData);
        }
        else
        {
            // GetPages
            status = RmP2PGetVASpaceInfoWithoutToken(address,
                                              length,
                                              pPlatformData,
                                              NULL,
                                              NULL,
                                              pThirdPartyP2PInfo,
                                              &pVASpaceInfo);
            if (NV_OK == status)
            {
                *ppVASpaceInfo = pVASpaceInfo;
            }
        }

        if (NV_OK == status)
        {
            if (bFound)
            {
                status = NV_ERR_GENERIC;
                break;
            }
            else
            {
                bFound = NV_TRUE;
                if (NULL != ppThirdPartyP2PInfo)
                {
                    *ppThirdPartyP2PInfo = pThirdPartyP2PInfo;
                }
            }
        }
    }

    if ((status != NV_OK) && !bLazyTried && (length != 0))
    {
        bLazyTried = NV_TRUE;
        _rmP2PDumpThirdPartyP2PInventory(processId, address, length);
        if (RmP2PAttachPidToInternalAndRegister(address, length, pGpu) == NV_OK)
        {
            pThirdPartyP2PInfo = NULL;
            pVASpaceInfo = NULL;
            bFound = NV_FALSE;
            goto retry;
        }
        _rmP2PDumpUserFbAllocs(processId, pGpu, length);
    }

    return status;
}

static NvBool _isSpaceAvailableForBar1P2PMapping(
    OBJGPU    *pGpu,
    Subdevice *pSubDevice,
    RsClient  *pClient,
    NvU64      length
)
{
    NvU64 bar1SizeBytes;
    NvU64 fbAvailableBytes;
    GETBAR1INFO bar1Info;
    NV_STATUS status;
    MemoryManager *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    NvBool bGpuLockTaken = (rmDeviceGpuLockIsOwner(gpuGetInstance(pGpu)) ||
                            rmGpuLockIsOwner());

    if (!bGpuLockTaken)
    {    
        NV_ASSERT_OK_OR_RETURN(rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE,
                                                       RM_LOCK_MODULES_P2P));
    }

    status = memmgrGetBAR1InfoForDevice(pGpu, pMemoryManager,
                                        GPU_RES_GET_DEVICE(pSubDevice),
                                        &bar1Info);

    if (!bGpuLockTaken)
    {    
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    }

    if (status != NV_OK)
        return NV_FALSE;

    // Convert Bar1 size to bytes as reported size is in KB.
    bar1SizeBytes = ((NvU64)bar1Info.bar1Size) << 10;

    if (bar1SizeBytes <  pSubDevice->P2PfbMappedBytes)
    {
        DBG_BREAKPOINT();
        return NV_FALSE;
    }

    fbAvailableBytes = (bar1SizeBytes - pSubDevice->P2PfbMappedBytes);
    return (fbAvailableBytes >= (CLI_THIRD_PARTY_P2P_BAR1_RESERVE + length));
}

static NV_STATUS _rmP2PGetPages(
    NvU64       p2pToken,
    NvU32       vaSpaceToken,
    NvU64       address,
    NvU64       length,
    NvU64      *pPhysicalAddresses,
    NvU32      *pWreqMbH,
    NvU32      *pRreqMbH,
    NvU32      *pEntries,
    NvBool     *pbMemCpuCacheable,
    OBJGPU    **ppGpu,
    void       *pPlatformData,
    void      (*pFreeCallback)(void *pData),
    void       *pData
)
{
    NV_STATUS status;
    OBJGPU *pGpu;
    ThirdPartyP2P *pThirdPartyP2P;
    Subdevice *pSubdevice;
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO pVASpaceInfo = NULL;

    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(address, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(length, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);

    if (0 != p2pToken)
    {
        status = CliGetThirdPartyP2PInfoFromToken(p2pToken,
                                                  &pThirdPartyP2P);
    }
    else
    {
        status = RmP2PGetInfoWithoutToken(address,
                                       length,
                                       pPlatformData,
                                       &pThirdPartyP2P,
                                       &pVASpaceInfo,
                                       NULL);
    }

    if (status != NV_OK)
    {
        return status;
    }

    pSubdevice = pThirdPartyP2P->pSubdevice;

    if ((pThirdPartyP2P->type == CLI_THIRD_PARTY_P2P_TYPE_PROPRIETARY) &&
        !(pThirdPartyP2P->flags & CLI_THIRD_PARTY_P2P_FLAGS_INITIALIZED))
    {
        status = NV_ERR_INVALID_STATE;
        goto failed;
    }

    status = RmP2PValidateSubDevice(pThirdPartyP2P, &pGpu);
    if (status != NV_OK)
    {
        goto failed;
    }

    if (0 != vaSpaceToken)
    {
        status = thirdpartyp2pGetVASpaceInfoFromToken(pThirdPartyP2P, vaSpaceToken, &pVASpaceInfo);
        if (status != NV_OK)
        {
            goto failed;
        }
    }

    if (pVASpaceInfo == NULL)
    {
        status = NV_ERR_INVALID_STATE;
        goto failed;
    }

    status = RmP2PValidateAddressRangeOrGetPages(address, length, pThirdPartyP2P,
                                                 &pPhysicalAddresses, &pWreqMbH,
                                                 &pRreqMbH, pEntries, pbMemCpuCacheable,
                                                 pPlatformData, pFreeCallback,
                                                 pData, pGpu, pSubdevice,
                                                 pVASpaceInfo, pThirdPartyP2P);
    if (status != NV_OK)
    {
        goto failed;
    }

    if (ppGpu != NULL)
    {
        *ppGpu = pGpu;
    }

    return NV_OK;
failed:
    thirdpartyp2pDelMappingInfoByKey(pThirdPartyP2P, pPlatformData);

    return status;
}

static
CLI_THIRD_PARTY_P2P_VIDMEM_INFO* _createOrReuseVidmemInfoPersistent
(
    OBJGPU         *pGpu,
    NvU64           address,
    NvU64           length,
    NvU64          *pOffset,
    NvHandle        hClientInternal,
    ThirdPartyP2P  *pThirdPartyP2P,
    ThirdPartyP2P  *pThirdPartyP2PInternal
)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO *pVidmemInfo = NULL;
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO *pVidmemInfoInternal = NULL;
    Memory *pMemoryInternal;
    RsClient *pClientInternal;
    Device *pDevice;
    Subdevice *pSubdevice;
    NvU64 offset = 0;
    NvHandle hMemoryDuped = 0;
    NV_STATUS status;
    NvBool bMemDuped = NV_FALSE;

    //
    // Note: hMemory is duped(memory is ref-counted) only once for the first time.
    // All subsequent get_pages_persistent() requests reuse the same VidmemInfo.
    // Mappings are ref-counted using ExtentInfo in the MappingInfoList.
    // The duped handle is freed when the MappingInfoList is empty in
    // put_pages_persistent() path.
    //

    //
    // Get user client's ThirdPartyP2P's VidmemInfo
    // Needed to get user's offset and hMemory
    //
    status = CliGetThirdPartyP2PVidmemInfoFromAddress(pThirdPartyP2P,
                                                      address,
                                                      length,
                                                      &offset,
                                                      &pVidmemInfo);
    if (status != NV_OK)
    {
        goto failed;
    }

    *pOffset = offset;

    //
    // Check if an internal VidmemInfo already exists.
    // Every VidmemInfo is assigned a unique ID and the internal ThirdPartyP2P
    // object's AddressRangeTree is keyed at user client's VidmemInfo ID instead
    // of the VA. This is because the VA could have been reassigned to another
    // phys allocation.
    //
    status = CliGetThirdPartyP2PVidmemInfoFromId(pThirdPartyP2PInternal,
                                                 pVidmemInfo->id,
                                                 &pVidmemInfoInternal);
    if (status == NV_OK)
    {
        return pVidmemInfoInternal;
    }
    else if (status != NV_ERR_OBJECT_NOT_FOUND)
    {
        goto failed;
    }

    pClientInternal = RES_GET_CLIENT(pThirdPartyP2PInternal);
    pDevice = GPU_RES_GET_DEVICE(pThirdPartyP2PInternal);
    pSubdevice = GPU_RES_GET_SUBDEVICE(pThirdPartyP2PInternal);

    // Dupe user client's hMemory
    status = pRmApi->DupObject(pRmApi,
                               hClientInternal,
                               RES_GET_HANDLE(pDevice),
                               &hMemoryDuped,
                               pThirdPartyP2P->hClient,
                               pVidmemInfo->hMemory,
                               0);
    if (status == NV_ERR_INVALID_OBJECT_PARENT)
    {
        // If duping under Device fails, try duping under Subdevice
        status = pRmApi->DupObject(pRmApi,
                                   hClientInternal,
                                   RES_GET_HANDLE(pSubdevice),
                                   &hMemoryDuped,
                                   pThirdPartyP2P->hClient,
                                   pVidmemInfo->hMemory,
                                   0);
    }
    if (status != NV_OK)
    {
        goto failed;
    }

    bMemDuped = NV_TRUE;

    status = memGetByHandleAndDevice(pClientInternal,
                                     hMemoryDuped,
                                     RES_GET_HANDLE(pDevice),
                                     &pMemoryInternal);
    if (status != NV_OK)
    {
        goto failed;
    }

    //
    // Add a new VidmemInfo with the address field as user's VidmemInfo ID
    // and length = 1. This is because keyStart and keyEnd for internal
    // AddressRangeTree should be the user's VidmemInfo ID.
    //
    status = CliAddThirdPartyP2PVidmemInfo(pThirdPartyP2PInternal,
                                           hMemoryDuped,
                                           pVidmemInfo->id,
                                           1,
                                           pVidmemInfo->offset,
                                           pMemoryInternal);
    if (status != NV_OK)
    {
        goto failed;
    }

    // Fetch the newly added VidmemInfo to return.
    status = CliGetThirdPartyP2PVidmemInfoFromId(pThirdPartyP2PInternal,
                                                 pVidmemInfo->id,
                                                 &pVidmemInfoInternal);
    if (status != NV_OK)
    {
        goto failed;
    }

    return pVidmemInfoInternal;

failed:
    if (bMemDuped)
    {
        pRmApi->Free(pRmApi, hClientInternal, hMemoryDuped);
    }

    return NULL;
}

static NV_STATUS RmP2PGetMigInfo(
    OBJGPU                   *pGpu,
    NvU64                     address,
    NvU64                     length,
    ThirdPartyP2P            *pThirdPartyP2P,
    KERNEL_MIG_GPU_INSTANCE **ppGpuInstanceInfo
)
{
    NvHandle hClient, hMemory;
    MIG_INSTANCE_REF ref;
    KernelMIGManager *pKernelMIGManager = GPU_GET_KERNEL_MIG_MANAGER(pGpu);
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO *pVidmemInfo = NULL;
    Memory *pMemory;
    RsClient *pClient;
    NvU64 offset;

    // Get hClient and hMemory
    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                          CliGetThirdPartyP2PVidmemInfoFromAddress(pThirdPartyP2P,
                                        address, length, &offset, &pVidmemInfo));
    hClient = pVidmemInfo->hClient;
    hMemory = pVidmemInfo->hMemory;

    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                          serverGetClientUnderLock(&g_resServ, hClient, &pClient));

    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                          memGetByHandle(pClient, hMemory, &pMemory));

    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                          kmigmgrGetInstanceRefFromDevice(pGpu, pKernelMIGManager,
                                                          pMemory->pDevice, &ref));

    // Refcount++ MIG instance
    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                          kmigmgrIncRefCount(ref.pKernelMIGGpuInstance->pShare));

    *ppGpuInstanceInfo = (void *) ref.pKernelMIGGpuInstance;

    return NV_OK;
}

static void RmP2PPutMigInfo(
    void  *pGpuInstanceInfo
)
{
    KERNEL_MIG_GPU_INSTANCE *pKernelMIGGpuInstance;

    if (pGpuInstanceInfo == NULL)
    {
        return;
    }

    pKernelMIGGpuInstance = (KERNEL_MIG_GPU_INSTANCE *) pGpuInstanceInfo;

    // Refcount-- MIG instance
    NV_ASSERT_OK(kmigmgrDecRefCount(pKernelMIGGpuInstance->pShare));
}

NV_STATUS RmP2PGetPagesPersistent(
    NvU64       address,
    NvU64       length,
    void      **p2pObject,
    NvU64      *pPhysicalAddresses,
    NvU32      *pEntries,
    NvBool     *pbMemCpuCacheable,
    NvBool      bForcePcie,
    void       *pPlatformData,
    void       *pGpuInfo,
    void      **ppGpuInstanceInfo
)
{
    RsResourceRef *pResourceRef;
    OBJGPU *pGpu = (OBJGPU *) pGpuInfo;
    ThirdPartyP2P *pThirdPartyP2P = NULL;
    ThirdPartyP2P *pThirdPartyP2PInternal = NULL;
    CLI_THIRD_PARTY_P2P_VASPACE_INFO *pVASpaceInfo = NULL;
    CLI_THIRD_PARTY_P2P_VIDMEM_INFO *pVidmemInfo = NULL;
    KERNEL_MIG_GPU_INSTANCE *pKernelMIGGpuInstance = NULL;
    NvU64 offset = 0;
    NvHandle hClientInternal;
    NvHandle hThirdPartyP2PInternal;
    NV_STATUS status;

    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(address, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(NV_IS_ALIGNED64(length, NVRM_P2P_PAGESIZE_BIG_64K),
                        NV_ERR_INVALID_ARGUMENT);

    if(gpuIsApmFeatureEnabled(pGpu))
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    //
    // Forced PCIe mappings are to be used only on coherent systems with a
    // direct PCIe connection between the exporter and importer.
    // MIG is not a supported use-case on these systems.
    //
    if (bForcePcie)
    {
        KernelBus *pKernelBus = GPU_GET_KERNEL_BUS(pGpu);

        if (!pGpu->getProperty(pGpu, PDB_PROP_GPU_COHERENT_CPU_MAPPING) ||
            pKernelBus->bBar1Disabled ||
            IS_MIG_ENABLED(pGpu))
        {
            return NV_ERR_NOT_SUPPORTED;
        }
    }

    status = RmP2PGetInfoWithoutToken(address, length, NULL,
                                      &pThirdPartyP2P, &pVASpaceInfo, pGpu);
    if (status != NV_OK)
    {
        return status;
    }

    if (IS_MIG_ENABLED(pGpu))
    {
        status = RmP2PGetMigInfo(pGpu, address, length, pThirdPartyP2P,
                                 &pKernelMIGGpuInstance);
        if (status != NV_OK)
        {
            return status;
        }
        *ppGpuInstanceInfo = (void *) pKernelMIGGpuInstance;

        if (pKernelMIGGpuInstance->instanceHandles.hThirdPartyP2P == NV01_NULL_OBJECT)
        {
            status = NV_ERR_NOT_SUPPORTED;

            goto failed;
        }

        hClientInternal = pKernelMIGGpuInstance->instanceHandles.hClient;
        hThirdPartyP2PInternal = pKernelMIGGpuInstance->instanceHandles.hThirdPartyP2P;
    }
    else
    {
        MemoryManager *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);

        if (pMemoryManager->hThirdPartyP2P == NV01_NULL_OBJECT)
        {
            return NV_ERR_NOT_SUPPORTED;
        }

        hClientInternal = pMemoryManager->hClient;
        hThirdPartyP2PInternal = pMemoryManager->hThirdPartyP2P;
        *ppGpuInstanceInfo = NULL;
    }

    status = serverutilGetResourceRef(hClientInternal,
                                      hThirdPartyP2PInternal,
                                      &pResourceRef);
    if (status != NV_OK)
    {
        goto failed;
    }

    pThirdPartyP2PInternal = dynamicCast(pResourceRef->pResource, ThirdPartyP2P);

    //
    // Forced PCIe mappings are not supported
    // with third party object type BAR1.
    //
    if ((bForcePcie) &&
        (pThirdPartyP2PInternal->type == CLI_THIRD_PARTY_P2P_TYPE_BAR1))
    {
        status = NV_ERR_NOT_SUPPORTED;

        goto failed;
    }

    pVidmemInfo = _createOrReuseVidmemInfoPersistent(pGpu, address, length, &offset,
                                                     hClientInternal,
                                                     pThirdPartyP2P,
                                                     pThirdPartyP2PInternal);
    if (pVidmemInfo == NULL)
    {
        status = NV_ERR_INVALID_STATE;

        goto failed;
    }

    status = RmP2PGetPagesUsingVidmemInfo(address, length, offset, bForcePcie,
                                          pThirdPartyP2PInternal,
                                          &pPhysicalAddresses, NULL, NULL,
                                          pEntries, pbMemCpuCacheable, pPlatformData,
                                          NULL, NULL, pGpu,
                                          pThirdPartyP2PInternal->pSubdevice,
                                          NULL, pThirdPartyP2PInternal, pVidmemInfo);
    if (status != NV_OK)
    {
        // Cleanup MappingInfo if it was allocated
        thirdpartyp2pDelMappingInfoByKey(pThirdPartyP2PInternal, pPlatformData);

        //
        // The cleanup with thirdpartyp2pDelMappingInfoByKey() above is not enough
        // since creating MappingInfo with pPlatformData could have failed.
        // Cleanup of the internal VidmemInfo is still needed since pPlatformData
        // lookup would fail and the VidmemInfo is not available for cleanup via
        // thirdpartyp2pDelPersistentMappingInfoByKey().
        //
        CliDelThirdPartyP2PVidmemInfoPersistent(pThirdPartyP2PInternal, pVidmemInfo);

        goto failed;
    }

    //
    // Update p2pObject as the internal ThirdPartyP2P object
    // which will be used by nvidia_p2p_put_pages() to look up mappings.
    //
    *p2pObject = (void *) pThirdPartyP2PInternal;

    return NV_OK;

failed:
    RmP2PPutMigInfo(pKernelMIGGpuInstance);

    return status;
}

NV_STATUS RmP2PGetPages(
    NvU64       p2pToken,
    NvU32       vaSpaceToken,
    NvU64       address,
    NvU64       length,
    NvU64      *pPhysicalAddresses,
    NvU32      *pWreqMbH,
    NvU32      *pRreqMbH,
    NvU32      *pEntries,
    OBJGPU    **ppGpu,
    void       *pPlatformData,
    void      (*pFreeCallback)(void *pData),
    void       *pData
)
{
    NvBool bMemCpuCacheable;

    if (pFreeCallback == NULL || pData == NULL)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "invalid argument(s) in RmP2PGetPages, pFreeCallback=%p pData=%p\n",
                  pFreeCallback, pData);
        return NV_ERR_INVALID_ARGUMENT;
    }

    return _rmP2PGetPages(p2pToken, vaSpaceToken, address, length,
                          pPhysicalAddresses, pWreqMbH, pRreqMbH,
                          pEntries, &bMemCpuCacheable, ppGpu, pPlatformData,
                          pFreeCallback, pData);
}

NV_STATUS RmP2PGetPagesWithoutCallbackRegistration(
    NvU64       p2pToken,
    NvU32       vaSpaceToken,
    NvU64       address,
    NvU64       length,
    NvU64      *pPhysicalAddresses,
    NvU32      *pWreqMbH,
    NvU32      *pRreqMbH,
    NvU32      *pEntries,
    NvBool     *pbMemCpuCacheable,
    OBJGPU    **ppGpu,
    void       *pPlatformData
)
{
    return _rmP2PGetPages(p2pToken, vaSpaceToken, address, length,
                          pPhysicalAddresses, pWreqMbH, pRreqMbH,
                          pEntries, pbMemCpuCacheable, ppGpu, pPlatformData,
                          NULL, NULL);
}

NV_STATUS RmP2PGetGpuByAddress(
    NvU64      address,
    NvU64      length,
    OBJGPU   **ppGpu
)
{
    ThirdPartyP2P *pThirdPartyP2P = NULL;
    CLI_THIRD_PARTY_P2P_VASPACE_INFO *pVASpaceInfo = NULL;
    OBJGPU *pGpu = NULL;
    NV_STATUS status = NV_OK;

    status = RmP2PGetInfoWithoutToken(address, length, NULL,
                                      &pThirdPartyP2P, &pVASpaceInfo, NULL);
    if (status != NV_OK)
    {
        return status;
    }

    status = RmP2PValidateSubDevice(pThirdPartyP2P, &pGpu);
    if (status != NV_OK)
    {
        return status;
    }

    *ppGpu = pGpu;

    return status;
}

NV_STATUS RmP2PRegisterCallback(
    NvU64       p2pToken,
    NvU64       address,
    NvU64       length,
    void       *pPlatformData,
    void      (*pFreeCallback)(void *pData),
    void       *pData
)
{
    NV_STATUS status;
    ThirdPartyP2P *pThirdPartyP2P;
    PCLI_THIRD_PARTY_P2P_VASPACE_INFO pVASpaceInfo = NULL;
    PCLI_THIRD_PARTY_P2P_VIDMEM_INFO pVidmemInfo;
    NvU64 offset;

    if (0 != p2pToken)
    {
        status = CliGetThirdPartyP2PInfoFromToken(p2pToken,
                                                  &pThirdPartyP2P);
    }
    else
    {
        status = RmP2PGetInfoWithoutToken(address,
                                          0,
                                          pPlatformData,
                                          &pThirdPartyP2P,
                                          &pVASpaceInfo,
                                          NULL);
    }
    if (status != NV_OK)
    {
        return status;
    }

    status = CliGetThirdPartyP2PVidmemInfoFromAddress(pThirdPartyP2P, address,
                                                      length, &offset, &pVidmemInfo);
    if (status != NV_OK)
    {
        return status;
    }

    return CliRegisterThirdPartyP2PMappingCallback(pThirdPartyP2P,
                                                   pVidmemInfo->hMemory,
                                                   pPlatformData, pFreeCallback,
                                                   pData);
}

NV_STATUS RmP2PPutPagesPersistent(
    void       *p2pObject,
    void       *pPlatformData,
    void       *pMigInfo
)
{
    NV_STATUS status;
    ThirdPartyP2P *pThirdPartyP2P = NULL;

    pThirdPartyP2P = (ThirdPartyP2P *)(p2pObject);

    if ((pThirdPartyP2P->type == CLI_THIRD_PARTY_P2P_TYPE_PROPRIETARY) &&
        !(pThirdPartyP2P->flags & CLI_THIRD_PARTY_P2P_FLAGS_INITIALIZED))
    {
        return NV_ERR_INVALID_STATE;
    }

    status = thirdpartyp2pDelPersistentMappingInfoByKey(pThirdPartyP2P, pPlatformData);

    NV_ASSERT(status == NV_OK);

    RmP2PPutMigInfo(pMigInfo);

    return status;
}

NV_STATUS RmP2PPutPages(
    NvU64       p2pToken,
    NvU32       vaSpaceToken,
    NvU64       address,
    void       *pPlatformData
)
{
    NV_STATUS status;
    ThirdPartyP2P *pThirdPartyP2P;

    if (0 != p2pToken)
    {
        status = CliGetThirdPartyP2PInfoFromToken(p2pToken,
                                                  &pThirdPartyP2P);
    }
    else
    {
        status = RmP2PGetInfoWithoutToken(address,
                                          0,
                                          pPlatformData,
                                          &pThirdPartyP2P,
                                          NULL, NULL);
    }
    if (status != NV_OK)
    {
        return status;
    }

    if ((pThirdPartyP2P->type == CLI_THIRD_PARTY_P2P_TYPE_PROPRIETARY) &&
        !(pThirdPartyP2P->flags & CLI_THIRD_PARTY_P2P_FLAGS_INITIALIZED))
    {
        return NV_ERR_INVALID_STATE;
    }

    status = thirdpartyp2pDelMappingInfoByKey(pThirdPartyP2P, pPlatformData);
    NV_ASSERT(status == NV_OK);

    return status;
}
