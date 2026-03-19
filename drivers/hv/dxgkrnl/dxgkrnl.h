/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2022, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * Headers for internal objects
 *
 */

#ifndef _DXGKRNL_H
#define _DXGKRNL_H

#include <linux/uuid.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/hyperv.h>
#include <uapi/misc/d3dkmthk.h>
#include <linux/version.h>
#include "misc.h"
#include <uapi/misc/d3dkmthk.h>

struct dxgadapter;

/*
 * Driver private data.
 * A single /dev/dxg device is created per virtual machine.
 */
struct dxgdriver{
       struct dxgglobal        *dxgglobal;
       struct device           *dxgdev;
       struct pci_driver       pci_drv;
       struct hv_driver        vmbus_drv;
};
extern struct dxgdriver dxgdrv;

#define DXGDEV dxgdrv.dxgdev

struct dxgvmbuschannel {
       struct vmbus_channel    *channel;
       struct hv_device        *hdev;
       spinlock_t              packet_list_mutex;
       struct list_head        packet_list_head;
       struct kmem_cache       *packet_cache;
       atomic64_t              packet_request_id;
};

int dxgvmbuschannel_init(struct dxgvmbuschannel *ch, struct hv_device *hdev);
void dxgvmbuschannel_destroy(struct dxgvmbuschannel *ch);
void dxgvmbuschannel_receive(void *ctx);

/*
 * The structure defines an offered vGPU vm bus channel.
 */
struct dxgvgpuchannel {
       struct list_head        vgpu_ch_list_entry;
       struct winluid          adapter_luid;
       struct hv_device        *hdev;
};

struct dxgglobal {
       struct dxgdriver        *drvdata;
       struct dxgvmbuschannel  channel;
       struct hv_device        *hdev;
       u32                     num_adapters;
       u32                     vmbus_ver;      /* Interface version */
       struct resource         *mem;
       u64                     mmiospace_base;
       u64                     mmiospace_size;
       struct miscdevice       dxgdevice;
       struct mutex            device_mutex;

       /*
        * List of the vGPU VM bus channels (dxgvgpuchannel)
        * Protected by device_mutex
        */
       struct list_head        vgpu_ch_list_head;

       /* protects acces to the global VM bus channel */
       struct rw_semaphore     channel_lock;

       bool                    global_channel_initialized;
       bool                    async_msg_enabled;
       bool                    misc_registered;
       bool                    pci_registered;
       bool                    vmbus_registered;
};

static inline struct dxgglobal *dxggbl(void)
{
       return dxgdrv.dxgglobal;
}

int dxgglobal_init_global_channel(void);
void dxgglobal_destroy_global_channel(void);
struct vmbus_channel *dxgglobal_get_vmbus(void);
struct dxgvmbuschannel *dxgglobal_get_dxgvmbuschannel(void);
int dxgglobal_acquire_channel_lock(void);
void dxgglobal_release_channel_lock(void);

struct dxgprocess {
       /* Placeholder */
};

/*
 * The convention is that VNBus instance id is a GUID, but the host sets
 * the lower part of the value to the host adapter LUID. The function
 * provides the necessary conversion.
 */
static inline void guid_to_luid(guid_t *guid, struct winluid *luid)
{
       *luid = *(struct winluid *)&guid->b[0];
}

/*
 * VM bus interface
 *
 */

/*
 * The interface version is used to ensure that the host and the guest use the
 * same VM bus protocol. It needs to be incremented every time the VM bus
 * interface changes. DXGK_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION is
 * incremented each time the earlier versions of the interface are no longer
 * compatible with the current version.
 */
#define DXGK_VMBUS_INTERFACE_VERSION_OLD               27
#define DXGK_VMBUS_INTERFACE_VERSION                   40
#define DXGK_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION   16

void dxgvmb_initialize(void);
int dxgvmb_send_set_iospace_region(u64 start, u64 len);

int ntstatus2int(struct ntstatus status);

#ifdef DEBUG

void dxgk_validate_ioctls(void);

#define DXG_TRACE(fmt, ...)  do {                      \
       trace_printk(dev_fmt(fmt) "\n", ##__VA_ARGS__); \
}  while (0)

