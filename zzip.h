#ifndef ZZIP_H
#define ZZIP_H

#include <windef.h>

BOOL ZzipStartup(void);

VOID ZzipCleanup(void);

BOOL ZzipUnpack(LPCWSTR pwzSourceFile, LPCWSTR pwzDestPath);

BOOL ZzipPack(LPCWSTR pwzSourceList[], LPCWSTR pwzDestFile);

#endif
