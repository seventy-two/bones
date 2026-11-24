// ImGui settings: a failed check is written to %TEMP%\shelf-assert.log
// rather than stopping the app with a dialog.
#pragma once

void ShelfAssert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ShelfAssert(#_EXPR, __FILE__, __LINE__))
