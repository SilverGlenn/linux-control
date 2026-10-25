#pragma once

#include "ControlBuildIdentity.h"

namespace release {
inline const char *versionText()
{
    return "Control Panel " CONTROL_RELEASE_VERSION " (" CONTROL_BUILD_ID ")";
}
inline const char *buildId() { return CONTROL_BUILD_ID; }
inline const char *sourceRevision() { return CONTROL_SOURCE_REVISION; }
inline const char *sourceManifestSha256() { return CONTROL_SOURCE_MANIFEST_SHA256; }
}
