#include "zzip.h"
#include <windows.h>
#include <stdio.h>

#define TOKEN_FILE 0
#define TOKEN_DIR_BEGIN 1
#define TOKEN_DIR_END 2

#define STATUS_BUFFER_TOO_SMALL 0xC0000023

#define COMPRESSED_BUF_SIZE 0x1000
#define UNCOMPRESSED_BUF_SIZE 0x1000 - 2

DWORD (WINAPI *_RtlCompressBuffer)(
		ULONG CompressionFormat,
		PVOID SourceBuffer,
		ULONG SourceBufferLength,
		PVOID DestinationBuffer,
		ULONG DestinationBufferLength,
		ULONG TrunkSize,
		PULONG pDestinationSize,
		PVOID WorkspaceBuffer);

DWORD (WINAPI *_RtlDecompressBuffer)(
		ULONG CompressionFormat,
		PVOID DestinationBuffer,
		ULONG DestinationBufferLength,
		PVOID SourceBuffer,
		ULONG SourceBufferLength,
		PULONG pDestinationSize);

DWORD (WINAPI *_RtlGetCompressionWorkSpaceSize)(
		ULONG CompressionFormat,
		PULONG pNeededBufferSize,
		PULONG pUnknown);

LPVOID g_pWorkBuffer = NULL;

BOOL ZzipStartup(void)
{
        HMODULE hNtdll = LoadLibraryW(L"ntdll.dll");
        _RtlCompressBuffer = (void *)GetProcAddress(hNtdll, "RtlCompressBuffer");
        _RtlDecompressBuffer = (void *)GetProcAddress(hNtdll, "RtlDecompressBuffer");
        _RtlGetCompressionWorkSpaceSize = (void *)GetProcAddress(hNtdll, "RtlGetCompressionWorkSpaceSize");

	ULONG uNeedSize, uFragSize;
        _RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1, &uNeedSize, &uFragSize);
	g_pWorkBuffer = HeapAlloc(GetProcessHeap(), 0, uNeedSize);
	if (!g_pWorkBuffer) {
		FreeLibrary(hNtdll);
		return FALSE;
	}
	return TRUE;
}

VOID ZzipCleanup(void)
{
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
	FreeLibrary(hNtdll);
}

