// ImGui settings: a failed check is written to %TEMP%\bones-assert.log
// rather than stopping the app with a dialog.
#pragma once

void BonesAssert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : BonesAssert(#_EXPR, __FILE__, __LINE__))
