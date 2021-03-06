#ifndef _DAQ_TEST_MODULE_H
#define _DAQ_TEST_MODULE_H

#include <daq_module_api.h>

#define TEST_MODULE_VERSION 1
#define TEST_MODULE_NAME    "Test"
#define TEST_MODULE_TYPE    (DAQ_TYPE_INTF_CAPABLE|DAQ_TYPE_INLINE_CAPABLE|DAQ_TYPE_MULTI_INSTANCE|DAQ_TYPE_NO_UNPRIV)

extern DAQ_ModuleAPI_t test_module;

#endif
