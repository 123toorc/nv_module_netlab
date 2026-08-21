/*
 * LD_PRELOAD hook that makes GDRCopy work on GeForce / RTX 4090D.
 *
 * GeForce libcuda never sends REGISTER_VA_SPACE / REGISTER_VIDMEM, so
 * nvidia_p2p_get_pages() returns NV_ERR_OBJECT_NOT_FOUND (0x57) / -EINVAL.
 *
 * This library:
 *   1. Forces CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_* to 1
 *   2. Turns cuMemAlloc into VMM (cuMemCreate/cuMemMap) so RM has an hMemory
 *   3. Hooks ioctl() to steal CUDA's hClient/hDevice/hSubdevice/hVASpace/hMemory
 *   4. Allocates NV50_THIRD_PARTY_P2P (0x503c) once and registers each buffer
 *
 * Do not change the NVIDIA kernel modules for this path. BAR1 P2P (aikitoria
 * 575.64.05-p2p) must already be loaded.
 *
 *   export LD_PRELOAD=/path/to/libgdr_geforce_hook.so
 *   ./copybw
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* NVIDIA RM ioctl wire format (must match nvos.h / cl503c.h / ctrl503c.h)    */
/* -------------------------------------------------------------------------- */

#define NV_IOCTL_MAGIC              'F'
#define NV_ESC_RM_ALLOC_OBJECT      0x28
#define NV_ESC_RM_CONTROL           0x2A
#define NV_ESC_RM_ALLOC             0x2B

#define NV01_DEVICE_0               0x00000080u
#define NV20_SUBDEVICE_0            0x00002080u
#define FERMI_VASPACE_A             0x000090f1u
#define NV50_THIRD_PARTY_P2P        0x0000503cu
#define NV01_MEMORY_LOCAL_USER      0x00000040u
#define NV01_MEMORY_LOCAL_PHYSICAL  0x000000c2u
#define NV01_MEMORY_LIST_FBMEM      0x00000082u

#define NV503C_FLAGS_TYPE_BAR1      0x00000001u
#define NV503C_CTRL_CMD_REGISTER_VA_SPACE  0x503c0102u
#define NV503C_CTRL_CMD_REGISTER_VIDMEM    0x503c0104u
#define NV503C_CTRL_CMD_UNREGISTER_VIDMEM  0x503c0105u

#define NV_OK                       0x00000000u
#define NV_ERR_INSERT_DUPLICATE_NAME 0x00000056u

#define GDR_PAGE                    0x10000ull

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint32_t paramsSize;
    uint32_t status;
} __attribute__((aligned(8))) nvos21_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint64_t pRightsRequested;
    uint32_t paramsSize;
    uint32_t flags;
    uint32_t status;
} __attribute__((aligned(8))) nvos64_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint32_t status;
} nvos05_t;

typedef struct {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;
    uint32_t flags;
    uint64_t params;
    uint32_t paramsSize;
    uint32_t status;
} __attribute__((aligned(8))) nvos54_t;

typedef struct {
    uint32_t flags;
} nv503c_alloc_t;

typedef struct {
    uint32_t hVASpace;
    uint32_t pad;
    uint64_t vaSpaceToken;
} nv503c_register_vaspace_t;

typedef struct {
    uint32_t hMemory;
    uint32_t pad;
    uint64_t address;
    uint64_t size;
    uint64_t offset;
} nv503c_register_vidmem_t;

typedef struct {
    uint32_t hMemory;
} nv503c_unregister_vidmem_t;

_Static_assert(sizeof(nvos21_t) == 32, "NVOS21 size");
_Static_assert(sizeof(nvos64_t) == 48, "NVOS64 size");
_Static_assert(sizeof(nvos54_t) == 32, "NVOS54 size");
_Static_assert(sizeof(nv503c_register_vaspace_t) == 16, "REGISTER_VA_SPACE size");
_Static_assert(sizeof(nv503c_register_vidmem_t) == 32, "REGISTER_VIDMEM size");

/* -------------------------------------------------------------------------- */
/* Minimal CUDA driver types (CUDA 12.x layout)                               */
/* -------------------------------------------------------------------------- */

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef unsigned long long CUmemGenericAllocationHandle;
typedef int CUdevice_attribute;
typedef int CUpointer_attribute;
typedef int CUmemAllocationGranularity_flags;