static BOOL WriteFileEntry(LPWIN32_FIND_DATAW pFindData, HANDLE hDestFile)
{
	BYTE byToken = (pFindData->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		? TOKEN_DIR_BEGIN
		: TOKEN_FILE;

	if (!WriteFile(hDestFile, &byToken, sizeof byToken, NULL, NULL))
		return FALSE;
	
	ULARGE_INTEGER uiTime = {
		.LowPart = pFindData->ftCreationTime.dwLowDateTime,
		.HighPart = pFindData->ftCreationTime.dwHighDateTime };

	DWORD dwTime =  ((LONGLONG)(uiTime.QuadPart - 116444736000000000) / 10000000);

	if (!WriteFile(hDestFile, &dwTime, sizeof dwTime, NULL, NULL))
		return FALSE;
	
	BYTE byNameLen = lstrlenW(pFindData->cFileName) * sizeof (WCHAR);

	if (!WriteFile(hDestFile, &byNameLen, sizeof byNameLen, NULL, NULL))
		return FALSE;
	
	if (!WriteFile(hDestFile, pFindData->cFileName, byNameLen, NULL, NULL))
		return FALSE;

	return TRUE;
}

static BOOL PackFile(LPWSTR pPath, DWORD dwPathLen, LPWIN32_FIND_DATAW pFindData, HANDLE hDestFile)
{
	BOOL bRetVal = FALSE;
	if (!(pFindData->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		wprintf(L"%S\n", pPath);
		HANDLE hSourceFile = CreateFileW(pPath, GENERIC_READ, 0, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hSourceFile == INVALID_HANDLE_VALUE)
			return TRUE;
		DWORD dwFileSizeHigh, dwFileSize = GetFileSize(hSourceFile, &dwFileSizeHigh);
		if (dwFileSize == INVALID_FILE_SIZE || dwFileSizeHigh != 0) {
			bRetVal = TRUE;
			goto CLOSE_FILE;
		}

		if (!WriteFileEntry(pFindData, hDestFile))
			goto CLOSE_FILE;

		BYTE baBuffer[UNCOMPRESSED_BUF_SIZE];
		DWORD dwBytesRead;
		while (ReadFile(hSourceFile, baBuffer, sizeof baBuffer, &dwBytesRead, NULL)) {
			if (dwBytesRead == 0)
				break;
			BYTE baCompressedBuffer[0x1002];
			DWORD dwCompressedSize;
			DWORD ret = _RtlCompressBuffer(COMPRESSION_FORMAT_LZNT1, baBuffer, dwBytesRead,
						baCompressedBuffer, sizeof baCompressedBuffer,
						4096, &dwCompressedSize, g_pWorkBuffer);
			WORD wCompressedSize = dwCompressedSize;
			if (!WriteFile(hDestFile, &wCompressedSize, sizeof wCompressedSize, NULL, NULL))
				goto CLOSE_FILE;
			if (!WriteFile(hDestFile, baCompressedBuffer, wCompressedSize, NULL, NULL))
				goto CLOSE_FILE;
		}
		WORD wCompressedSize = 0;
		if (!WriteFile(hDestFile, &wCompressedSize, sizeof wCompressedSize, NULL, NULL))
			goto CLOSE_FILE;

		bRetVal = TRUE;
CLOSE_FILE:
		CloseHandle(hSourceFile);

		return bRetVal;
	}

	HANDLE hFind;
	WIN32_FIND_DATAW find;

	lstrcatW(pPath + dwPathLen, L"\\*.*");
	hFind = FindFirstFileW(pPath, &find);
	if (hFind == INVALID_HANDLE_VALUE)
		return TRUE;

	if (!WriteFileEntry(pFindData, hDestFile))
		goto CLOSE_FIND;

	do {
		if (lstrcmpW(find.cFileName, L".") == 0 || lstrcmpW(find.cFileName, L"..") == 0)
			continue;

		lstrcpyW(pPath + dwPathLen, L"\\");
		lstrcpyW(pPath + dwPathLen + 1, find.cFileName);

		DWORD dwNameLen = lstrlenW(find.cFileName);
		if (!PackFile(pPath, dwPathLen + 1 + dwNameLen, &find, hDestFile))
			goto CLOSE_FIND;

	} while (FindNextFileW(hFind, &find));
	
	BYTE byToken = TOKEN_DIR_END;
	if (!WriteFile(hDestFile, &byToken, sizeof byToken, NULL, NULL))
		goto CLOSE_FIND;

	bRetVal = TRUE;
CLOSE_FIND:
	FindClose(hFind);
	return bRetVal;
}

BOOL ZzipPack(LPCWSTR pwzSourceList[], LPCWSTR pwzDestFile)
{

	HANDLE hDestFile = CreateFileW(pwzDestFile, GENERIC_WRITE, 0, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hDestFile == INVALID_HANDLE_VALUE)
		return FALSE;
	
	CHAR szMagic[2] = { 'l', 'z' };
	if (!WriteFile(hDestFile, szMagic, sizeof szMagic, NULL, NULL))
		goto fail;

	WCHAR wzFileName[MAX_PATH];

	for (int i = 0; pwzSourceList[i]; i++) {
		DWORD dwPathLen = GetFullPathNameW(pwzSourceList[i], sizeof wzFileName / sizeof(WCHAR), wzFileName, NULL);
		if (!dwPathLen)
			continue;

		HANDLE hFind;
		WIN32_FIND_DATAW find;
		hFind = FindFirstFileW(wzFileName, &find);
		if (hFind == INVALID_HANDLE_VALUE)
			continue;

		if (!PackFile(wzFileName, dwPathLen, &find, hDestFile)) {
			FindClose(hFind);
			goto fail;
		}
		FindClose(hFind);
	}
	CloseHandle(hDestFile);
	return TRUE;
fail:
	CloseHandle(hDestFile);
	DeleteFileW(pwzDestFile);
	return FALSE;
}

static BOOL ReadFileEntry(HANDLE hSourceFile, LPWSTR pName, LPDWORD pCreationTime)
{
	DWORD dwBytesRead;
	if (!ReadFile(hSourceFile, pCreationTime, sizeof *pCreationTime, &dwBytesRead, NULL))
		return FALSE;
	if (dwBytesRead != sizeof *pCreationTime)
		return FALSE;

	BYTE byNameLen;
	if (!ReadFile(hSourceFile, &byNameLen, sizeof byNameLen, &dwBytesRead, NULL))
		return FALSE;
	if (dwBytesRead != sizeof byNameLen)
		return FALSE;
	
	if (!ReadFile(hSourceFile, pName, byNameLen, &dwBytesRead, NULL))
		return FALSE;
	if (dwBytesRead != byNameLen)
		return FALSE;
	pName[byNameLen / 2] = '\0';

	return TRUE;
}

static BOOL CreateDestFile(HANDLE hSourceFile, LPWSTR pwzDestName, DWORD dwCreationTime)
{
	BOOL bRetVal = FALSE;
	DWORD dwBytesRead;

	wprintf(L"%S\n", pwzDestName);

	HANDLE hDestFile = CreateFileW(pwzDestName, GENERIC_WRITE, 0, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hDestFile == INVALID_HANDLE_VALUE) {
		// Open failed
	}

	BYTE baReadBuffer[COMPRESSED_BUF_SIZE];
	BYTE baUncompressedBuffer[UNCOMPRESSED_BUF_SIZE];
	WORD wCompressedSize;
	while (1) {
		if (!ReadFile(hSourceFile, &wCompressedSize, sizeof wCompressedSize, &dwBytesRead, NULL))
			goto CLOSE_DEST;
		if (dwBytesRead != sizeof wCompressedSize)
			goto CLOSE_DEST;

		if (wCompressedSize == 0)
			break;

		if (!ReadFile(hSourceFile, baReadBuffer, wCompressedSize, &dwBytesRead, NULL))
			goto CLOSE_DEST;
		if (dwBytesRead != wCompressedSize)
			goto CLOSE_DEST;

		if (hDestFile == INVALID_HANDLE_VALUE)
			continue;

		ULONG ulUncompressedSize;
		_RtlDecompressBuffer(COMPRESSION_FORMAT_LZNT1, baUncompressedBuffer, sizeof baUncompressedBuffer,
				baReadBuffer, wCompressedSize, &ulUncompressedSize);

		if (!WriteFile(hDestFile, baUncompressedBuffer, ulUncompressedSize, NULL, NULL)) {
			// Write failed
		}
	}
	bRetVal = TRUE;
CLOSE_DEST:
	;
	ULARGE_INTEGER uiTime =  { .QuadPart = (LONGLONG)dwCreationTime * 10000000 + 116444736000000000 };
	FILETIME ft = { .dwHighDateTime = uiTime.HighPart, .dwLowDateTime = uiTime.LowPart };
	SetFileTime(hDestFile, &ft, &ft, &ft);
	CloseHandle(hDestFile);
	return bRetVal;
}

static BOOL UnpackFile(HANDLE hSourceFile, LPWSTR pPath, DWORD dwPathLen)
{
	BOOL bRetVal = FALSE;
	DWORD dwBytesRead;

	BYTE byToken;

	while (1) {
		if (!ReadFile(hSourceFile, &byToken, sizeof byToken, &dwBytesRead, NULL))
			goto END;
		if (dwBytesRead != sizeof (byToken))
			goto END;

		WCHAR wzName[MAX_PATH];
		DWORD dwCreationTime;

		if (byToken == TOKEN_DIR_END)
			break;

		if (!ReadFileEntry(hSourceFile, wzName, &dwCreationTime))
			goto END;

		lstrcpyW(pPath + dwPathLen, L"\\");
		lstrcatW(pPath + dwPathLen, wzName);

		if (byToken == TOKEN_FILE) {
			if (!CreateDestFile(hSourceFile, pPath, dwCreationTime))
				goto END;
		}
		else if (byToken == TOKEN_DIR_BEGIN) {
			if (!CreateDirectoryW(pPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
				goto END;
			DWORD dwNameLen = lstrlenW(wzName);
			if (!UnpackFile(hSourceFile, pPath, dwPathLen + dwNameLen + 1))
				goto END;
		}
		else {
			// Invalid file format
			goto END;
		}
	}

	bRetVal = TRUE;
END:
	return bRetVal;
}

BOOL ZzipUnpack(LPCWSTR pwzSourceFile, LPCWSTR pwzDestPath)
{
	BOOL bRetVal = FALSE;
	HANDLE hSourceFile = CreateFileW(pwzSourceFile, GENERIC_READ, 0, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hSourceFile == INVALID_HANDLE_VALUE)
		return bRetVal;
	
	BYTE baMagic[2];
	DWORD dwBytesRead;
	if (!ReadFile(hSourceFile, baMagic, sizeof baMagic, &dwBytesRead, NULL))
		goto CLOSE_SOURCE;

	if (dwBytesRead != sizeof baMagic)
		goto CLOSE_SOURCE;

	if (baMagic[0] != 'l' || baMagic[1] != 'z')
		goto CLOSE_SOURCE;

	WCHAR wzFileName[MAX_PATH];
	DWORD dwPathLen = GetFullPathNameW(pwzDestPath, sizeof wzFileName / sizeof(WCHAR), wzFileName, NULL);
	if (!dwPathLen)
		goto CLOSE_SOURCE;
	
	bRetVal = UnpackFile(hSourceFile, wzFileName, dwPathLen);
CLOSE_SOURCE:
	CloseHandle(hSourceFile);
	return bRetVal;
}