#define DXG_ERR(fmt, ...) do {                         \
       dev_err(DXGDEV, fmt, ##__VA_ARGS__);            \
       trace_printk("*** dxgkerror *** " dev_fmt(fmt) "\n", ##__VA_ARGS__);    \
} while (0)

#else

#define DXG_TRACE(...)
#define DXG_ERR(fmt, ...) do {                 \
       dev_err(DXGDEV, fmt, ##__VA_ARGS__);    \
} while (0)

#endif /* DEBUG */

#endif
diff --git a/drivers/hv/dxgkrnl/dxgmodule.c b/drivers/hv/dxgkrnl/dxgmodule.c
new file mode 100644
index 000000000000..de02edc4d023
--- /dev/null
 b/drivers/hv/dxgkrnl/dxgmodule.c
@@ -0,0 +1,506 @@
// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2022, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * Interface with Linux kernel, PCI driver and the VM bus driver
 *
 */

#include <linux/module.h>
#include <linux/eventfd.h>
#include <linux/hyperv.h>
#include <linux/pci.h>
#include "dxgkrnl.h"

#define PCI_VENDOR_ID_MICROSOFT                0x1414
#define PCI_DEVICE_ID_VIRTUAL_RENDER   0x008E

#undef pr_fmt
#define pr_fmt(fmt)    "dxgk: " fmt

/*
 * Interface from dxgglobal
 */

struct vmbus_channel *dxgglobal_get_vmbus(void)
{
       return dxggbl()->channel.channel;
}

struct dxgvmbuschannel *dxgglobal_get_dxgvmbuschannel(void)
{
       return &dxggbl()->channel;
}

int dxgglobal_acquire_channel_lock(void)
{
       struct dxgglobal *dxgglobal = dxggbl();

       down_read(&dxgglobal->channel_lock);
       if (dxgglobal->channel.channel == NULL) {
               DXG_ERR("Failed to acquire global channel lock");
               return -ENODEV;
       } else {
               return 0;
       }
}

void dxgglobal_release_channel_lock(void)
{
       up_read(&dxggbl()->channel_lock);
}

const struct file_operations dxgk_fops = {
       .owner = THIS_MODULE,
};

/*
 * Interface with the PCI driver
 */

/*
 * Part of the PCI config space of the compute device is used for
 * configuration data. Reading/writing of the PCI config space is forwarded
 * to the host.
 *
 * Below are offsets in the PCI config spaces for various configuration values.
 */

/* Compute device VM bus channel instance ID */
#define DXGK_VMBUS_CHANNEL_ID_OFFSET   192

/* DXGK_VMBUS_INTERFACE_VERSION (u32) */
#define DXGK_VMBUS_VERSION_OFFSET      (DXGK_VMBUS_CHANNEL_ID_OFFSET + \
                                       sizeof(guid_t))

/* Luid of the virtual GPU on the host (struct winluid) */
#define DXGK_VMBUS_VGPU_LUID_OFFSET    (DXGK_VMBUS_VERSION_OFFSET + \
                                       sizeof(u32))

/* The guest writes its capabilities to this address */
#define DXGK_VMBUS_GUESTCAPS_OFFSET    (DXGK_VMBUS_VERSION_OFFSET + \
                                       sizeof(u32))

/* Capabilities of the guest driver, reported to the host */
struct dxgk_vmbus_guestcaps {
       union {
               struct {
                       u32     wsl2            : 1;
                       u32     reserved        : 31;
               };
               u32 guest_caps;
       };
};

/*
 * A helper function to read PCI config space.
 */
static int dxg_pci_read_dwords(struct pci_dev *dev, int offset, int size,
                              void *val)
{
       int off = offset;
       int ret;
       int i;

       /* Make sure the offset and size are 32 bit aligned */
       if (offset & 3 || size & 3)
               return -EINVAL;

       for (i = 0; i < size / sizeof(int); i++) {
               ret = pci_read_config_dword(dev, off, &((int *)val)[i]);
               if (ret) {
                       DXG_ERR("Failed to read PCI config: %d", off);
                       return ret;
               }
               off += sizeof(int);
       }
       return 0;
}