#define CUDA_SUCCESS 0
#define CUDA_ERROR_NOT_SUPPORTED 801
#define CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED 110
#define CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED 116
#define CU_POINTER_ATTRIBUTE_SYNC_MEMOPS 6
#define CU_MEM_ALLOCATION_TYPE_PINNED 0x1
#define CU_MEM_LOCATION_TYPE_DEVICE 0x1
#define CU_MEM_ACCESS_FLAGS_PROT_READWRITE 0x3
#define CU_MEM_ALLOC_GRANULARITY_MINIMUM 0x0

typedef struct {
    int type;
    int id;
} CUmemLocation;

typedef struct {
    unsigned int type;
    unsigned int requestedHandleTypes;
    CUmemLocation location;
    void *win32HandleMetaData;
    struct {
        unsigned char compressionType;
        unsigned char gpuDirectRDMACapable;
        unsigned short usage;
        unsigned char reserved[4];
    } allocFlags;
} CUmemAllocationProp;

typedef struct {
    CUmemLocation location;
    unsigned int flags;
} CUmemAccessDesc;

/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */

typedef CUresult (*fn_cuDeviceGetAttribute)(int *, CUdevice_attribute, CUdevice);
typedef CUresult (*fn_cuCtxGetDevice)(CUdevice *);
typedef CUresult (*fn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*fn_cuMemFree)(CUdeviceptr);
typedef CUresult (*fn_cuMemGetAllocationGranularity)(size_t *, const CUmemAllocationProp *, CUmemAllocationGranularity_flags);
typedef CUresult (*fn_cuMemAddressReserve)(CUdeviceptr *, size_t, size_t, CUdeviceptr, unsigned long long);
typedef CUresult (*fn_cuMemAddressFree)(CUdeviceptr, size_t);
typedef CUresult (*fn_cuMemCreate)(CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long);
typedef CUresult (*fn_cuMemRelease)(CUmemGenericAllocationHandle);
typedef CUresult (*fn_cuMemMap)(CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long);
typedef CUresult (*fn_cuMemUnmap)(CUdeviceptr, size_t);
typedef CUresult (*fn_cuMemSetAccess)(CUdeviceptr, size_t, const CUmemAccessDesc *, size_t);
typedef CUresult (*fn_cuPointerSetAttribute)(const void *, CUpointer_attribute, CUdeviceptr);
typedef int (*fn_ioctl)(int, unsigned long, ...);

static fn_ioctl real_ioctl;
static fn_cuDeviceGetAttribute real_cuDeviceGetAttribute;
static fn_cuCtxGetDevice real_cuCtxGetDevice;
static fn_cuMemAlloc real_cuMemAlloc;
static fn_cuMemAlloc real_cuMemAlloc_v2;
static fn_cuMemFree real_cuMemFree;
static fn_cuMemFree real_cuMemFree_v2;
static fn_cuMemGetAllocationGranularity real_cuMemGetAllocationGranularity;
static fn_cuMemAddressReserve real_cuMemAddressReserve;
static fn_cuMemAddressFree real_cuMemAddressFree;
static fn_cuMemCreate real_cuMemCreate;
static fn_cuMemRelease real_cuMemRelease;
static fn_cuMemMap real_cuMemMap;
static fn_cuMemUnmap real_cuMemUnmap;
static fn_cuMemSetAccess real_cuMemSetAccess;
static fn_cuPointerSetAttribute real_cuPointerSetAttribute;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int target_gpu;
static int nv_fd = -1;
static uint32_t h_client;
static uint32_t h_device;
static uint32_t h_subdevice;
static uint32_t h_vaspace;
static int n_device;
static int handles_ready;
static uint32_t h_tpp;
static int tpp_ready;
static int vas_registered;

static int capturing_memory;
static uint32_t captured_hmemory;

static int quiet;
static int hook_ready;

typedef struct vmm_alloc {
    CUdeviceptr va;
    size_t size;
    CUmemGenericAllocationHandle phys;
    uint32_t hMemory;
    int replaced_cumemalloc;
    struct vmm_alloc *next;
} vmm_alloc_t;

