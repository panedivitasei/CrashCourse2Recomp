// cc2 - ReXGlue Recompiled Project

// Resolved via REXGLUE_ENTRYPOINT_INCLUDE_DIR (generated/cc2).
#include "cc2_init.h"

#include "cc2_app.h"

#include <rex/hook.h>

// Xbox 360 USB camera imports, unimplemented in the runtime. No-op bodies to link.
DEFINE_REX_FUNC(__imp__XUsbcamCreate)        { }
DEFINE_REX_FUNC(__imp__XUsbcamDestroy)       { }
DEFINE_REX_FUNC(__imp__XUsbcamGetState)      { }
DEFINE_REX_FUNC(__imp__XUsbcamSetConfig)     { }
DEFINE_REX_FUNC(__imp__XUsbcamSetView)       { }
DEFINE_REX_FUNC(__imp__XUsbcamSetCaptureMode){ }
DEFINE_REX_FUNC(__imp__XUsbcamReadFrame)     { }

REX_DEFINE_APP(cc2, Cc2App::Create)