static int dxg_pci_probe_device(struct pci_dev *dev,
                               const struct pci_device_id *id)
{
       int ret;
       guid_t guid;
       u32 vmbus_interface_ver = DXGK_VMBUS_INTERFACE_VERSION;
       struct winluid vgpu_luid = {};
       struct dxgk_vmbus_guestcaps guest_caps = {.wsl2 = 1};
       struct dxgglobal *dxgglobal = dxggbl();

       mutex_lock(&dxgglobal->device_mutex);

       if (dxgglobal->vmbus_ver == 0)  {
               /* Report capabilities to the host */

               ret = pci_write_config_dword(dev, DXGK_VMBUS_GUESTCAPS_OFFSET,
                                       guest_caps.guest_caps);
               if (ret)
                       goto cleanup;

               /* Negotiate the VM bus version */

               ret = pci_read_config_dword(dev, DXGK_VMBUS_VERSION_OFFSET,
                                       &vmbus_interface_ver);
               if (ret == 0 && vmbus_interface_ver != 0)
                       dxgglobal->vmbus_ver = vmbus_interface_ver;
               else
                       dxgglobal->vmbus_ver = DXGK_VMBUS_INTERFACE_VERSION_OLD;

               if (dxgglobal->vmbus_ver < DXGK_VMBUS_INTERFACE_VERSION)
                       goto read_channel_id;

               ret = pci_write_config_dword(dev, DXGK_VMBUS_VERSION_OFFSET,
                                       DXGK_VMBUS_INTERFACE_VERSION);
               if (ret)
                       goto cleanup;

               if (dxgglobal->vmbus_ver > DXGK_VMBUS_INTERFACE_VERSION)
                       dxgglobal->vmbus_ver = DXGK_VMBUS_INTERFACE_VERSION;
       }

read_channel_id:

       /* Get the VM bus channel ID for the virtual GPU */
       ret = dxg_pci_read_dwords(dev, DXGK_VMBUS_CHANNEL_ID_OFFSET,
                               sizeof(guid), (int *)&guid);
       if (ret)
               goto cleanup;

       if (dxgglobal->vmbus_ver >= DXGK_VMBUS_INTERFACE_VERSION) {
               ret = dxg_pci_read_dwords(dev, DXGK_VMBUS_VGPU_LUID_OFFSET,
                                         sizeof(vgpu_luid), &vgpu_luid);
               if (ret)
                       goto cleanup;
       }

       DXG_TRACE("Adapter channel: %pUb", &guid);
       DXG_TRACE("Vmbus interface version: %d", dxgglobal->vmbus_ver);
       DXG_TRACE("Host luid: %x-%x", vgpu_luid.b, vgpu_luid.a);

cleanup:

       mutex_unlock(&dxgglobal->device_mutex);

       if (ret)
               DXG_TRACE("err: %d",  ret);
       return ret;
}

static void dxg_pci_remove_device(struct pci_dev *dev)
{
       /* Placeholder */
}

static struct pci_device_id dxg_pci_id_table[] = {
       {
               .vendor = PCI_VENDOR_ID_MICROSOFT,
               .device = PCI_DEVICE_ID_VIRTUAL_RENDER,
               .subvendor = PCI_ANY_ID,
               .subdevice = PCI_ANY_ID
       },
       { 0 }
};

/*
 * Interface with the VM bus driver
 */

static int dxgglobal_getiospace(struct dxgglobal *dxgglobal)
{
       /* Get mmio space for the global channel */
       struct hv_device *hdev = dxgglobal->hdev;
       struct vmbus_channel *channel = hdev->channel;
       resource_size_t pot_start = 0;
       resource_size_t pot_end = -1;
       int ret;

       dxgglobal->mmiospace_size = channel->offermsg.offer.mmio_megabytes;
       if (dxgglobal->mmiospace_size == 0) {
               DXG_TRACE("Zero mmio space is offered");
               return -ENOMEM;
       }
       dxgglobal->mmiospace_size <<= 20;
       DXG_TRACE("mmio offered: %llx", dxgglobal->mmiospace_size);

       ret = vmbus_allocate_mmio(&dxgglobal->mem, hdev, pot_start, pot_end,
                                 dxgglobal->mmiospace_size, 0x10000, false);
       if (ret) {
               DXG_ERR("Unable to allocate mmio memory: %d", ret);
               return ret;
       }
       dxgglobal->mmiospace_size = dxgglobal->mem->end -
           dxgglobal->mem->start + 1;
       dxgglobal->mmiospace_base = dxgglobal->mem->start;
       DXG_TRACE("mmio allocated %llx  %llx %llx %llx",
                dxgglobal->mmiospace_base, dxgglobal->mmiospace_size,
                dxgglobal->mem->start, dxgglobal->mem->end);

       return 0;
}