static vmm_alloc_t *vmm_list;

static void logmsg(const char *fmt, ...)
{
    va_list ap;

    if (quiet)
        return;
    fputs("[gdr-geforce] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint64_t round_up(uint64_t x, uint64_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static int is_memory_class(uint32_t hClass)
{
    switch (hClass) {
    case NV01_MEMORY_LOCAL_USER:
    case NV01_MEMORY_LOCAL_PHYSICAL:
    case NV01_MEMORY_LIST_FBMEM:
    case 0x0000003eu:
    case 0x00000071u:
    case 0x0000007au:
    case 0x0000003fu:
        return 1;
    default:
        return (hClass != NV01_DEVICE_0 &&
                hClass != NV20_SUBDEVICE_0 &&
                hClass != FERMI_VASPACE_A &&
                hClass != NV50_THIRD_PARTY_P2P &&
                hClass != 0x0u &&
                hClass != 0x41u);
    }
}

static void track_alloc(int fd, uint32_t hRoot, uint32_t hParent,
                        uint32_t hNew, uint32_t hClass, uint32_t status)
{
    if (status != NV_OK || hNew == 0)
        return;

    if (capturing_memory) {
        int known = (hClass == NV01_MEMORY_LOCAL_USER ||
                     hClass == NV01_MEMORY_LOCAL_PHYSICAL ||
                     hClass == NV01_MEMORY_LIST_FBMEM ||
                     hClass == 0x0000003eu ||
                     hClass == 0x00000071u ||
                     hClass == 0x0000007au);
        if (known || (captured_hmemory == 0 && is_memory_class(hClass))) {
            captured_hmemory = hNew;
            logmsg("captured hMemory=0x%x class=0x%x\n", hNew, hClass);
        }
    }

    if (hClass == NV01_DEVICE_0 && hRoot != 0) {
        if (h_client != hRoot) {
            h_client = hRoot;
            n_device = 0;
            h_device = 0;
            h_subdevice = 0;
            h_vaspace = 0;
            handles_ready = 0;
            tpp_ready = 0;
            vas_registered = 0;
            h_tpp = 0;
        }
        nv_fd = fd;
        if (n_device == target_gpu)
            h_device = hNew;
        n_device++;
        logmsg("DEVICE #%d hClient=0x%x hDevice=0x%x fd=%d\n",
               n_device - 1, h_client, hNew, fd);
    }

    if (hClass == NV20_SUBDEVICE_0 && hParent == h_device && h_device != 0) {
        h_subdevice = hNew;
        logmsg("SUBDEVICE hSubdevice=0x%x\n", h_subdevice);
    }

    if (hClass == FERMI_VASPACE_A && hParent == h_device && h_device != 0 &&
        h_vaspace == 0) {
        h_vaspace = hNew;
        if (h_client && h_device && h_subdevice && h_vaspace) {
            handles_ready = 1;
            logmsg("ready GPU %d client=0x%x device=0x%x subdev=0x%x va=0x%x\n",
                   target_gpu, h_client, h_device, h_subdevice, h_vaspace);
        }
    }
}

static void after_rm_alloc(int fd, unsigned long request, void *arg)
{
    unsigned int nr = (unsigned int)_IOC_NR(request);
    unsigned int type = (unsigned int)_IOC_TYPE(request);
    unsigned int size = (unsigned int)_IOC_SIZE(request);

    if (type != NV_IOCTL_MAGIC || arg == NULL)
        return;

    if (nr == NV_ESC_RM_ALLOC) {
        if (size == sizeof(nvos64_t)) {
            nvos64_t *p = arg;
            track_alloc(fd, p->hRoot, p->hObjectParent, p->hObjectNew,
                        p->hClass, p->status);
        } else if (size == sizeof(nvos21_t)) {
            nvos21_t *p = arg;
            track_alloc(fd, p->hRoot, p->hObjectParent, p->hObjectNew,
                        p->hClass, p->status);
        }
        return;
    }

    if (nr == NV_ESC_RM_ALLOC_OBJECT && size == sizeof(nvos05_t)) {
        nvos05_t *p = arg;
        track_alloc(fd, p->hRoot, p->hObjectParent, p->hObjectNew,
                    p->hClass, p->status);
    }
}

static int call_ioctl(int fd, unsigned long request, void *arg)
{
    if (!real_ioctl)
        real_ioctl = (fn_ioctl)dlsym(RTLD_NEXT, "ioctl");
    return real_ioctl(fd, request, arg);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    int ret;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    ret = call_ioctl(fd, request, arg);
    if (ret == 0) {
        pthread_mutex_lock(&lock);
        after_rm_alloc(fd, request, arg);
        pthread_mutex_unlock(&lock);
    }
    return ret;
}

static uint32_t rm_alloc(uint32_t hRoot, uint32_t hParent, uint32_t hClass,
                         void *parms, uint32_t parmsSize, uint32_t *hOut)
{
    nvos64_t p;
    unsigned long req;
    int rc;

    memset(&p, 0, sizeof(p));
    p.hRoot = hRoot;
    p.hObjectParent = hParent;
    p.hObjectNew = 0;
    p.hClass = hClass;
    p.pAllocParms = (uint64_t)(uintptr_t)parms;
    p.paramsSize = parmsSize;

    req = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, nvos64_t);
    rc = call_ioctl(nv_fd, req, &p);
    if (rc == 0 && p.status == NV_OK && p.hObjectNew != 0) {
        *hOut = p.hObjectNew;
        return NV_OK;
    }

    if (rc != 0 || p.status != NV_OK) {
        nvos21_t q;
        static uint32_t next_handle = 0x00e00001u;

        memset(&q, 0, sizeof(q));
        q.hRoot = hRoot;
        q.hObjectParent = hParent;
        q.hObjectNew = next_handle++;
        q.hClass = hClass;
        q.pAllocParms = (uint64_t)(uintptr_t)parms;
        q.paramsSize = parmsSize;
        req = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, nvos21_t);
        rc = call_ioctl(nv_fd, req, &q);
        if (rc == 0 && q.status == NV_OK) {
            *hOut = q.hObjectNew;
            return NV_OK;
        }
        return q.status ? q.status : (uint32_t)(rc < 0 ? (uint32_t)errno : 0xffffffffu);
    }

    return p.status ? p.status : 0xffffffffu;
}

