///////////////////////////////////////////////////////////////////////////////
//
// This file is part of ntfslinkutils.
//
// Copyright (c) 2014, Jean-Philippe Steinmetz
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
// 
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include <Junction.h>
#include <memory.h>
#include <Symlink.h>
#include <strsafe.h>

#include "DataTypes.h"
#include "StringUtils.h"

using namespace libntfslinks;

fixlinkOptions Options;
fixlinkStats Stats;

/**
 * Prints a friendly message based on the given error code.
 */
void PrintErrorMessage(DWORD ErrorCode, LPCTSTR Path)
{
	switch (ErrorCode)
	{
	case ERROR_FILE_NOT_FOUND: _tprintf(TEXT("File not found: %s.\n"), Path); break;
	case ERROR_PATH_NOT_FOUND: _tprintf(TEXT("Path not found: %s.\n"), Path); break;
	case ERROR_ACCESS_DENIED: _tprintf(TEXT("Access denied: %s.\n"), Path); break;
	}
}

/**
 * Modifies the target path of all reparse points in the given path.
 *
 * @param Path The path of the reparse point or directory tree to traverse and modify.
 * @param CurDepth The current level that has been traversed in the filesystem tree.
 * @return Returns zero if the operation was successful, otherwise a non-zero value on failure.
 */