int dxgglobal_init_global_channel(void)
{
       int ret = 0;
       struct dxgglobal *dxgglobal = dxggbl();

       ret = dxgvmbuschannel_init(&dxgglobal->channel, dxgglobal->hdev);
       if (ret) {
               DXG_ERR("dxgvmbuschannel_init failed: %d", ret);
               goto error;
       }

       ret = dxgglobal_getiospace(dxgglobal);
       if (ret) {
               DXG_ERR("getiospace failed: %d", ret);
               goto error;
       }

       hv_set_drvdata(dxgglobal->hdev, dxgglobal);

error:
       return ret;
}

void dxgglobal_destroy_global_channel(void)
{
       struct dxgglobal *dxgglobal = dxggbl();

       down_write(&dxgglobal->channel_lock);

       dxgglobal->global_channel_initialized = false;

       if (dxgglobal->mem) {
               vmbus_free_mmio(dxgglobal->mmiospace_base,
                               dxgglobal->mmiospace_size);
               dxgglobal->mem = NULL;
       }

       dxgvmbuschannel_destroy(&dxgglobal->channel);

       if (dxgglobal->hdev) {
               hv_set_drvdata(dxgglobal->hdev, NULL);
               dxgglobal->hdev = NULL;
       }

       up_write(&dxgglobal->channel_lock);
}

static const struct hv_vmbus_device_id dxg_vmbus_id_table[] = {
       /* Per GPU Device GUID */
       { HV_GPUP_DXGK_VGPU_GUID },
       /* Global Dxgkgnl channel for the virtual machine */
       { HV_GPUP_DXGK_GLOBAL_GUID },
       { }
};

static int dxg_probe_vmbus(struct hv_device *hdev,
                          const struct hv_vmbus_device_id *dev_id)
{
       int ret = 0;
       struct winluid luid;
       struct dxgvgpuchannel *vgpuch;
       struct dxgglobal *dxgglobal = dxggbl();

       mutex_lock(&dxgglobal->device_mutex);

       if (uuid_le_cmp(hdev->dev_type, dxg_vmbus_id_table[0].guid) == 0) {
               /* This is a new virtual GPU channel */
               guid_to_luid(&hdev->channel->offermsg.offer.if_instance, &luid);
               DXG_TRACE("vGPU channel: %pUb",
                        &hdev->channel->offermsg.offer.if_instance);
               vgpuch = kzalloc(sizeof(struct dxgvgpuchannel), GFP_KERNEL);
               if (vgpuch == NULL) {
                       ret = -ENOMEM;
                       goto error;
               }
               vgpuch->adapter_luid = luid;
               vgpuch->hdev = hdev;
               list_add_tail(&vgpuch->vgpu_ch_list_entry,
                             &dxgglobal->vgpu_ch_list_head);
       } else if (uuid_le_cmp(hdev->dev_type,
                  dxg_vmbus_id_table[1].guid) == 0) {
               /* This is the global Dxgkgnl channel */
               DXG_TRACE("Global channel: %pUb",
                        &hdev->channel->offermsg.offer.if_instance);
               if (dxgglobal->hdev) {
                       /* This device should appear only once */
                       DXG_ERR("global channel already exists");
                       ret = -EBADE;
                       goto error;
               }
               dxgglobal->hdev = hdev;
       } else {
               /* Unknown device type */
               DXG_ERR("Unknown VM bus device type");
               ret = -ENODEV;
       }

error:

       mutex_unlock(&dxgglobal->device_mutex);

       return ret;
}

static int dxg_remove_vmbus(struct hv_device *hdev)
{
       int ret = 0;
       struct dxgvgpuchannel *vgpu_channel;
       struct dxgglobal *dxgglobal = dxggbl();

       mutex_lock(&dxgglobal->device_mutex);

       if (uuid_le_cmp(hdev->dev_type, dxg_vmbus_id_table[0].guid) == 0) {
               DXG_TRACE("Remove virtual GPU channel");
               list_for_each_entry(vgpu_channel,
                                   &dxgglobal->vgpu_ch_list_head,
                                   vgpu_ch_list_entry) {
                       if (vgpu_channel->hdev == hdev) {
                               list_del(&vgpu_channel->vgpu_ch_list_entry);
                               kfree(vgpu_channel);
                               break;
                       }
               }
       } else if (uuid_le_cmp(hdev->dev_type,
                  dxg_vmbus_id_table[1].guid) == 0) {
               DXG_TRACE("Remove global channel device");
               dxgglobal_destroy_global_channel();
       } else {
               /* Unknown device type */
               DXG_ERR("Unknown device type");
               ret = -ENODEV;
       }

       mutex_unlock(&dxgglobal->device_mutex);

       return ret;
}