static uint32_t rm_ctrl(uint32_t hCli, uint32_t hObj, uint32_t cmd,
                        void *params, uint32_t paramsSize)
{
    nvos54_t c;
    unsigned long req;
    int rc;

    memset(&c, 0, sizeof(c));
    c.hClient = hCli;
    c.hObject = hObj;
    c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)params;
    c.paramsSize = paramsSize;
    req = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, nvos54_t);
    rc = call_ioctl(nv_fd, req, &c);
    if (rc < 0)
        return (uint32_t)errno;
    return c.status;
}

static int ensure_tpp_locked(void)
{
    nv503c_alloc_t ap;
    nv503c_register_vaspace_t rv;
    uint32_t st;

    if (tpp_ready)
        return 0;
    if (nv_fd < 0 || h_client == 0 || h_subdevice == 0) {
        logmsg("cannot alloc 0x503c yet (fd=%d client=0x%x subdev=0x%x)\n",
               nv_fd, h_client, h_subdevice);
        return -1;
    }

    memset(&ap, 0, sizeof(ap));
    ap.flags = NV503C_FLAGS_TYPE_BAR1;
    st = rm_alloc(h_client, h_subdevice, NV50_THIRD_PARTY_P2P,
                  &ap, sizeof(ap), &h_tpp);
    if (st != NV_OK) {
        logmsg("RM_ALLOC 0x503c failed status=0x%x\n", st);
        return -1;
    }
    tpp_ready = 1;
    logmsg("allocated NV50_THIRD_PARTY_P2P hTPP=0x%x\n", h_tpp);

    memset(&rv, 0, sizeof(rv));
    rv.hVASpace = h_vaspace;
    st = rm_ctrl(h_client, h_tpp, NV503C_CTRL_CMD_REGISTER_VA_SPACE,
                 &rv, sizeof(rv));
    if (st != NV_OK && st != NV_ERR_INSERT_DUPLICATE_NAME) {
        logmsg("REGISTER_VA_SPACE hVASpace=0x%x failed status=0x%x\n",
               h_vaspace, st);
        return -1;
    }
    vas_registered = 1;
    logmsg("REGISTER_VA_SPACE hVASpace=0x%x token=0x%llx status=0x%x\n",
           h_vaspace, (unsigned long long)rv.vaSpaceToken, st);
    return 0;
}