DWORD fixlink(LPCTSTR Path, int CurDepth = 0)
{
	DWORD result = 0;

	// If applicable, do not go further than the specified maximum depth
	if (Options.MaxDepth >= 0 && CurDepth > Options.MaxDepth)
	{
		return result;
	}

	// Retrieve the file attributes of Path
	WIN32_FILE_ATTRIBUTE_DATA srcAttributeData = {0};
	if (GetFileAttributesEx(Path, GetFileExInfoStandard, &srcAttributeData))
	{
		// Reparse points must be processed first as they can also be considered a directory.
		if ((srcAttributeData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			// Is this a junction or a symlink?
			if (IsJunction(Path))
			{
				// Retrieve the existing target
				TCHAR Target[MAX_PATH] = {0};
				result = GetJunctionTarget(Path, Target, sizeof(Target));
				if (result == 0)
				{
					// Perform a string replace on the target path
					TCHAR NewTarget[MAX_PATH] = {0};
					StrReplace(Target, Options.OldTargetBase, Options.NewTargetBase, NewTarget, -1, -1);

					// Delete the original junction
					result = DeleteJunction(Path);
					if (result == 0)
					{
						// Recreate the junction at the new target
						result = CreateJunction(Path, NewTarget);
						if (result == 0 && Options.bVerbose)
						{
							_tprintf(TEXT("junction %s target modified. old=%s, new=%s\n"), Path, Target, NewTarget);
						}
					}
				}
			}
			else if (IsSymlink(Path))
			{
				// Retrieve the existing target
				TCHAR Target[MAX_PATH] = {0};
				result = GetSymlinkTarget(Path, Target, sizeof(Target));
				if (result == 0)
				{
					// Perform a string replace on the target path
					TCHAR NewTarget[MAX_PATH] = {0};
					StrReplace(Target, Options.OldTargetBase, Options.NewTargetBase, NewTarget, -1, -1);

					// Delete the original symlink
					result = DeleteSymlink(Path);
					if (result == 0)
					{
						// Recreate the symlink at the new target
						result = CreateSymlink(Path, NewTarget);
						if (result == 0 && Options.bVerbose)
						{
							_tprintf(TEXT("symlink %s target modified. old=%s, new=%s\n"), Path, Target, NewTarget);
						}
					}
				}
			}
			else
			{
				result = GetLastError();
				if (result == 0)
				{
					_tprintf(TEXT("Unrecognized reparse point: %s\n"), Path);
					Stats.NumSkipped++;
				}
			}
		}
		else if ((srcAttributeData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			WIN32_FIND_DATA ffd;
			TCHAR szDir[MAX_PATH] = {0};
			HANDLE hFind;

			// The search path must include '\*'
			StringCchCopy(szDir, sizeof(szDir), Path);
			StringCchCat(szDir, sizeof(szDir), TEXT("\\*"));

			// Iterate through the list of files in the directory and fixlink each one.
			hFind = FindFirstFile(szDir, &ffd);
			if (hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					// Ignore the '.' and '..' entries
					size_t fileNameLength;
					if (FAILED(StringCchLength(ffd.cFileName, MAX_PATH, &fileNameLength)) || fileNameLength == 0 ||
						(fileNameLength == 1 && ffd.cFileName[0] == '.') ||
						(fileNameLength == 2 && ffd.cFileName[0] == '.' && ffd.cFileName[1] == '.'))
					{
						continue;
					}

					// Ignore anything that isn't a directory or reparse point
					if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
						(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
					{
						TCHAR filePath[MAX_PATH];
						StringCchCopy(filePath, sizeof(filePath), Path);
						StringCchCat(filePath, sizeof(filePath), TEXT("\\"));
						StringCchCat(filePath, sizeof(filePath), ffd.cFileName);

						result = fixlink(filePath, CurDepth+1);
					}
				} while (FindNextFile(hFind, &ffd) != 0);

				// Close the handle for the first pass
				FindClose(hFind);
			}
			else
			{
				// If we failed to be able to read the directory listing due to a access violation count it as a skip
				// instead of a complete failure.
				if (GetLastError() == ERROR_ACCESS_DENIED)
				{
					PrintErrorMessage(GetLastError(), Path);
					Stats.NumSkipped++;
				}
				else
				{
					result = GetLastError();
				}
			}
		}
	}
	else
	{
		result = GetLastError();
	}

	// Was the operation successful?
	if (result != 0)
	{
		Stats.NumFailed++;
		PrintErrorMessage(result, Path);
	}

	return result;
}

void PrintUsage()
{
	_tprintf(TEXT("Modifies the target path of all symbolic links and junctions in a given set of paths.\n\n"));
	_tprintf(TEXT("Usage: fixlink [/V] [/LEV:n] <find> <replace> <path>...\n\n"));
	_tprintf(TEXT("Options:\n"));
	_tprintf(TEXT("\t\t/LEV:n\t\tOnly copy the top n levels of the source directory tree.\n"));
	_tprintf(TEXT("\t\t/V\t\tEnable verbose output and display more information.\n"));
	_tprintf(TEXT("\t\t/VER\t\tDisplay the version and copyright information.\n"));
	_tprintf(TEXT("\t\t/?\t\tView this list of options.\n"));
}

void PrintVersion()
{
	_tprintf(TEXT("Copyright (C) 2014, Jean-Philippe Steinmetz. All rights reserved.\n"));
	_tprintf(TEXT("\n"));
	_tprintf(TEXT("Redistribution and use in source and binary forms, with or without\n"));
	_tprintf(TEXT("modification, are permitted provided that the following conditions are met:\n"));
	_tprintf(TEXT("\n"));
	_tprintf(TEXT("* Redistributions of source code must retain the above copyright notice, this\n"));
	_tprintf(TEXT("  list of conditions and the following disclaimer.\n"));
	_tprintf(TEXT("\n"));
	_tprintf(TEXT("* Redistributions in binary form must reproduce the above copyright notice,\n"));
	_tprintf(TEXT("  this list of conditions and the following disclaimer in the documentation\n"));
	_tprintf(TEXT("  and/or other materials provided with the distribution.\n"));
	_tprintf(TEXT("\n"));
	_tprintf(TEXT("THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"));
	_tprintf(TEXT("AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"));
	_tprintf(TEXT("IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\n"));
	_tprintf(TEXT("DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"));
	_tprintf(TEXT("FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"));
	_tprintf(TEXT("DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"));
	_tprintf(TEXT("SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"));
	_tprintf(TEXT("CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,\n"));
	_tprintf(TEXT("OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"));
	_tprintf(TEXT("OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"));
}

int _tmain(int argc, TCHAR* argv[])
{
	DWORD result;
	int requiredArgs = 4;
	int StartArgIdx = 4;

	// Parse the command line arguments
	TCHAR Value[1024];
	for (int i = 1; i < argc; i++)
	{
		if (StrFind(argv[i], TEXT("/VER")) >= 0 || StrFind(argv[i], TEXT("/ver")) >= 0)
		{
			PrintVersion();
			return 0;
		}
		else if (StrFind(argv[i], TEXT("/?")) >= 0)
		{
			PrintUsage();
			return 0;
		}
		else if (StrFind(argv[i], TEXT("/LEV")) >= 0 || StrFind(argv[i], TEXT("/lev")) >= 0)
		{
			memset(Value, 0, sizeof(Value));
			StringCchCopy(Value, sizeof(Value), &argv[i][5]);
			Options.MaxDepth = _ttoi(Value);
		}
		else if (StrFind(argv[i], TEXT("/V")) >= 0 || StrFind(argv[i], TEXT("/v")) >= 0)
		{
			Options.bVerbose = true;
		}
		else if (i + 1 < argc)
		{
			StringCchCopy(Options.OldTargetBase, sizeof(Options.OldTargetBase), argv[i]);
			StringCchCopy(Options.NewTargetBase, sizeof(Options.NewTargetBase), argv[i+1]);
			StartArgIdx = i + 2;
			break;
		}
	}

	// Check the minimum required arguments
	if (argc < requiredArgs)
	{
		_tprintf(TEXT("Error: Missing argument(s).\n"));
		PrintUsage();
		return 1;
	}

	// Iterate through each argument that isn't an option and execute fixlink on it
	for (int i = 1; i < argc; i++)
	{
		// Ignore options
		if (argv[i][0] == '/')
		{
			continue;
		}

		result = fixlink(argv[i]);
		if (result != 0)
		{
			// Exit on failure
			break;
		}
	}

	// Print the execution statistics
	_tprintf(TEXT("Modified: %d\n"), Stats.NumModified);
	_tprintf(TEXT("Skipped: %d\n"), Stats.NumSkipped);
	_tprintf(TEXT("Failed: %d\n"), Stats.NumFailed);

	// Make sure that if there were errors it is reflected in the result
	if (result == 0 && Stats.NumFailed > 0)
	{
		result = 1;
	}

	return result;
}