MODULE_DEVICE_TABLE(vmbus, dxg_vmbus_id_table);
MODULE_DEVICE_TABLE(pci, dxg_pci_id_table);

/*
 * Global driver data
 */

struct dxgdriver dxgdrv = {
       .vmbus_drv.name = KBUILD_MODNAME,
       .vmbus_drv.id_table = dxg_vmbus_id_table,
       .vmbus_drv.probe = dxg_probe_vmbus,
       .vmbus_drv.remove = dxg_remove_vmbus,
       .vmbus_drv.driver = {
               .probe_type = PROBE_PREFER_ASYNCHRONOUS,
       },
       .pci_drv.name = KBUILD_MODNAME,
       .pci_drv.id_table = dxg_pci_id_table,
       .pci_drv.probe = dxg_pci_probe_device,
       .pci_drv.remove = dxg_pci_remove_device
};

static struct dxgglobal *dxgglobal_create(void)
{
       struct dxgglobal *dxgglobal;

       dxgglobal = kzalloc(sizeof(struct dxgglobal), GFP_KERNEL);
       if (!dxgglobal)
               return NULL;

       mutex_init(&dxgglobal->device_mutex);

       INIT_LIST_HEAD(&dxgglobal->vgpu_ch_list_head);

       init_rwsem(&dxgglobal->channel_lock);

       return dxgglobal;
}

static void dxgglobal_destroy(struct dxgglobal *dxgglobal)
{
       if (dxgglobal) {
               mutex_lock(&dxgglobal->device_mutex);
               dxgglobal_destroy_global_channel();
               mutex_unlock(&dxgglobal->device_mutex);

               if (dxgglobal->vmbus_registered)
                       vmbus_driver_unregister(&dxgdrv.vmbus_drv);

               dxgglobal_destroy_global_channel();

               if (dxgglobal->pci_registered)
                       pci_unregister_driver(&dxgdrv.pci_drv);

               if (dxgglobal->misc_registered)
                       misc_deregister(&dxgglobal->dxgdevice);

               dxgglobal->drvdata->dxgdev = NULL;

               kfree(dxgglobal);
               dxgglobal = NULL;
       }
}

static int __init dxg_drv_init(void)
{
       int ret;
       struct dxgglobal *dxgglobal = NULL;

       dxgglobal = dxgglobal_create();
       if (dxgglobal == NULL) {
               pr_err("dxgglobal_init failed");
               ret = -ENOMEM;
               goto error;
       }
       dxgglobal->drvdata = &dxgdrv;

       dxgglobal->dxgdevice.minor = MISC_DYNAMIC_MINOR;
       dxgglobal->dxgdevice.name = "dxg";
       dxgglobal->dxgdevice.fops = &dxgk_fops;
       dxgglobal->dxgdevice.mode = 0666;
       ret = misc_register(&dxgglobal->dxgdevice);
       if (ret) {
               pr_err("misc_register failed: %d", ret);
               goto error;
       }
       dxgglobal->misc_registered = true;
       dxgdrv.dxgdev = dxgglobal->dxgdevice.this_device;
       dxgdrv.dxgglobal = dxgglobal;

       ret = vmbus_driver_register(&dxgdrv.vmbus_drv);
       if (ret) {
               DXG_ERR("vmbus_driver_register failed: %d", ret);
               goto error;
       }
       dxgglobal->vmbus_registered = true;

       ret = pci_register_driver(&dxgdrv.pci_drv);
       if (ret) {
               DXG_ERR("pci_driver_register failed: %d", ret);
               goto error;
       }
       dxgglobal->pci_registered = true;

       return 0;

error:
       /* This function does the cleanup */
       dxgglobal_destroy(dxgglobal);
       dxgdrv.dxgglobal = NULL;

       return ret;
}

static void __exit dxg_drv_exit(void)
{
       dxgglobal_destroy(dxgdrv.dxgglobal);
}

module_init(dxg_drv_init);
module_exit(dxg_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Microsoft Dxgkrnl virtual compute device Driver");