static int register_vidmem_locked(uint64_t va, uint64_t size, uint32_t hMemory)
{
    nv503c_register_vidmem_t vm;
    uint32_t st;
    uint64_t reg_va;
    uint64_t reg_sz;

    if (hMemory == 0) {
        logmsg("REGISTER_VIDMEM skipped: no hMemory for va=0x%llx\n",
               (unsigned long long)va);
        return -1;
    }
    if (ensure_tpp_locked() != 0)
        return -1;

    reg_va = va & ~(GDR_PAGE - 1);
    reg_sz = round_up(size + (va - reg_va), GDR_PAGE);
    if (reg_sz == 0)
        reg_sz = GDR_PAGE;

    memset(&vm, 0, sizeof(vm));
    vm.hMemory = hMemory;
    vm.address = reg_va;
    vm.size = reg_sz;
    vm.offset = 0;
    st = rm_ctrl(h_client, h_tpp, NV503C_CTRL_CMD_REGISTER_VIDMEM,
                 &vm, sizeof(vm));
    if (st != NV_OK && st != NV_ERR_INSERT_DUPLICATE_NAME) {
        logmsg("REGISTER_VIDMEM hMemory=0x%x va=0x%llx size=0x%llx failed status=0x%x\n",
               hMemory, (unsigned long long)reg_va, (unsigned long long)reg_sz, st);
        return -1;
    }
    logmsg("REGISTER_VIDMEM hMemory=0x%x va=0x%llx size=0x%llx status=0x%x\n",
           hMemory, (unsigned long long)reg_va, (unsigned long long)reg_sz, st);
    return 0;
}

static void remember_vmm(CUdeviceptr va, size_t size,
                         CUmemGenericAllocationHandle phys, uint32_t hMemory,
                         int replaced)
{
    vmm_alloc_t *n = calloc(1, sizeof(*n));

    if (!n)
        return;
    n->va = va;
    n->size = size;
    n->phys = phys;
    n->hMemory = hMemory;
    n->replaced_cumemalloc = replaced;
    n->next = vmm_list;
    vmm_list = n;
}

static vmm_alloc_t *take_vmm(CUdeviceptr va)
{
    vmm_alloc_t **pp;
    vmm_alloc_t *n;

    for (pp = &vmm_list; *pp; pp = &(*pp)->next) {
        if ((*pp)->va == va) {
            n = *pp;
            *pp = n->next;
            return n;
        }
    }
    return NULL;
}

static void *load_cuda_sym(const char *name)
{
    void *h;
    void *s = dlsym(RTLD_NEXT, name);

    if (s)
        return s;
    h = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!h)
        h = dlopen("libcuda.so.1", RTLD_LAZY);
    if (!h)
        return NULL;
    return dlsym(h, name);
}

static void resolve_cuda(void)
{
    if (real_cuMemAlloc_v2)
        return;
    real_cuDeviceGetAttribute = load_cuda_sym("cuDeviceGetAttribute");
    real_cuCtxGetDevice = load_cuda_sym("cuCtxGetDevice");
    real_cuMemAlloc_v2 = load_cuda_sym("cuMemAlloc_v2");
    real_cuMemAlloc = load_cuda_sym("cuMemAlloc");
    real_cuMemFree_v2 = load_cuda_sym("cuMemFree_v2");
    real_cuMemFree = load_cuda_sym("cuMemFree");
    real_cuMemGetAllocationGranularity = load_cuda_sym("cuMemGetAllocationGranularity");
    real_cuMemAddressReserve = load_cuda_sym("cuMemAddressReserve");
    real_cuMemAddressFree = load_cuda_sym("cuMemAddressFree");
    real_cuMemCreate = load_cuda_sym("cuMemCreate");
    real_cuMemRelease = load_cuda_sym("cuMemRelease");
    real_cuMemMap = load_cuda_sym("cuMemMap");
    real_cuMemUnmap = load_cuda_sym("cuMemUnmap");
    real_cuMemSetAccess = load_cuda_sym("cuMemSetAccess");
    real_cuPointerSetAttribute = load_cuda_sym("cuPointerSetAttribute");
    if (!real_ioctl)
        real_ioctl = (fn_ioctl)dlsym(RTLD_NEXT, "ioctl");
}

