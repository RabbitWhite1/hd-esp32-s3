// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once

// Firmware version string. CI stamps it through -DFW_VERSION (see
// .github/workflows/firmware.yml): a tag builds as the tag, anything else as
// "main-<short sha>". A local build carries no stamp and reports "dev". The
// over-the-air self-update check compares this against the newest release tag,
// so it has to identify the running image exactly.
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
