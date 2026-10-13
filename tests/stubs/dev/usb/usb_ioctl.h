/* Test-only field model and request IDs, never used by target builds. */
#ifndef TEST_USB_IOCTL_H
#define TEST_USB_IOCTL_H
#include "usb.h"
#define USB_FS_FLAG_SINGLE_SHORT_OK 1
#define USB_FS_FLAG_FORCE_SHORT 4
#define USB_FS_OPEN 1001
#define USB_FS_START 1002
#define USB_FS_INIT 1003
#define USB_FS_UNINIT 1004
#define USB_DO_REQUEST 1005
#define USB_FS_COMPLETE 1006
#define USB_GET_DEVICEINFO 1007
#define USB_GET_DEVICE_DESC 1010
#define USB_GET_FULL_DESC 1008
#define USB_GET_IFACE_DRIVER 1009
#define USB_UNCONFIG_INDEX 255
struct usb_device_info { unsigned udi_vendorNo, udi_productNo, udi_releaseNo, udi_class, udi_subclass, udi_protocol; };
struct usb_gen_descriptor { void *ugd_data; unsigned ugd_maxlen, ugd_actlen, ugd_config_index, ugd_iface_index; };
struct usb_fs_endpoint { void **ppBuffer; uint32_t *pLength; unsigned nFrames, aFrames, status, flags, timeout; };
struct usb_fs_open { uint32_t max_bufsize; unsigned max_frames; uint16_t max_packet_length; uint8_t ep_index, ep_no; };
struct usb_fs_start { uint8_t ep_index; };
struct usb_fs_init { struct usb_fs_endpoint *pEndpoints; unsigned ep_index_max; };
struct usb_fs_uninit { unsigned unused; };
struct usb_fs_complete { uint8_t ep_index; };
struct usb_ctl_request { void *ucr_data; struct test_request ucr_request; };
#endif