static CUresult vmm_alloc_and_register(CUdeviceptr *dptr, size_t bytesize)
{
    CUmemAllocationProp prop;
    CUmemAccessDesc access;
    CUdevice dev = 0;
    CUdeviceptr va = 0;
    CUmemGenericAllocationHandle phys = 0;
    size_t gran = 0;
    size_t alloc_sz;
    CUresult r;
    uint32_t hMemory = 0;
    int sync = 1;

    resolve_cuda();
    if (!real_cuMemCreate || !real_cuMemMap || !real_cuMemAddressReserve ||
        !real_cuCtxGetDevice) {
        logmsg("VMM symbols missing, cannot replace cuMemAlloc\n");
        return CUDA_ERROR_NOT_SUPPORTED;
    }

    r = real_cuCtxGetDevice(&dev);
    if (r != CUDA_SUCCESS)
        return r;

    memset(&prop, 0, sizeof(prop));
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = (int)dev;

    r = real_cuMemGetAllocationGranularity(&gran, &prop,
                                           CU_MEM_ALLOC_GRANULARITY_MINIMUM);
    if (r != CUDA_SUCCESS)
        return r;
    if (gran < GDR_PAGE)
        gran = GDR_PAGE;

    alloc_sz = round_up(bytesize, gran);
    r = real_cuMemAddressReserve(&va, alloc_sz, gran, 0, 0);
    if (r != CUDA_SUCCESS)
        return r;

    pthread_mutex_lock(&lock);
    capturing_memory = 1;
    captured_hmemory = 0;
    pthread_mutex_unlock(&lock);

    r = real_cuMemCreate(&phys, alloc_sz, &prop, 0);
    if (r != CUDA_SUCCESS) {
        real_cuMemAddressFree(va, alloc_sz);
        pthread_mutex_lock(&lock);
        capturing_memory = 0;
        pthread_mutex_unlock(&lock);
        return r;
    }

    pthread_mutex_lock(&lock);
    capturing_memory = 0;
    hMemory = captured_hmemory;
    pthread_mutex_unlock(&lock);

    r = real_cuMemMap(va, alloc_sz, 0, phys, 0);
    if (r != CUDA_SUCCESS) {
        real_cuMemRelease(phys);
        real_cuMemAddressFree(va, alloc_sz);
        return r;
    }

    memset(&access, 0, sizeof(access));
    access.location = prop.location;
    access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    r = real_cuMemSetAccess(va, alloc_sz, &access, 1);
    if (r != CUDA_SUCCESS) {
        real_cuMemUnmap(va, alloc_sz);
        real_cuMemRelease(phys);
        real_cuMemAddressFree(va, alloc_sz);
        return r;
    }

    if (real_cuPointerSetAttribute)
        real_cuPointerSetAttribute(&sync, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, va);

    pthread_mutex_lock(&lock);
    register_vidmem_locked((uint64_t)va, (uint64_t)alloc_sz, hMemory);
    remember_vmm(va, alloc_sz, phys, hMemory, 1);
    pthread_mutex_unlock(&lock);

    *dptr = va;
    logmsg("cuMemAlloc -> VMM va=0x%llx size=0x%llx hMemory=0x%x\n",
           (unsigned long long)va, (unsigned long long)alloc_sz, hMemory);
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetAttribute(int *pi, CUdevice_attribute attrib, CUdevice dev)
{
    CUresult r;

    resolve_cuda();
    if (!real_cuDeviceGetAttribute)
        return CUDA_ERROR_NOT_SUPPORTED;
    r = real_cuDeviceGetAttribute(pi, attrib, dev);
    if (pi && (attrib == CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED ||
               attrib == CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED)) {
        logmsg("cuDeviceGetAttribute(%d) was %d -> 1\n", attrib, *pi);
        *pi = 1;
        return CUDA_SUCCESS;
    }
    return r;
}

CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    CUresult r = vmm_alloc_and_register(dptr, bytesize);

    if (r == CUDA_SUCCESS)
        return r;
    resolve_cuda();
    logmsg("VMM replace failed (%d), falling back to real cuMemAlloc\n", r);
    if (real_cuMemAlloc_v2)
        return real_cuMemAlloc_v2(dptr, bytesize);
    if (real_cuMemAlloc)
        return real_cuMemAlloc(dptr, bytesize);
    return r;
}

