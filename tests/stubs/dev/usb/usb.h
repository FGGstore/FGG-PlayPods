/* Test-only field model: not a PS5 ABI definition. */
#ifndef TEST_USB_H
#define TEST_USB_H
#include <stdint.h>
#define UT_WRITE_CLASS_DEVICE 0
#define USETW(field, value) ((field) = (value))
struct test_request { unsigned bmRequestType, bRequest, wValue, wIndex, wLength; };
struct usb_device_descriptor { uint8_t bytes[18]; };
#endif