CUresult cuMemAlloc(CUdeviceptr *dptr, size_t bytesize)
{
    return cuMemAlloc_v2(dptr, bytesize);
}

CUresult cuMemCreate(CUmemGenericAllocationHandle *handle, size_t size,
                     const CUmemAllocationProp *prop, unsigned long long flags)
{
    CUresult r;
    CUmemAllocationProp copy;

    resolve_cuda();
    if (!real_cuMemCreate)
        return CUDA_ERROR_NOT_SUPPORTED;

    pthread_mutex_lock(&lock);
    capturing_memory = 1;
    captured_hmemory = 0;
    pthread_mutex_unlock(&lock);

    r = real_cuMemCreate(handle, size, prop, flags);
    if (r != CUDA_SUCCESS && prop && prop->allocFlags.gpuDirectRDMACapable) {
        copy = *prop;
        copy.allocFlags.gpuDirectRDMACapable = 0;
        logmsg("cuMemCreate RDMA-capable failed (%d), retry without flag\n", r);
        r = real_cuMemCreate(handle, size, &copy, flags);
    }

    pthread_mutex_lock(&lock);
    capturing_memory = 0;
    pthread_mutex_unlock(&lock);
    return r;
}

CUresult cuMemMap(CUdeviceptr ptr, size_t size, size_t offset,
                  CUmemGenericAllocationHandle handle, unsigned long long flags)
{
    CUresult r;
    uint32_t hMemory;

    resolve_cuda();
    if (!real_cuMemMap)
        return CUDA_ERROR_NOT_SUPPORTED;
    r = real_cuMemMap(ptr, size, offset, handle, flags);
    if (r != CUDA_SUCCESS)
        return r;

    pthread_mutex_lock(&lock);
    hMemory = captured_hmemory;
    register_vidmem_locked((uint64_t)ptr, (uint64_t)size, hMemory);
    remember_vmm(ptr, size, handle, hMemory, 0);
    pthread_mutex_unlock(&lock);
    return r;
}

static CUresult free_replaced(CUdeviceptr dptr)
{
    vmm_alloc_t *n;
    CUresult r = CUDA_SUCCESS;

    pthread_mutex_lock(&lock);
    n = take_vmm(dptr);
    pthread_mutex_unlock(&lock);
    if (!n)
        return CUDA_ERROR_NOT_SUPPORTED;

    if (n->replaced_cumemalloc) {
        if (real_cuMemUnmap)
            r = real_cuMemUnmap(n->va, n->size);
        if (real_cuMemRelease && n->phys)
            real_cuMemRelease(n->phys);
        if (real_cuMemAddressFree)
            real_cuMemAddressFree(n->va, n->size);
    }
    free(n);
    return r == CUDA_SUCCESS ? CUDA_SUCCESS : r;
}

CUresult cuMemFree_v2(CUdeviceptr dptr)
{
    CUresult r;

    resolve_cuda();
    r = free_replaced(dptr);
    if (r == CUDA_SUCCESS)
        return r;
    if (real_cuMemFree_v2)
        return real_cuMemFree_v2(dptr);
    if (real_cuMemFree)
        return real_cuMemFree(dptr);
    return r;
}

CUresult cuMemFree(CUdeviceptr dptr)
{
    return cuMemFree_v2(dptr);
}

__attribute__((constructor))
static void gdr_geforce_init(void)
{
    const char *e;

    e = getenv("GDR_GEFORCE_QUIET");
    quiet = (e && e[0] != '0');
    e = getenv("GPUDIRECT_GPU");
    if (!e)
        e = getenv("GDR_GEFORCE_GPU");
    if (e)
        target_gpu = atoi(e);

    resolve_cuda();
    hook_ready = 1;
    logmsg("loaded (GPU %d). LD_PRELOAD hook: attr116=1, cuMemAlloc->VMM, RM 0x503c register\n",
           target_gpu);
}
