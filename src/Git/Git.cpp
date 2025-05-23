﻿// TortoiseGit - a Windows shell extension for easy version control

// Copyright (C) 2008-2025 - TortoiseGit

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "stdafx.h"
#include "Git.h"
#include "GitRev.h"
#include "registry.h"
#include "GitForWindows.h"
#include "UnicodeUtils.h"
#include "gitdll.h"
#include <fstream>
#include <iterator>
#include "FormatMessageWrapper.h"
#include "SmartHandle.h"
#include "MassiveGitTaskBase.h"
#include "git2/sys/filter.h"
#include "git2/sys/transport.h"
#include "git2/sys/errors.h"
#include "../libgit2/filter-filter.h"
#include "../libgit2/ssh-wintunnel.h"

constexpr static int CalculateDiffSimilarityIndexThreshold(DWORD index) noexcept
{
	if (index < 0 || index > 100)
		return 50;
	return index;
}

bool CGit::ms_bCygwinGit = (CRegDWORD(L"Software\\TortoiseGit\\CygwinHack", FALSE) == TRUE);
bool CGit::ms_bMsys2Git = (CRegDWORD(L"Software\\TortoiseGit\\Msys2Hack", FALSE) == TRUE);
int CGit::ms_iSimilarityIndexThreshold = CalculateDiffSimilarityIndexThreshold(CRegDWORD(L"Software\\TortoiseGit\\DiffSimilarityIndexThreshold", 50));
int CGit::m_LogEncode=CP_UTF8;

static LPCWSTR nextpath(const wchar_t* path, wchar_t* buf, size_t buflen)
{
	if (!path || !buf || buflen == 0)
		return nullptr;

	const wchar_t* base = path;
	const wchar_t term = (*path == L'"') ? *path++ : L';';

	for (buflen--; *path && *path != term && buflen; buflen--)
		*buf++ = *path++;

	*buf = L'\0'; /* reserved a byte via initial subtract */

	while (*path == term || *path == L';')
		++path;

	return (path != base) ? path : nullptr;
}

static CString FindFileOnPath(const CString& filename, LPCWSTR env, bool wantDirectory = false)
{
	wchar_t buf[MAX_PATH] = { 0 };

	// search in all paths defined in PATH
	while ((env = nextpath(env, buf, _countof(buf) - 1)) != nullptr && *buf)
	{
		wchar_t* pfin = buf + wcslen(buf) - 1;

		// ensure trailing slash
		if (*pfin != L'/' && *pfin != L'\\')
			wcscpy_s(++pfin, 2, L"\\"); // we have enough space left, MAX_PATH-1 is used in nextpath above

		const size_t len = wcslen(buf);

		if ((len + filename.GetLength()) < _countof(buf))
			wcscpy_s(pfin + 1, _countof(buf) - len, filename);
		else
			break;

		if (PathFileExists(buf))
		{
			if (wantDirectory)
				pfin[1] = L'\0';
			return CString(buf);
		}
	}

	return L"";
}

static BOOL FindGitPath()
{
	size_t size;
	_wgetenv_s(&size, nullptr, 0, L"PATH");
	if (!size)
		return FALSE;

	auto env = std::make_unique<wchar_t[]>(size);
	if (!env)
		return FALSE;
	_wgetenv_s(&size, env.get(), size, L"PATH");

	CString gitExeDirectory = FindFileOnPath(L"git.exe", env.get(), true);
	if (!gitExeDirectory.IsEmpty())
	{
		CGit::ms_LastMsysGitDir = gitExeDirectory;
		CGit::ms_LastMsysGitDir.TrimRight(L'\\');

		// check for (broken) scoop shim, cf. issue #4033
		if (CString contents; CGit::LoadTextFile(CGit::ms_LastMsysGitDir + L"\\git.shim", contents))
		{
			CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": found git.shim");
			int start = 0 ;
			while (start >= 0)
			{
				CString line = contents.Tokenize(L"\n", start);
				if (auto separatorPos = line.Find(L'='); separatorPos > 0 && line.Mid(0, separatorPos).Trim() == L"path")
				{
					CString path = line.Mid(separatorPos + 1).Trim();
					if (path.GetLength() >= 2 && path[0] == L'"' && path[path.GetLength() - 1] == L'"')
						path = path.Mid(1, path.GetLength() - 2);
					if (CStringUtils::EndsWithI(path, L"\\git.exe") && PathFileExists(path))
						CGit::ms_LastMsysGitDir = path.Mid(0, path.GetLength() - static_cast<int>(wcslen(L"\\git.exe")));
					break;
				}
			}
		}

		if (CStringUtils::EndsWith(CGit::ms_LastMsysGitDir, L"\\mingw32\\bin") || CStringUtils::EndsWith(CGit::ms_LastMsysGitDir, L"\\mingw64\\bin"))
		{
			// prefer cmd directory as early Git for Windows 2.x releases only had this
			CString installRoot = CGit::ms_LastMsysGitDir.Left(CGit::ms_LastMsysGitDir.GetLength() - static_cast<int>(wcslen(L"\\mingw64\\bin"))) + L"\\cmd\\git.exe";
			if (PathFileExists(installRoot))
				CGit::ms_LastMsysGitDir = CGit::ms_LastMsysGitDir.Left(CGit::ms_LastMsysGitDir.GetLength() - static_cast<int>(wcslen(L"\\mingw64\\bin"))) + L"\\cmd";
		}
		if (CStringUtils::EndsWith(CGit::ms_LastMsysGitDir, L"\\cmd"))
		{
			// often the msysgit\cmd folder is on the %PATH%, but
			// that git.exe does not work, so try to guess the bin folder
			if (PathFileExists(CGit::ms_LastMsysGitDir.Left(CGit::ms_LastMsysGitDir.GetLength() - static_cast<int>(wcslen(L"\\cmd"))) + L"\\bin\\git.exe"))
				CGit::ms_LastMsysGitDir = CGit::ms_LastMsysGitDir.Left(CGit::ms_LastMsysGitDir.GetLength() - static_cast<int>(wcslen(L"\\cmd"))) + L"\\bin";
		}
		return TRUE;
	}

	return FALSE;
}

static CString FindExecutableOnPath(const CString& executable, LPCWSTR env)
{
	CString filename = executable;

	if (!CStringUtils::EndsWith(executable, L".exe"))
		filename += L".exe";

	if (PathFileExists(filename))
		return filename;

	filename = FindFileOnPath(filename, env);
	if (!filename.IsEmpty())
		return filename;

	return executable;
}

static bool g_bSortLogical;
static bool g_bSortLocalBranchesFirst;
static bool g_bSortTagsReversed;
static git_credential_acquire_cb g_Git2CredCallback;
static git_transport_certificate_check_cb g_Git2CheckCertificateCallback;

static void GetSortOptions()
{
#ifdef GOOGLETEST_INCLUDE_GTEST_GTEST_H_
	g_bSortLogical = true;
	g_bSortLocalBranchesFirst = true;
	g_bSortTagsReversed = false;
#else
	g_bSortLogical = !CRegDWORD(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\NoStrCmpLogical", 0, false, HKEY_CURRENT_USER);
	if (g_bSortLogical)
		g_bSortLogical = !CRegDWORD(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\NoStrCmpLogical", 0, false, HKEY_LOCAL_MACHINE);
	g_bSortLocalBranchesFirst = !CRegDWORD(L"Software\\TortoiseGit\\NoSortLocalBranchesFirst", 0, false, HKEY_CURRENT_USER);
	if (g_bSortLocalBranchesFirst)
		g_bSortLocalBranchesFirst = !CRegDWORD(L"Software\\TortoiseGit\\NoSortLocalBranchesFirst", 0, false, HKEY_LOCAL_MACHINE);
	g_bSortTagsReversed = !!CRegDWORD(L"Software\\TortoiseGit\\SortTagsReversed", 0, false, HKEY_LOCAL_MACHINE);
	if (!g_bSortTagsReversed)
		g_bSortTagsReversed = !!CRegDWORD(L"Software\\TortoiseGit\\SortTagsReversed", 0, false, HKEY_CURRENT_USER);
#endif
}

static int LogicalComparePredicate(const CString &left, const CString &right)
{
	if (g_bSortLogical)
		return StrCmpLogicalW(left, right) < 0;
	return StrCmpI(left, right) < 0;
}

static int LogicalCompareReversedPredicate(const CString &left, const CString &right)
{
	return LogicalComparePredicate(right, left);
}

static int LogicalCompareBranchesPredicate(const CString &left, const CString &right)
{
	if (g_bSortLocalBranchesFirst)
	{
		const bool leftIsRemote = CStringUtils::StartsWith(left, L"remotes/");
		const bool rightIsRemote = CStringUtils::StartsWith(right, L"remotes/");

		if (leftIsRemote && !rightIsRemote)
			return false;
		else if (!leftIsRemote && rightIsRemote)
			return true;
	}
	return LogicalComparePredicate(left, right);
}

#define CALL_OUTPUT_READ_CHUNK_SIZE 1024

CString CGit::ms_LastMsysGitDir;
CString CGit::ms_MsysGitRootDir;
int CGit::ms_LastMsysGitVersion = 0;
CGit g_Git;


CGit::CGit()
{
	git_libgit2_init();
	GetCurrentDirectory(MAX_PATH, CStrBuf(m_CurrentDir, MAX_PATH));
	m_IsUseGitDLL = !!CRegDWORD(L"Software\\TortoiseGit\\UsingGitDLL",1);
	m_IsUseLibGit2 = !!CRegDWORD(L"Software\\TortoiseGit\\UseLibgit2", TRUE);
	m_IsUseLibGit2_mask = CRegDWORD(L"Software\\TortoiseGit\\UseLibgit2_mask", DEFAULT_USE_LIBGIT2_MASK);

	GetSortOptions();
	CheckMsysGitDir();
}

CGit::~CGit()
{
	if(this->m_GitDiff)
	{
		git_close_diff(m_GitDiff);
		m_GitDiff=0;
	}
	if(this->m_GitSimpleListDiff)
	{
		git_close_diff(m_GitSimpleListDiff);
		m_GitSimpleListDiff=0;
	}
	git_libgit2_shutdown();
}

bool CGit::IsBranchNameValid(const CString& branchname)
{
	if (branchname.FindOneOf(L"\"|<>") >= 0) // not valid on Windows
		return false;
	int valid = 0;
	git_branch_name_is_valid(&valid, CUnicodeUtils::GetUTF8(branchname));
	return valid == 1;
}

int CGit::RunAsync(CString cmd, PROCESS_INFORMATION& piOut, HANDLE* hReadOut, HANDLE* hErrReadOut, const CString* StdioFile)
{
	CAutoGeneralHandle hRead, hWrite, hReadErr, hWriteErr, hWriteIn, hReadIn;
	CAutoFile hStdioFile;

	SECURITY_ATTRIBUTES sa = { 0 };
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle=TRUE;
	if (!CreatePipe(hReadIn.GetPointer(), hWriteIn.GetPointer(), &sa, 0))
	{
		CString err { static_cast<LPCWSTR>(CFormatMessageWrapper()) };
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": could not open stdin pipe: %s\n", static_cast<LPCWSTR>(err.Trim()));
		return TGIT_GIT_ERROR_OPEN_PIP;
	}
	if (!CreatePipe(hRead.GetPointer(), hWrite.GetPointer(), &sa, 0))
	{
		CString err { static_cast<LPCWSTR>(CFormatMessageWrapper()) };
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": could not open stdout pipe: %s\n", static_cast<LPCWSTR>(err.Trim()));
		return TGIT_GIT_ERROR_OPEN_PIP;
	}
	if (hErrReadOut && !CreatePipe(hReadErr.GetPointer(), hWriteErr.GetPointer(), &sa, 0))
	{
		CString err { static_cast<LPCWSTR>(CFormatMessageWrapper()) };
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": could not open stderr pipe: %s\n", static_cast<LPCWSTR>(err.Trim()));
		return TGIT_GIT_ERROR_OPEN_PIP;
	}

	if(StdioFile)
		hStdioFile = CreateFile(*StdioFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	STARTUPINFO si = { 0 };
	PROCESS_INFORMATION pi = { 0 };
	si.cb=sizeof(STARTUPINFO);
	si.hStdInput = hReadIn;
	if (hErrReadOut)
		si.hStdError = hWriteErr;
	else
		si.hStdError = hWrite;
	if(StdioFile)
		si.hStdOutput=hStdioFile;
	else
		si.hStdOutput=hWrite;

	si.wShowWindow=SW_HIDE;
	si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;

	LPWSTR pEnv = m_Environment;
	const DWORD dwFlags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS;

	memset(&this->m_CurrentGitPi,0,sizeof(PROCESS_INFORMATION));

	bool startsWithGit = CStringUtils::StartsWith(cmd, L"git");
	if ((ms_bMsys2Git || ms_bCygwinGit) && startsWithGit)
	{
		if (!CStringUtils::StartsWith(cmd, L"git.exe config "))
			cmd.Replace(L'\\', L'/');
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": executing via bash: %s\n", static_cast<LPCWSTR>(cmd));
		CString tmpFile = GetTempFile();
		if (ms_bMsys2Git)
			cmd = L"/usr/bin/" + cmd;
		else if (ms_bCygwinGit)
			cmd = L"/bin/" + cmd;
		CStringUtils::WriteStringToTextFile(tmpFile, cmd);
		tmpFile.Replace(L'\\', L'/');
		cmd = L'"' + CGit::ms_LastMsysGitDir + L"\\bash.exe\" \"" + tmpFile + L'"';
	}
	else if (startsWithGit || CStringUtils::StartsWith(cmd, L"bash"))
	{
		const int firstSpace = cmd.Find(L' ');
		if (firstSpace > 0)
			cmd = L'"' + CGit::ms_LastMsysGitDir + L'\\' + cmd.Left(firstSpace) + L'"' + cmd.Mid(firstSpace);
		else
			cmd = L'"' + CGit::ms_LastMsysGitDir + L'\\' + cmd + L'"';
	}

	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": executing %s\n", static_cast<LPCWSTR>(cmd));
	if(!CreateProcess(nullptr, cmd.GetBuffer(), nullptr, nullptr, TRUE, dwFlags, pEnv, m_CurrentDir.GetBuffer(), &si, &pi))
	{
		CString err { static_cast<LPCWSTR>(CFormatMessageWrapper()) };
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": error while executing command: %s\n", static_cast<LPCWSTR>(err.Trim()));
		return TGIT_GIT_ERROR_CREATE_PROCESS;
	}

	// Close the pipe handle so the child process stops reading.
	hWriteIn.CloseHandle();

	m_CurrentGitPi = pi;
	piOut = pi;
	if(hReadOut)
		*hReadOut = hRead.Detach();
	if(hErrReadOut)
		*hErrReadOut = hReadErr.Detach();
	return 0;
}
//Must use sperate function to convert ANSI str to union code string
//Because A2W use stack as internal convert buffer.
void CGit::StringAppend(CString& str, const char* p, int code, int length)
{
	const int len = [length, p]() {
		if (length < 0)
			return SafeSizeToInt(strlen(p));
		else
			return length;
	}();
	if (len == 0)
		return;
	const int currentContentLen = str.GetLength();
	if (len >= INT_MAX / 2)
		throw std::overflow_error("CString would become too long");
	auto* buf = str.GetBuffer(SafeSizeToInt(static_cast<size_t>(len) * 2 + currentContentLen)) + currentContentLen;
	const int appendedLen = MultiByteToWideChar(code, 0, p, len, buf, len * 2);
	str.ReleaseBuffer(currentContentLen + appendedLen); // no - 1 because MultiByteToWideChar is called with a fixed length (thus no nul char included)
}

// This method was originally used to check for orphaned branches
BOOL CGit::CanParseRev(CString ref)
{
	if (ref.IsEmpty())
		ref = L"HEAD";

	// --end-of-options does not work without --verify, but we cannot use --verify becasue we also want to check for valid ranges
	CString cmdout;
	if (Run(L"git.exe rev-parse --revs-only " + ref, &cmdout, CP_UTF8))
		return FALSE;
	if(cmdout.IsEmpty())
		return FALSE;

	return TRUE;
}

// Checks if we have an orphaned HEAD
BOOL CGit::IsInitRepos()
{
	CGitHash hash;
	if (GetHash(hash, L"HEAD") != 0)
		return FALSE;
	return hash.IsEmpty() ? TRUE : FALSE;
}

DWORD WINAPI CGit::AsyncReadStdErrThread(LPVOID lpParam)
{
	auto pDataArray = static_cast<ASYNCREADSTDERRTHREADARGS*>(lpParam);

	DWORD readnumber;
	char data[CALL_OUTPUT_READ_CHUNK_SIZE];
	while (ReadFile(pDataArray->fileHandle, data, CALL_OUTPUT_READ_CHUNK_SIZE, &readnumber, nullptr))
	{
		if (pDataArray->pcall->OnOutputErrData(data,readnumber))
			break;
	}

	return 0;
}

#ifdef _MFC_VER
void CGit::KillRelatedThreads(CWinThread* thread)
{
	CAutoLocker lock(m_critSecThreadMap);
	auto it = m_AsyncReadStdErrThreadMap.find(thread->m_nThreadID);
	if (it != m_AsyncReadStdErrThreadMap.cend())
	{
		TerminateThread(it->second, static_cast<DWORD>(-1));
		m_AsyncReadStdErrThreadMap.erase(it);
	}
	TerminateThread(thread->m_hThread, static_cast<DWORD>(-1));
}
#endif

int CGit::Run(CGitCall& pcall)
{
	PROCESS_INFORMATION pi;
	CAutoGeneralHandle hRead, hReadErr;
	if (RunAsync(pcall.GetCmd(), pi, hRead.GetPointer(), hReadErr.GetPointer()))
		return TGIT_GIT_ERROR_CREATE_PROCESS;

	CAutoGeneralHandle piThread(std::move(pi.hThread));
	CAutoGeneralHandle piProcess(std::move(pi.hProcess));

	ASYNCREADSTDERRTHREADARGS threadArguments;
	threadArguments.fileHandle = hReadErr;
	threadArguments.pcall = &pcall;
	CAutoGeneralHandle thread = CreateThread(nullptr, 0, AsyncReadStdErrThread, &threadArguments, 0, nullptr);

	if (thread)
	{
		CAutoLocker lock(m_critSecThreadMap);
		m_AsyncReadStdErrThreadMap[GetCurrentThreadId()] = thread;
	}

	DWORD readnumber;
	char data[CALL_OUTPUT_READ_CHUNK_SIZE];
	bool bAborted=false;
	while (ReadFile(hRead, data, CALL_OUTPUT_READ_CHUNK_SIZE, &readnumber, nullptr))
	{
		// TODO: when OnOutputData() returns 'true', abort git-command. Send CTRL-C signal?
		if(!bAborted)//For now, flush output when command aborted.
			if(pcall.OnOutputData(data,readnumber))
				bAborted=true;
	}
	if(!bAborted)
		pcall.OnEnd();

	if (thread)
	{
		WaitForSingleObject(thread, INFINITE);

		CAutoLocker lock(m_critSecThreadMap);
		m_AsyncReadStdErrThreadMap.erase(GetCurrentThreadId());
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitcode =0;

	if(!GetExitCodeProcess(pi.hProcess,&exitcode))
	{
		CString err { static_cast<LPCWSTR>(CFormatMessageWrapper()) };
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": could not get exit code: %s\n", static_cast<LPCWSTR>(err.Trim()));
		return TGIT_GIT_ERROR_GET_EXIT_CODE;
	}
	else
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": process exited: %d\n", exitcode);

	return exitcode;
}
class CGitCall_ByteVector : public CGitCall
{
public:
	CGitCall_ByteVector(CString cmd,BYTE_VECTOR* pvector, BYTE_VECTOR* pvectorErr = nullptr) : CGitCall(cmd),m_pvector(pvector), m_pvectorErr(pvectorErr) {}
	bool OnOutputData(const char* data, size_t size) override
	{
		ASSERT(data);
		if (!m_pvector || size == 0)
			return false;
		const size_t oldsize = m_pvector->size();
		size_t newLength;
		if (SizeTAdd(oldsize, size, &newLength) != S_OK)
			return false;
		m_pvector->resize(newLength);
		memcpy(&*(m_pvector->begin()+oldsize),data,size);
		return false;
	}
	bool OnOutputErrData(const char* data, size_t size) override
	{
		ASSERT(data);
		if (!m_pvectorErr || size == 0)
			return false;
		const size_t oldsize = m_pvectorErr->size();
		size_t newLength;
		if (SizeTAdd(oldsize, size, &newLength) != S_OK)
			return false;
		m_pvectorErr->resize(newLength);
		memcpy(&*(m_pvectorErr->begin() + oldsize), data, size);
		return false;
	}
	BYTE_VECTOR* m_pvector;
	BYTE_VECTOR* m_pvectorErr;
};
int CGit::Run(CString cmd,BYTE_VECTOR *vector, BYTE_VECTOR *vectorErr)
{
	CGitCall_ByteVector call(cmd, vector, vectorErr);
	return Run(call);
}
int CGit::Run(CString cmd, CString* output, int code)
{
	CString err;
	const int ret = Run(cmd, output, &err, code);

	if (output && !err.IsEmpty())
	{
		if (!output->IsEmpty())
			*output += L'\n';
		*output += err;
	}

	return ret;
}
int CGit::Run(CString cmd, CString* output, CString* outputErr, int code)
{
	BYTE_VECTOR vector, vectorErr;
	int ret;
	if (outputErr)
		ret = Run(cmd, &vector, &vectorErr);
	else
		ret = Run(cmd, &vector);

	vector.push_back(0);
	if (output)
		StringAppend(*output, vector.data(), code);

	if (outputErr)
	{
		vectorErr.push_back(0);
		StringAppend(*outputErr, vectorErr.data(), code);
	}

	return ret;
}

CString CGit::GetUserName()
{
	CEnvironment env;
	env.CopyProcessEnvironment();
	CString envname = env.GetEnv(L"GIT_AUTHOR_NAME");
	if (!envname.IsEmpty())
		return envname;

	if (ms_LastMsysGitVersion >= ConvertVersionToInt(2, 22, 0))
		if (auto authorname = GetConfigValue(L"author.name"); !authorname.IsEmpty())
			return authorname;

	return GetConfigValue(L"user.name");
}
CString CGit::GetUserEmail()
{
	CEnvironment env;
	env.CopyProcessEnvironment();
	CString envmail = env.GetEnv(L"GIT_AUTHOR_EMAIL");
	if (!envmail.IsEmpty())
		return envmail;

	if (ms_LastMsysGitVersion >= ConvertVersionToInt(2, 22, 0))
		if (auto authormail = GetConfigValue(L"author.email"); !authormail.IsEmpty())
			return authormail;

	return GetConfigValue(L"user.email");
}

CString CGit::GetCommitterName()
{
	CEnvironment env;
	env.CopyProcessEnvironment();
	CString envname = env.GetEnv(L"GIT_COMMITTER_NAME");
	if (!envname.IsEmpty())
		return envname;

	if (ms_LastMsysGitVersion >= ConvertVersionToInt(2, 22, 0))
		if (auto committername = GetConfigValue(L"committer.name"); !committername.IsEmpty())
			return committername;

	return GetConfigValue(L"user.name");
}

CString CGit::GetCommitterEmail()
{
	CEnvironment env;
	env.CopyProcessEnvironment();
	CString envmail = env.GetEnv(L"GIT_AUTHOR_EMAIL");
	if (!envmail.IsEmpty())
		return envmail;

	if (ms_LastMsysGitVersion >= ConvertVersionToInt(2, 22, 0))
		if (auto committermail = GetConfigValue(L"committer.email"); !committermail.IsEmpty())
			return committermail;

	return GetConfigValue(L"user.email");
}

CString CGit::GetConfigValue(const CString& name, const CString& def, bool wantBool)
{
	if(this->m_IsUseGitDLL)
	{
		CAutoLocker lock(g_Git.m_critGitDllSec);

		try
		{
			CheckAndInitDll();
		}catch(...)
		{
		}
		CStringA key, value;
		key =  CUnicodeUtils::GetUTF8(name);

		try
		{
			if (git_get_config(key, CStrBufA(value, 4096), 4096))
				return def;
		}
		catch (const char *msg)
		{
			::MessageBox(nullptr, L"Could not get config.\nlibgit reports:\n" + CUnicodeUtils::GetUnicode(msg), L"TortoiseGit", MB_OK | MB_ICONERROR);
			return def;
		}

		return CUnicodeUtils::GetUnicode(value);
	}
	else
	{
		CString cmd;
		cmd.Format(L"git.exe config%s %s", wantBool ? L" --bool" : L"", static_cast<LPCWSTR>(name));
		CString configValue;
		if (Run(cmd, &configValue, nullptr, CP_UTF8))
			return def;
		if (configValue.IsEmpty())
			return configValue;
		return configValue.Left(configValue.GetLength() - 1); // strip last newline character
	}
}

bool CGit::GetConfigValueBool(const CString& name, const bool def)
{
	CString configValue = GetConfigValue(name, def ? L"true" : L"false", true);
	configValue.MakeLower();
	configValue.Trim();
	if (configValue == L"true" || configValue == L"on" || configValue == L"yes" || StrToInt(configValue) != 0)
		return true;
	else
		return false;
}

int CGit::GetConfigValueInt32(const CString& name, const int def)
{
	CString configValue = GetConfigValue(name);
	int value = def;
	if (!git_config_parse_int32(&value, CUnicodeUtils::GetUTF8(configValue)))
		return value;
	return def;
}

int CGit::SetConfigValue(const CString& key, const CString& value, CONFIG_TYPE type)
{
	if(this->m_IsUseGitDLL)
	{
		CAutoLocker lock(g_Git.m_critGitDllSec);

		try
		{
			CheckAndInitDll();
		}catch(...)
		{
		}
		CStringA keya, valuea;
		keya = CUnicodeUtils::GetUTF8(key);
		valuea = CUnicodeUtils::GetUTF8(value);

		try
		{
			return git_set_config(keya, valuea, type);
		}
		catch (const char *msg)
		{
			::MessageBox(nullptr, L"Could not set config.\nlibgit reports:\n" + CUnicodeUtils::GetUnicode(msg), L"TortoiseGit", MB_OK | MB_ICONERROR);
			return -1;
		}
	}
	else
	{
		CString cmd;
		CString option;
		switch(type)
		{
		case CONFIG_GLOBAL:
			option = L"--global";
			break;
		case CONFIG_SYSTEM:
			option = L"--system";
			break;
		default:
			break;
		}
		CString mangledValue = value;
		mangledValue.Replace(L"\\\"", L"\\\\\"");
		mangledValue.Replace(L"\"", L"\\\"");
		cmd.Format(L"git.exe config %s %s \"%s\"", static_cast<LPCWSTR>(option), static_cast<LPCWSTR>(key), static_cast<LPCWSTR>(mangledValue));
		CString out;
		if (Run(cmd, &out, nullptr, CP_UTF8))
			return -1;
	}
	return 0;
}

int CGit::UnsetConfigValue(const CString& key, CONFIG_TYPE type)
{
	if(this->m_IsUseGitDLL)
	{
		CAutoLocker lock(g_Git.m_critGitDllSec);

		try
		{
			CheckAndInitDll();
		}catch(...)
		{
		}
		CStringA keya;
		keya = CUnicodeUtils::GetUTF8(key);

		try
		{
			return git_set_config(keya, nullptr, type);
		}
		catch (const char *msg)
		{
			::MessageBox(nullptr, L"Could not unset config.\nlibgit reports:\n" + CUnicodeUtils::GetUnicode(msg), L"TortoiseGit", MB_OK | MB_ICONERROR);
			return -1;
		}
	}
	else
	{
		CString cmd;
		CString option;
		switch(type)
		{
		case CONFIG_GLOBAL:
			option = L"--global";
			break;
		case CONFIG_SYSTEM:
			option = L"--system";
			break;
		default:
			break;
		}
		cmd.Format(L"git.exe config %s --unset %s", static_cast<LPCWSTR>(option), static_cast<LPCWSTR>(key));
		CString out;
		if (Run(cmd, &out, nullptr, CP_UTF8))
			return -1;
	}
	return 0;
}

int CGit::ApplyPatchToIndex(const CString& patchPath, CString* out)
{
	CString cmd;
	cmd.Format(L"git.exe apply --cached -- \"%s\"", static_cast<LPCWSTR>(patchPath));
	return Run(cmd, out, CP_UTF8);
}

int CGit::ApplyPatchToIndexReverse(const CString& patchPath, CString* out)
{
	CString cmd;
	cmd.Format(L"git.exe apply --cached -R -- \"%s\"", static_cast<LPCWSTR>(patchPath));
	return Run(cmd, out, CP_UTF8);
}

CString CGit::GetCurrentBranch(bool fallback)
{
	CString output;
	//Run(L"git.exe branch", &branch);

	const int result = GetCurrentBranchFromFile(m_CurrentDir, output, fallback);
	if (result != 0 && ((result == 1 && !fallback) || result != 1))
		return L"(no branch)";
	else
		return output;
}

void CGit::GetRemoteTrackedBranch(const CString& localBranch, CString& pullRemote, CString& pullBranch)
{
	if (localBranch.IsEmpty())
		return;

	CString configName;
	configName.Format(L"branch.%s.remote", static_cast<LPCWSTR>(localBranch));
	pullRemote =  GetConfigValue(configName);

	//Select pull-branch from current branch
	configName.Format(L"branch.%s.merge", static_cast<LPCWSTR>(localBranch));
	pullBranch = StripRefName(GetConfigValue(configName));
}

void CGit::GetRemoteTrackedBranchForHEAD(CString& remote, CString& branch)
{
	CString refName;
	if (GetCurrentBranchFromFile(m_CurrentDir, refName))
		return;
	GetRemoteTrackedBranch(StripRefName(refName), remote, branch);
}

void CGit::GetRemotePushBranch(const CString& localBranch, CString& pushRemote, CString& pushBranch)
{
	if (localBranch.IsEmpty())
		return;

	CString configName;

	configName.Format(L"branch.%s.pushremote", static_cast<LPCWSTR>(localBranch));
	pushRemote = g_Git.GetConfigValue(configName);
	if (pushRemote.IsEmpty())
	{
		pushRemote = g_Git.GetConfigValue(L"remote.pushdefault");
		if (pushRemote.IsEmpty())
		{
			configName.Format(L"branch.%s.remote", static_cast<LPCWSTR>(localBranch));
			pushRemote = g_Git.GetConfigValue(configName);
		}
	}

	configName.Format(L"branch.%s.pushbranch", static_cast<LPCWSTR>(localBranch));
	pushBranch = g_Git.GetConfigValue(configName); // do not strip branch name (for gerrit), see issue #1609)
	if (pushBranch.IsEmpty())
	{
		configName.Format(L"branch.%s.merge", static_cast<LPCWSTR>(localBranch));
		pushBranch = CGit::StripRefName(g_Git.GetConfigValue(configName));
	}
}

CString CGit::GetFullRefName(const CString& shortRefName)
{
	CString refName;
	CString cmd;
	cmd.Format(L"git.exe rev-parse --symbolic-full-name --verify --end-of-options %s", static_cast<LPCWSTR>(shortRefName));
	if (Run(cmd, &refName, nullptr, CP_UTF8) != 0)
		return CString();//Error
	return refName.TrimRight();
}

CString CGit::StripRefName(CString refName)
{
	if (CStringUtils::StartsWith(refName, L"refs/heads/"))
		refName = refName.Mid(static_cast<int>(wcslen(L"refs/heads/")));
	else if (CStringUtils::StartsWith(refName, L"refs/"))
		refName = refName.Mid(static_cast<int>(wcslen(L"refs/")));
	return refName.TrimRight();
}

int CGit::GetCurrentBranchFromFile(const CString &sProjectRoot, CString &sBranchOut, bool fallback)
{
	// read current branch name like git-gui does, by parsing the .git/HEAD file directly

	if ( sProjectRoot.IsEmpty() )
		return -1;

	CString sDotGitPath;
	if (!GitAdminDir::GetWorktreeAdminDirPath(sProjectRoot, sDotGitPath))
		return -1;

	CString sHeadFile = sDotGitPath + L"HEAD";

	CAutoFILE pFile = _wfsopen(sHeadFile.GetString(), L"r", SH_DENYWR);
	if (!pFile)
		return -1;

	char s[MAX_PATH] = {0};
	fgets(s, sizeof(s), pFile);

	const char *pfx = "ref: refs/heads/";
	const size_t len = strlen(pfx);

	if ( !strncmp(s, pfx, len) )
	{
		//# We're on a branch.  It might not exist.  But
		//# HEAD looks good enough to be a branch.
		CStringA utf8Branch(s + len);
		sBranchOut = CUnicodeUtils::GetUnicode(utf8Branch);
		sBranchOut.TrimRight(L" \r\n\t");

		if ( sBranchOut.IsEmpty() )
			return -1;
	}
	else if (fallback)
	{
		std::string_view utf8Hash{s};
		CStringUtils::TrimRight(utf8Hash);
		bool ok = false;
		CGitHash hash = CGitHash::FromHexStr(utf8Hash, &ok);
		if (ok)
			sBranchOut = hash.ToString();
		else
			//# Assume this is a detached head.
			sBranchOut = L"HEAD";
		return 1;
	}
	else
	{
		//# Assume this is a detached head.
		sBranchOut = "HEAD";

		return 1;
	}

	return 0;
}

CString CGit::GetLogCmd(CString range, const CTGitPath* path, int mask, CFilterData* Filter, int logOrderBy)
{
	CString param;

	if(mask& LOG_INFO_STAT )
		param += L" --numstat";
	if(mask& LOG_INFO_FILESTATE)
		param += L" --raw";

	if(mask& LOG_INFO_BOUNDARY)
		param += L" --left-right --boundary";

	if(mask& CGit::LOG_INFO_ALL_BRANCH)
	{
		param += L" --all";
		if ((mask & LOG_INFO_ALWAYS_APPLY_RANGE) == 0)
			range.Empty();
	}

	if (mask& CGit::LOG_INFO_BASIC_REFS)
	{
		param += L" --branches";
		param += L" --tags";
		param += L" --remotes";
		param += L" --glob=stas[h]"; // require at least one glob operator
		param += L" --glob=bisect";
		if ((mask & LOG_INFO_ALWAYS_APPLY_RANGE) == 0)
			range.Empty();
	}

	if(mask & CGit::LOG_INFO_LOCAL_BRANCHES)
	{
		param += L" --branches";
		if ((mask & LOG_INFO_ALWAYS_APPLY_RANGE) == 0)
			range.Empty();
	}

	if(mask& CGit::LOG_INFO_DETECT_COPYRENAME)
		param.AppendFormat(L" -C%d%%", ms_iSimilarityIndexThreshold);

	if(mask& CGit::LOG_INFO_DETECT_RENAME )
		param.AppendFormat(L" -M%d%%", ms_iSimilarityIndexThreshold);

	if(mask& CGit::LOG_INFO_FIRST_PARENT )
		param += L" --first-parent";

	if(mask& CGit::LOG_INFO_NO_MERGE )
		param += L" --no-merges";

	if (mask & CGit::LOG_INFO_FULL_HISTORY)
		param += " --full-history";
	else
		param += " --parents"; // cf. issue #3728

	if(mask& CGit::LOG_INFO_FOLLOW)
		param += L" --follow";

	if(mask& CGit::LOG_INFO_SHOW_MERGEDFILE)
		param += L" -c";

	if(mask& CGit::LOG_INFO_FULL_DIFF)
		param += L" --full-diff";

	if(mask& CGit::LOG_INFO_SIMPILFY_BY_DECORATION)
		param += L" --simplify-by-decoration";

	if (mask & CGit::LOG_INFO_SPARSE)
		param += L" --sparse";

	if (Filter)
	{
		if (Filter->m_NumberOfLogsScale >= CFilterData::SHOW_LAST_N_YEARS)
		{
			CTime now = CTime::GetCurrentTime();
			CTime time = CTime(now.GetYear(), now.GetMonth(), now.GetDay(), 0, 0, 0);
			__time64_t substract = 86400;
			CString scale;
			switch (Filter->m_NumberOfLogsScale)
			{
			case CFilterData::SHOW_LAST_N_YEARS:
				substract *= 365;
				break;
			case CFilterData::SHOW_LAST_N_MONTHS:
				substract *= 30;
				break;
			case CFilterData::SHOW_LAST_N_WEEKS:
				substract *= 7;
				break;
			}
			Filter->m_From = static_cast<DWORD>(time.GetTime()) - (Filter->m_NumberOfLogs * substract);
		}
		if (Filter->m_NumberOfLogsScale == CFilterData::SHOW_LAST_N_COMMITS)
			param.AppendFormat(L" -n%ld", Filter->m_NumberOfLogs);
		else if (Filter->m_NumberOfLogsScale >= CFilterData::SHOW_LAST_SEL_DATE  && Filter->m_From > 0)
			param.AppendFormat(L" --max-age=%I64u", Filter->m_From);
	}

	if( Filter && (Filter->m_To != -1))
		param.AppendFormat(L" --min-age=%I64u", Filter->m_To);

	if (logOrderBy == LOG_ORDER_TOPOORDER || (mask & CGit::LOG_ORDER_TOPOORDER))
		param += L" --topo-order";
	else if (logOrderBy == LOG_ORDER_DATEORDER)
		param += L" --date-order";
	else if (logOrderBy == LOG_ORDER_AUTHORDATEORDER)
		param += L" --author-date-order";

	CString cmd;
	CString file;
	if (path)
		file.Format(L" \"%s\"", static_cast<LPCWSTR>(path->GetGitPathString()));
	// gitdll.dll:setup_revisions() only looks at args[1] and greater. To account for this, pass a dummy parameter in the 0th place
	cmd.Format(L"-z%s %s --%s", static_cast<LPCWSTR>(param), static_cast<LPCWSTR>(range), static_cast<LPCWSTR>(file));

	return cmd;
}
#define BUFSIZE 512
void GetTempPath(CString &path)
{
	wchar_t lpPathBuffer[BUFSIZE] = { 0 };
	const DWORD dwBufSize = BUFSIZE;
	const DWORD dwRetVal = GetTortoiseGitTempPath(dwBufSize, lpPathBuffer);
	if (dwRetVal > dwBufSize || (dwRetVal == 0))
		path.Empty();
	path.Format(L"%s", lpPathBuffer);
}
CString GetTempFile()
{
	wchar_t lpPathBuffer[BUFSIZE] = { 0 };
	const DWORD dwBufSize = BUFSIZE;
	wchar_t szTempName[BUFSIZE] = { 0 };

	auto dwRetVal = GetTortoiseGitTempPath(dwBufSize, lpPathBuffer);
	if (dwRetVal > dwBufSize || (dwRetVal == 0))
		return L"";

	 // Create a temporary file.
	if (!GetTempFileName(lpPathBuffer,			// directory for tmp files
								TEXT("Patch"),	// temp file name prefix
								0,				// create unique name
								szTempName))	// buffer for name
		return L"";

	return CString(szTempName);
}

DWORD GetTortoiseGitTempPath(DWORD nBufferLength, LPWSTR lpBuffer)
{
	const DWORD result = ::GetTempPath(nBufferLength, lpBuffer);
	if (result == 0) return 0;
	if (!lpBuffer || (result + 13 > nBufferLength))
	{
		if (lpBuffer)
			lpBuffer[0] = '\0';
		return result + 13;
	}

	wcscat_s(lpBuffer, nBufferLength, L"TortoiseGit\\");
	CreateDirectory(lpBuffer, nullptr);

	return result + 13;
}

int CGit::RunLogFile(CString cmd, const CString &filename, CString *stdErr)
{
	PROCESS_INFORMATION pi;
	CAutoGeneralHandle hReadErr;
	if (RunAsync(cmd, pi, nullptr, hReadErr.GetPointer(), &filename))
		return TGIT_GIT_ERROR_CREATE_PROCESS;

	CAutoGeneralHandle piThread(std::move(pi.hThread));
	CAutoGeneralHandle piProcess(std::move(pi.hProcess));

	BYTE_VECTOR stderrVector;
	CGitCall_ByteVector pcall(L"", nullptr, &stderrVector);
	ASYNCREADSTDERRTHREADARGS threadArguments;
	threadArguments.fileHandle = hReadErr;
	threadArguments.pcall = &pcall;
	CAutoGeneralHandle thread = CreateThread(nullptr, 0, AsyncReadStdErrThread, &threadArguments, 0, nullptr);

	if (thread)
	{
		CAutoLocker lock(m_critSecThreadMap);
		m_AsyncReadStdErrThreadMap[GetCurrentThreadId()] = thread;
	}

	WaitForSingleObject(pi.hProcess,INFINITE);

	if (thread)
	{
		WaitForSingleObject(thread, INFINITE);

		CAutoLocker lock(m_critSecThreadMap);
		m_AsyncReadStdErrThreadMap.erase(GetCurrentThreadId());
	}

	stderrVector.push_back(0);
	if (stdErr)
		StringAppend(*stdErr, stderrVector.data(), CP_UTF8);

	DWORD exitcode = 0;
	if (!GetExitCodeProcess(pi.hProcess, &exitcode))
	{
		CString err { static_cast<LPCWSTR>(CFormatMessageWrapper()) };
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": could not get exit code: %s\n", static_cast<LPCWSTR>(err.Trim()));
		return TGIT_GIT_ERROR_GET_EXIT_CODE;
	}
	else
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": process exited: %d\n", exitcode);

	return exitcode;
}

CAutoRepository CGit::GetGitRepository() const
{
	return CAutoRepository(GetGitPathStringA(m_CurrentDir));
}

int CGit::GetHash(git_repository * repo, CGitHash &hash, const CString& friendname, bool skipFastCheck /* = false */)
{
	ATLASSERT(repo);

	// no need to look up a ref if it's already an OID
	if (!skipFastCheck)
	{
		bool isHash = false;
		hash = CGitHash::FromHexStr(friendname, &isHash);
		if (isHash)
			return 0;
	}

	const int isHeadOrphan = git_repository_head_unborn(repo);
	if (isHeadOrphan != 0)
	{
		hash.Empty();
		if (isHeadOrphan == 1)
		{
			if (friendname == GitRev::GetHead()) // special check for unborn branch: if not requesting HEAD, do normal commit lookup
				return 0;
		}
		else
			return -1;
	}

	CAutoObject gitObject;
	if (git_revparse_single(gitObject.GetPointer(), repo, CUnicodeUtils::GetUTF8(friendname)))
		return -1;

	hash = git_object_id(gitObject);

	return 0;
}

int CGit::GetHash(CGitHash &hash, const CString& friendname)
{
	// no need to look up a ref if it's already an OID
	bool hashOk = false;
	hash = CGitHash::FromHexStr(friendname, &hashOk);
	if (hashOk)
		return 0;

	if (m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		return GetHash(repo, hash, friendname, true);
	}
	else
	{
		if (friendname.IsEmpty())
		{
			gitLastErr.Empty();
			return -1;
		}
		CString branch = FixBranchName(friendname);
		if (friendname == L"FETCH_HEAD" && branch.IsEmpty())
			branch = friendname;
		CString cmd;
		cmd.Format(L"git.exe rev-parse --verify --end-of-options %s", static_cast<LPCWSTR>(branch));
		gitLastErr.Empty();
		const int ret = Run(cmd, &gitLastErr, nullptr, CP_UTF8);
		hash = CGitHash::FromHexStr(gitLastErr.Trim());
		if (ret == 0)
			gitLastErr.Empty();
		else if (friendname == L"HEAD") // special check for unborn branch
		{
			CString currentbranch;
			if (GetCurrentBranchFromFile(m_CurrentDir, currentbranch))
				return -1;
			gitLastErr.Empty();
			return 0;
		}
		return ret;
	}
}

int CGit::GetInitAddList(CTGitPathList& outputlist, bool getStagingStatus)
{
	BYTE_VECTOR cmdout;

	outputlist.Clear();
	if (Run(L"git.exe ls-files -s -t -z", &cmdout))
		return -1;

	if (outputlist.ParserFromLsFile(cmdout))
		return -1;
	for(int i = 0; i < outputlist.GetCount(); ++i)
		const_cast<CTGitPath&>(outputlist[i]).m_Action = CTGitPath::LOGACTIONS_ADDED;

	if (getStagingStatus)
	{
		BYTE_VECTOR cmdunstagedout;
		for (int i = 0; i < outputlist.GetCount(); ++i)
			const_cast<CTGitPath&>(outputlist[i]).m_stagingStatus = CTGitPath::StagingStatus::TotallyStaged;
		if (Run(L"git.exe diff-files --raw --numstat -C -M -z --", &cmdunstagedout))
			return -1;

		CTGitPathList unstaged;
		unstaged.ParserFromLog(cmdunstagedout);
		// File shows up both in the output of ls-files and diff-files: partially staged (typically modified after being added)
		for (int j = 0; j < unstaged.GetCount(); ++j)
		{
			CString path = unstaged[j].GetGitPathString();
			outputlist.UpdateStagingStatusFromPath(path, CTGitPath::StagingStatus::PartiallyStaged); // TODO: This is inefficient
		}
	}
	return 0;
}
int CGit::GetCommitDiffList(const CString &rev1, const CString &rev2, CTGitPathList &outputlist, bool ignoreSpaceAtEol, bool ignoreSpaceChange, bool ignoreAllSpace , bool ignoreBlankLines)
{
	CString cmd;
	CString ignore;
	if (ignoreSpaceAtEol)
		ignore += L" --ignore-space-at-eol";
	if (ignoreSpaceChange)
		ignore += L" --ignore-space-change";
	if (ignoreAllSpace)
		ignore += L" --ignore-all-space";
	if (ignoreBlankLines)
		ignore += L" --ignore-blank-lines";

	if(rev1 == GIT_REV_ZERO || rev2 == GIT_REV_ZERO)
	{
		if(rev1 == GIT_REV_ZERO)
			cmd.Format(L"git.exe diff -r --raw -C%d%% -M%d%% --numstat -z %s --end-of-options %s --", ms_iSimilarityIndexThreshold, ms_iSimilarityIndexThreshold, static_cast<LPCWSTR>(ignore), static_cast<LPCWSTR>(rev2));
		else
			cmd.Format(L"git.exe diff -r -R --raw -C%d%% -M%d%% --numstat -z %s --end-of-options %s --", ms_iSimilarityIndexThreshold, ms_iSimilarityIndexThreshold, static_cast<LPCWSTR>(ignore), static_cast<LPCWSTR>(rev1));
	}
	else
		cmd.Format(L"git.exe diff-tree -r --raw -C%d%% -M%d%% --numstat -z %s --end-of-options %s %s --", ms_iSimilarityIndexThreshold, ms_iSimilarityIndexThreshold, static_cast<LPCWSTR>(ignore), static_cast<LPCWSTR>(rev2), static_cast<LPCWSTR>(rev1));

	BYTE_VECTOR out;
	if (Run(cmd, &out))
		return -1;

	return outputlist.ParserFromLog(out);
}

int CGit::GetTagList(STRING_VECTOR &list)
{
	const size_t prevCount = list.size();
	if (this->m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CAutoStrArray tag_names;

		if (git_tag_list(tag_names, repo))
			return -1;

		for (size_t i = 0; i < tag_names->count; ++i)
		{
			list.push_back(CUnicodeUtils::GetUnicode(tag_names->strings[i]));
		}

		std::sort(list.begin() + prevCount, list.end(), g_bSortTagsReversed ? LogicalCompareReversedPredicate : LogicalComparePredicate);

		return 0;
	}
	else
	{
		gitLastErr.Empty();
		const int ret = Run(L"git.exe tag -l", [&](const CStringA& lineA)
		{
			if (lineA.IsEmpty())
				return;
			list.push_back(CUnicodeUtils::GetUnicode(lineA));
		}, &gitLastErr);
		if (!ret)
			std::sort(list.begin() + prevCount, list.end(), g_bSortTagsReversed ? LogicalCompareReversedPredicate : LogicalComparePredicate);
		else if (ret == 1 && IsInitRepos())
			return 0;
		return ret;
	}
}

CString CGit::GetGitLastErr(const CString& msg)
{
	if (this->m_IsUseLibGit2)
		return GetLibGit2LastErr(msg);
	else if (gitLastErr.IsEmpty())
		return msg + L"\nUnknown git.exe error.";
	else
	{
		CString lastError = gitLastErr;
		gitLastErr.Empty();
		return msg + L'\n' + lastError;
	}
}

CString CGit::GetGitLastErr(const CString& msg, LIBGIT2_CMD cmd)
{
	if (UsingLibGit2(cmd))
		return GetLibGit2LastErr(msg);
	else if (gitLastErr.IsEmpty())
		return msg + L"\nUnknown git.exe error.";
	else
	{
		CString lastError = gitLastErr;
		gitLastErr.Empty();
		return msg + L'\n' + lastError;
	}
}

CString CGit::GetLibGit2LastErr()
{
	const git_error *libgit2err = git_error_last();
	if (libgit2err)
	{
		CString lastError = CUnicodeUtils::GetUnicode(libgit2err->message);
		git_error_clear();
		return L"libgit2 returned: " + lastError;
	}
	else
		return L"An error occoured in libgit2, but no message is available.";
}

CString CGit::GetLibGit2LastErr(const CString& msg)
{
	if (!msg.IsEmpty())
		return msg + L'\n' + GetLibGit2LastErr();
	return GetLibGit2LastErr();
}

CString CGit::FixBranchName_Mod(CString& branchName)
{
	if (branchName == L"FETCH_HEAD")
		branchName = DerefFetchHead();
	return branchName;
}

CString	CGit::FixBranchName(const CString& branchName)
{
	CString tempBranchName = branchName;
	FixBranchName_Mod(tempBranchName);
	return tempBranchName;
}

bool CGit::IsBranchTagNameUnique(const CString& name)
{
	if (m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return true; // TODO: optimize error reporting

		CAutoReference tagRef;
		if (git_reference_lookup(tagRef.GetPointer(), repo, CUnicodeUtils::GetUTF8(L"refs/tags/" + name)))
			return true;

		CAutoReference branchRef;
		if (git_reference_lookup(branchRef.GetPointer(), repo, CUnicodeUtils::GetUTF8(L"refs/heads/" + name)))
			return true;

		return false;
	}
	// else
	CString cmd;
	cmd.Format(L"git.exe show-ref --tags --heads -- refs/heads/%s refs/tags/%s", static_cast<LPCWSTR>(name), static_cast<LPCWSTR>(name));

	int refCnt = 0;
	Run(cmd, [&](const CStringA& lineA)
	{
		if (lineA.IsEmpty())
			return;
		++refCnt;
	});

	return (refCnt <= 1);
}

bool CGit::IsLocalBranch(const CString& shortName)
{
	STRING_VECTOR list;
	GetBranchList(list, nullptr, CGit::BRANCH_LOCAL);
	return std::find(list.cbegin(), list.cend(), shortName) != list.cend();
}

bool CGit::BranchTagExists(const CString& name, bool isBranch /*= true*/)
{
	if (m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return false; // TODO: optimize error reporting

		CString prefix;
		if (isBranch)
			prefix = L"refs/heads/";
		else
			prefix = L"refs/tags/";

		CAutoReference ref;
		if (git_reference_lookup(ref.GetPointer(), repo, CUnicodeUtils::GetUTF8(prefix + name)))
			return false;

		return true;
	}
	// else
	CString cmd, output;

	cmd = L"git.exe show-ref ";
	if (isBranch)
		cmd += L"--heads ";
	else
		cmd += L"--tags ";

	cmd += L"-- refs/heads/" + name;
	cmd += L" refs/tags/" + name;

	if (!Run(cmd, &output, nullptr, CP_UTF8))
	{
		if (!output.IsEmpty())
			return true;
	}

	return false;
}

CString CGit::DerefFetchHead()
{
	CString dotGitPath;
	GitAdminDir::GetWorktreeAdminDirPath(m_CurrentDir, dotGitPath);
	std::ifstream fetchHeadFile((dotGitPath + L"FETCH_HEAD").GetString(), std::ios::in | std::ios::binary);
	int forMergeLineCount = 0;
	std::string line;
	std::string hashToReturn;
	while(getline(fetchHeadFile, line))
	{
		//Tokenize this line
		std::string::size_type prevPos = 0;
		std::string::size_type pos = line.find('\t');
		if(pos == std::string::npos)	continue; //invalid line

		std::string hash = line.substr(0, pos);
		++pos; prevPos = pos; pos = line.find('\t', pos); if(pos == std::string::npos) continue;

		bool forMerge = pos == prevPos;
		++pos; prevPos = pos; pos = line.size(); if(pos == std::string::npos) continue;

		//std::string remoteBranch = line.substr(prevPos, pos - prevPos);

		//Process this line
		if(forMerge)
		{
			hashToReturn = hash;
			++forMergeLineCount;
			if(forMergeLineCount > 1)
				return L""; //More then 1 branch to merge found. Octopus merge needed. Cannot pick single ref from FETCH_HEAD
		}
	}

	return CUnicodeUtils::GetUnicode(hashToReturn);
}

int CGit::GetBranchList(STRING_VECTOR &list, int *current, BRANCH_TYPE type, bool skipCurrent /*false*/)
{
	const size_t prevCount = list.size();
	int ret = 0;
	CString cur;
	bool headIsDetached = false;
	if (m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		if (git_repository_head_detached(repo) == 1)
			headIsDetached = true;

		if ((type & (BRANCH_LOCAL | BRANCH_REMOTE)) != 0)
		{
			git_branch_t flags = GIT_BRANCH_LOCAL;
			if ((type & BRANCH_LOCAL) && (type & BRANCH_REMOTE))
				flags = GIT_BRANCH_ALL;
			else if (type & BRANCH_REMOTE)
				flags = GIT_BRANCH_REMOTE;

			CAutoBranchIterator it;
			if (git_branch_iterator_new(it.GetPointer(), repo, flags))
				return -1;

			CAutoReference ref;
			git_branch_t branchType;
			while (git_branch_next(ref.GetPointer(), &branchType, it) == 0)
			{
				const char * name = nullptr;
				if (git_branch_name(&name, ref))
					continue;

				CString branchname = CUnicodeUtils::GetUnicode(name);
				if (branchType & GIT_BRANCH_REMOTE)
					list.push_back(L"remotes/" + branchname);
				else
				{
					if (git_branch_is_head(ref))
					{
						if (skipCurrent)
							continue;
						cur = branchname;
					}
					list.push_back(branchname);
				}
			}
		}
	}
	else
	{
		CString cmd = L"git.exe branch --no-color";

		if ((type & BRANCH_ALL) == BRANCH_ALL)
			cmd += L" -a";
		else if (type & BRANCH_REMOTE)
			cmd += L" -r";

		ret = Run(cmd, [&](CStringA lineA)
		{
			lineA.Trim(" \r\n\t");
			if (lineA.IsEmpty())
				return;
			if (lineA.Find(" -> ") >= 0)
				return; // skip something like: refs/origin/HEAD -> refs/origin/master

			CString branch = CUnicodeUtils::GetUnicode(lineA);
			if (lineA[0] == '*')
			{
				if (skipCurrent)
					return;
				branch = branch.Mid(static_cast<int>(wcslen(L"* ")));
				cur = branch;

				// check whether HEAD is detached
				CString currentHead;
				if (branch[0] == L'(' && GetCurrentBranchFromFile(m_CurrentDir, currentHead) == 1)
				{
					headIsDetached = true;
					return;
				}
			}
			else if (lineA[0] == '+') // since Git 2.23 branches that are checked out in other worktrees connected to the same repository prefixed with '+'
				branch = branch.Mid(static_cast<int>(wcslen(L"+ ")));
			if ((type & BRANCH_REMOTE) != 0 && (type & BRANCH_LOCAL) == 0)
				branch = L"remotes/" + branch;
			list.push_back(branch);
		});
		if (ret == 1 && IsInitRepos())
			return 0;
	}

	if(type & BRANCH_FETCH_HEAD && !DerefFetchHead().IsEmpty())
		list.push_back(L"FETCH_HEAD");

	std::sort(list.begin() + prevCount, list.end(), LogicalCompareBranchesPredicate);

	if (current && !headIsDetached && !skipCurrent)
	{
		for (unsigned int i = 0; i < list.size(); ++i)
		{
			if (list[i] == cur)
			{
				*current = i;
				break;
			}
		}
	}

	return ret;
}

int CGit::GetRefsCommitIsOn(STRING_VECTOR& list, const CGitHash& hash, bool includeTags, bool includeBranches, BRANCH_TYPE type)
{
	if (!includeTags && !includeBranches || hash.IsEmpty())
		return 0;

	const size_t prevCount = list.size();
	if (UsingLibGit2(GIT_CMD_BRANCH_CONTAINS))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CAutoReferenceIterator it;
		if (git_reference_iterator_new(it.GetPointer(), repo))
			return -1;

		auto checkDescendent = [&list, &hash, &repo](const git_oid* oid, const git_reference* ref) {
			if (!oid)
				return;
			if (git_oid_equal(oid, hash) || git_graph_descendant_of(repo, oid, hash) == 1)
			{
				const char* name = git_reference_name(ref);
				if (!name)
					return;

				list.push_back(CUnicodeUtils::GetUnicode(name));
			}
		};

		CAutoReference ref;
		while (git_reference_next(ref.GetPointer(), it) == 0)
		{
			if (git_reference_is_tag(ref))
			{
				if (!includeTags)
					continue;

				CAutoTag tag;
				if (git_tag_lookup(tag.GetPointer(), repo, git_reference_target(ref)) == 0)
				{
					CAutoObject obj;
					if (git_tag_peel(obj.GetPointer(), tag) < 0)
						continue;
					checkDescendent(git_object_id(obj), ref);
					continue;
				}
			}
			else if (git_reference_is_remote(ref))
			{
				if (!includeBranches || !(type & BRANCH_REMOTE))
					continue;
			}
			else if (git_reference_is_branch(ref))
			{
				if (!includeBranches || !(type & BRANCH_LOCAL))
					continue;
			}
			else
				continue;

			if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC)
			{
				CAutoReference peeledRef;
				if (git_reference_resolve(peeledRef.GetPointer(), ref) < 0)
					continue;

				checkDescendent(git_reference_target(peeledRef), ref);
				continue;
			}

			checkDescendent(git_reference_target(ref), ref);
		}
	}
	else
	{
		if (includeBranches)
		{
			CString cmd = L"git.exe branch --no-color";
			if ((type & BRANCH_ALL) == BRANCH_ALL)
				cmd += L" -a";
			else if (type & BRANCH_REMOTE)
				cmd += L" -r";
			cmd += L" --contains " + hash.ToString();

			if (Run(cmd, [&](CStringA lineA)
			{
				lineA.Trim(" \r\n\t");
				if (lineA.IsEmpty())
					return;
				if (lineA.Find(" -> ") >= 0) // normalize symbolic refs: "refs/origin/HEAD -> refs/origin/master" to "refs/origin/HEAD"
					lineA.Truncate(lineA.Find(" -> "));

				CString branch = CUnicodeUtils::GetUnicode(lineA);
				if (lineA[0] == '*')
				{
					branch = branch.Mid(static_cast<int>(wcslen(L"* ")));
					CString currentHead;
					if (branch[0] == L'(' && GetCurrentBranchFromFile(m_CurrentDir, currentHead) == 1)
						return;
				}

				if ((type & BRANCH_REMOTE) != 0 && (type & BRANCH_LOCAL) == 0)
					branch = L"refs/remotes/" + branch;
				else if (CStringUtils::StartsWith(branch, L"remotes/"))
					branch = L"refs/" + branch;
				else
					branch = L"refs/heads/" + branch;
				list.push_back(branch);
			}))
				return -1;
		}

		if (includeTags)
		{
			CString cmd = L"git.exe tag --contains " + hash.ToString();
			if (Run(cmd, [&list](CStringA lineA)
			{
				if (lineA.IsEmpty())
					return;
				list.push_back(L"refs/tags/" + CUnicodeUtils::GetUnicode(lineA));
			}))
				return -1;
		}
	}

	std::sort(list.begin() + prevCount, list.end(), LogicalCompareBranchesPredicate);
	list.erase(std::unique(list.begin() + prevCount, list.end()), list.end());

	return 0;
}

int CGit::GetRemoteList(STRING_VECTOR &list)
{
	const size_t prevCount = list.size();
	if (this->m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CAutoStrArray remotes;
		if (git_remote_list(remotes, repo))
			return -1;

		for (size_t i = 0; i < remotes->count; ++i)
		{
			list.push_back(CUnicodeUtils::GetUnicode(remotes->strings[i]));
		}

		std::sort(list.begin() + prevCount, list.end(), LogicalComparePredicate);

		return 0;
	}

	gitLastErr.Empty();
	return Run(L"git.exe remote", [&](const CStringA& lineA)
	{
		if (lineA.IsEmpty())
			return;
		list.push_back(CUnicodeUtils::GetUnicode(lineA));
	}, &gitLastErr);
}

int CGit::GetRemoteRefs(const CString& remote, REF_VECTOR& list, bool includeTags, bool includeBranches)
{
	if (!includeTags && !includeBranches)
		return 0;

	const size_t prevCount = list.size();
	if (UsingLibGit2(GIT_CMD_FETCH))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CStringA remoteA = CUnicodeUtils::GetUTF8(remote);
		CAutoRemote gitremote;
		// first try with a named remote (e.g. "origin")
		if (git_remote_lookup(gitremote.GetPointer(), repo, remoteA) < 0)
		{
			// retry with repository located at a specific url
			if (git_remote_create_anonymous(gitremote.GetPointer(), repo, remoteA) < 0)
				return -1;
		}

		git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
		callbacks.credentials = g_Git2CredCallback;
		callbacks.certificate_check = g_Git2CheckCertificateCallback;
		git_proxy_options proxy = GIT_PROXY_OPTIONS_INIT;
		proxy.type = GIT_PROXY_AUTO;
		if (git_remote_connect(gitremote, GIT_DIRECTION_FETCH, &callbacks, &proxy, nullptr) < 0)
			return -1;

		const git_remote_head** heads = nullptr;
		size_t size = 0;
		if (git_remote_ls(&heads, &size, gitremote) < 0)
			return -1;

		for (size_t i = 0; i < size; i++)
		{
			CString ref = CUnicodeUtils::GetUnicode(heads[i]->name);
			CString shortname;
			if (GetShortName(ref, shortname, L"refs/tags/"))
			{
				if (!includeTags)
					continue;
			}
			else
			{
				if (!includeBranches)
					continue;
				if (!GetShortName(ref, shortname, L"refs/heads/"))
					shortname = ref;
			}
			if (includeTags && includeBranches)
				list.emplace_back(TGitRef{ ref, &heads[i]->oid });
			else
				list.emplace_back(TGitRef{ shortname, &heads[i]->oid });
		}
		std::sort(list.begin() + prevCount, list.end(), g_bSortTagsReversed && includeTags && !includeBranches ? LogicalCompareReversedPredicate : LogicalComparePredicate);
		return 0;
	}

	CString cmd;
	cmd.Format(L"git.exe ls-remote%s -- \"%s\"", (includeTags && !includeBranches) ? L" -t" : L" --refs", static_cast<LPCWSTR>(remote));
	gitLastErr = cmd + L'\n';
	if (Run(
		cmd, [&](const CStringA& origLineA) {
			if (origLineA.GetLength() <= GIT_HASH_SIZE * 2 + static_cast<int>(strlen("\t")) || origLineA[GIT_HASH_SIZE * 2] != '\t') // OID, tab, refname
				return;
			CGitHash hash = CGitHash::FromHexStr(std::string_view(origLineA, GIT_HASH_SIZE * 2));
			CString ref = CUnicodeUtils::GetUnicode(origLineA.Mid(GIT_HASH_SIZE * 2 + static_cast<int>(strlen("\t"))));
			CString shortname;
			if (GetShortName(ref, shortname, L"refs/tags/"))
			{
				if (!includeTags)
					return;
			}
			else
			{
				if (!includeBranches)
					return;
				if (!GetShortName(ref, shortname, L"refs/heads/"))
					shortname = ref;
			}
			if (includeTags && includeBranches)
				list.emplace_back(TGitRef{ ref, hash });
			else if (includeTags && CStringUtils::EndsWith(ref, L"^{}"))
				list.emplace_back(TGitRef{ shortname + L"^{}", hash });
			else
				list.emplace_back(TGitRef{ shortname, hash });
		},
		&gitLastErr))
		return -1;
	std::sort(list.begin() + prevCount, list.end(), g_bSortTagsReversed && includeTags && !includeBranches ? LogicalCompareReversedPredicate : LogicalComparePredicate);
	return 0;
}

int CGit::DeleteRemoteRefs(const CString& sRemote, const STRING_VECTOR& list)
{
	if (UsingLibGit2(GIT_CMD_PUSH))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CStringA remoteA = CUnicodeUtils::GetUTF8(sRemote);
		CAutoRemote remote;
		if (git_remote_lookup(remote.GetPointer(), repo, remoteA) < 0)
			return -1;

		git_push_options pushOpts = GIT_PUSH_OPTIONS_INIT;
		git_remote_callbacks& callbacks = pushOpts.callbacks;
		callbacks.credentials = g_Git2CredCallback;
		callbacks.certificate_check = g_Git2CheckCertificateCallback;
		std::vector<CStringA> refspecs;
		refspecs.reserve(list.size());
		std::transform(list.cbegin(), list.cend(), std::back_inserter(refspecs), [](auto& ref) { return CUnicodeUtils::GetUTF8(L":" + ref); });

		std::vector<char*> vc;
		vc.reserve(refspecs.size());
		std::transform(refspecs.begin(), refspecs.end(), std::back_inserter(vc), [](CStringA& s) -> char* { return s.GetBuffer(); });
		git_strarray specs = { vc.data(), vc.size() };

		if (git_remote_push(remote, &specs, &pushOpts) < 0)
			return -1;
	}
	else
	{
		CMassiveGitTaskBase mgtPush(L"push " + sRemote, FALSE);
		for (const auto& ref : list)
			mgtPush.AddFile(L':' + ref);

		BOOL cancel = FALSE;
		mgtPush.Execute(cancel);
	}

	return 0;
}

int libgit2_addto_list_each_ref_fn(git_reference* rawref, void* payload)
{
	CAutoReference ref{ std::move(rawref) };
	auto list = static_cast<STRING_VECTOR*>(payload);
	list->push_back(CUnicodeUtils::GetUnicode(git_reference_name(ref)));
	return 0;
}

int CGit::GetRefList(STRING_VECTOR &list)
{
	const size_t prevCount = list.size();
	if (this->m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		if (git_reference_foreach(repo, libgit2_addto_list_each_ref_fn, &list))
			return -1;

		std::sort(list.begin() + prevCount, list.end(), LogicalComparePredicate);

		return 0;
	}

	gitLastErr.Empty();
	const int ret = Run(L"git.exe show-ref -d", [&](const CStringA& lineA)
	{
		const int start = lineA.Find(L' ');
		ASSERT(start == 2 * GIT_HASH_SIZE);
		if (start <= 0)
			return;

		CString name = CUnicodeUtils::GetUnicode(lineA.Mid(start + 1));
		if (list.empty() || name != *list.crbegin() + L"^{}")
			list.push_back(name);
	}, &gitLastErr);
	if (!ret)
		std::sort(list.begin() + prevCount, list.end(), LogicalComparePredicate);
	else if (ret == 1 && IsInitRepos())
		return 0;
	return ret;
}

struct map_each_ref_payload {
	git_repository* repo = nullptr;
	MAP_HASH_NAME* map = nullptr;
};

int libgit2_addto_map_each_ref_fn(git_reference* rawref, void* payload)
{
	CAutoReference ref{ std::move(rawref) };
	auto payloadContent = static_cast<map_each_ref_payload*>(payload);

	CString str = CUnicodeUtils::GetUnicode(git_reference_name(ref));

	CAutoObject gitObject;
	if (git_revparse_single(gitObject.GetPointer(), payloadContent->repo, git_reference_name(ref)))
		return (git_reference_is_remote(ref) && git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) ? 0 : 1; // don't bail out for symbolic remote references ("git.exe show-ref -d" also doesn't complain), cf. issue #2926

	if (git_object_type(gitObject) == GIT_OBJECT_TAG)
	{
		str += L"^{}"; // deref tag
		CAutoObject derefedTag;
		if (git_tag_target(derefedTag.GetPointer(), reinterpret_cast<git_tag*>(static_cast<git_object*>(gitObject))))
			return 1;
		gitObject.Swap(derefedTag);
	}

	(*payloadContent->map)[git_object_id(gitObject)].push_back(str);

	return 0;
}
int CGit::GetMapHashToFriendName(git_repository* repo, MAP_HASH_NAME &map)
{
	ATLASSERT(repo);

	map_each_ref_payload payloadContent = { repo, &map };

	if (git_reference_foreach(repo, libgit2_addto_map_each_ref_fn, &payloadContent))
		return -1;

	for (auto it = map.begin(); it != map.end(); ++it)
	{
		std::sort(it->second.begin(), it->second.end());
	}

	return 0;
}

int CGit::GetMapHashToFriendName(MAP_HASH_NAME &map)
{
	if (this->m_IsUseLibGit2)
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		return GetMapHashToFriendName(repo, map);
	}

	gitLastErr.Empty();
	const int ret = Run(L"git.exe show-ref -d", [&](const CStringA& lineA)
	{
		const int start = lineA.Find(L' ');
		ASSERT(start == 2 * GIT_HASH_SIZE);
		if (start <= 0)
			return;

		CGitHash hash = CGitHash::FromHexStr(std::string_view(lineA, start));
		map[hash].push_back(CUnicodeUtils::GetUnicode(lineA.Mid(start + 1)));
	}, &gitLastErr);

	if (ret == 1 && IsInitRepos())
		return 0;
	return ret;
}

int CGit::GuessRefForHash(CString& ref, const CGitHash& hash)
{
	MAP_HASH_NAME map;
	if (GetMapHashToFriendName(map))
		return -1;

	auto it = map.find(hash);
	if (it == map.cend())
	{
		ref = hash.ToString(GetShortHASHLength());
		return 1;
	}

	const auto& reflist = it->second;
	for (const auto& reftype : { L"refs/heads/", L"refs/remotes/", L"refs/tags/" })
	{
		auto found = std::find_if(reflist.cbegin(), reflist.cend(), [&reftype](const auto& ref) { return CStringUtils::StartsWith(ref, reftype); });
		if (found == reflist.cend())
			continue;

		GetShortName(*found, ref, reftype);
		return 0;
	}

	ref = hash.ToString(GetShortHASHLength());
	return 1;
}

int CGit::GetBranchDescriptions(MAP_STRING_STRING& map)
{
	CAutoConfig config(true);
	if (git_config_add_file_ondisk(config, CGit::GetGitPathStringA(GetGitLocalConfig()), GIT_CONFIG_LEVEL_LOCAL, nullptr, FALSE) < 0)
		return -1;
	return git_config_foreach_match(config, "^branch\\..*\\.description$", [](const git_config_entry* entry, void* data)
	{
		auto descriptions = static_cast<MAP_STRING_STRING*>(data);
		CString key = CUnicodeUtils::GetUnicode(entry->name);
		// extract branch name from config key
		key = key.Mid(static_cast<int>(wcslen(L"branch.")), key.GetLength() - static_cast<int>(wcslen(L"branch.")) - static_cast<int>(wcslen(L".description")));
		descriptions->insert(std::make_pair(key, CUnicodeUtils::GetUnicode(entry->value)));
		return 0;
	}, &map);
}

static void SetLibGit2SearchPath(int level, const CString &value)
{
	CStringA valueA = CUnicodeUtils::GetUTF8(value);
	git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, static_cast<LPCSTR>(valueA));
}

static void SetLibGit2TemplatePath(const CString &value)
{
	CStringA valueA = CUnicodeUtils::GetUTF8(value);
	git_libgit2_opts(GIT_OPT_SET_TEMPLATE_PATH, static_cast<LPCSTR>(valueA));
}

int CGit::FindAndSetGitExePath(BOOL bFallback)
{
	CRegString msysdir = CRegString(REG_MSYSGIT_PATH, L"", FALSE);
	CString str = msysdir;
	if (!str.IsEmpty() && PathFileExists(str + L"\\git.exe"))
	{
		CGit::ms_LastMsysGitDir = str;
		return TRUE;
	}

	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": git.exe not exists: %s\n", static_cast<LPCWSTR>(CGit::ms_LastMsysGitDir));
	if (!bFallback)
		return FALSE;

	// first, search PATH if git/bin directory is already present
	if (FindGitPath())
	{
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": FindGitPath() => %s\n", static_cast<LPCWSTR>(CGit::ms_LastMsysGitDir));
		msysdir = CGit::ms_LastMsysGitDir;
		msysdir.write();
		return TRUE;
	}

	if (FindGitForWindows(str))
	{
		msysdir = str;
		CGit::ms_LastMsysGitDir = str;
		msysdir.write();
		return TRUE;
	}

	return FALSE;
}

BOOL CGit::CheckMsysGitDir(BOOL bFallback)
{
	if (m_bInitialized)
		return TRUE;

	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": CheckMsysGitDir(%d)\n", bFallback);
	this->m_Environment.clear();
	m_Environment.CopyProcessEnvironment();

	m_Environment.SetEnv(L"TGIT_INITIATED_CALL", L"1");

	// Sanitize GIT_* environment variables, cf. https://github.com/git-for-windows/build-extra/pull/529 and git/environment.h
	m_Environment.SetEnv(L"GIT_INDEX_FILE", nullptr);
	m_Environment.SetEnv(L"GIT_INDEX_VERSION", nullptr);
	m_Environment.SetEnv(L"GIT_OBJECT_DIRECTORY", nullptr);
	m_Environment.SetEnv(L"GIT_ALTERNATE_OBJECT_DIRECTORIES", nullptr);
	m_Environment.SetEnv(L"GIT_DIR", nullptr);
	m_Environment.SetEnv(L"GIT_WORK_TREE", nullptr);
	m_Environment.SetEnv(L"GIT_NAMESPACE", nullptr);
	m_Environment.SetEnv(L"GIT_CEILING_DIRECTORIES", nullptr);
	m_Environment.SetEnv(L"GIT_DISCOVERY_ACROSS_FILESYSTEM", nullptr);
	m_Environment.SetEnv(L"GIT_COMMON_DIR", nullptr);
	m_Environment.SetEnv(L"GIT_DEFAULT_HASH", nullptr);
	m_Environment.SetEnv(L"GIT_CONFIG", nullptr);
	m_Environment.SetEnv(L"GIT_CONFIG_GLOBAL", nullptr);
	m_Environment.SetEnv(L"GIT_CONFIG_SYSTEM", nullptr);
	m_Environment.SetEnv(L"GIT_CONFIG_NOSYSTEM", nullptr);
	m_Environment.SetEnv(L"GIT_CONFIG_COUNT", nullptr);
	m_Environment.SetEnv(L"GIT_ATTR_NOSYSTEM", nullptr);
	m_Environment.SetEnv(L"GIT_ATTR_SOURCE", nullptr);
	m_Environment.SetEnv(L"GIT_SHALLOW_FILE", nullptr);
	m_Environment.SetEnv(L"GIT_GRAFT_FILE", nullptr);

	// Git for Windows 2.10.1 and 2.10.2 require LC_ALL to be set, see https://tortoisegit.org/issue/2859 and https://github.com/git-for-windows/git/issues/945,
	// because MSys2 changed the default to "ASCII". SO, make sure we have a proper default set
	if (m_Environment.GetEnv(L"LC_ALL").IsEmpty())
		m_Environment.SetEnv(L"LC_ALL", L"C");

	// set HOME if not set already
	size_t homesize;
	_wgetenv_s(&homesize, nullptr, 0, L"HOME");
	if (!homesize)
		m_Environment.SetEnv(L"HOME", GetHomeDirectory());

	//setup ssh client
	CString sshclient = CRegString(L"Software\\TortoiseGit\\SSH");
	if (sshclient.IsEmpty())
		sshclient = CRegString(L"Software\\TortoiseGit\\SSH", L"", FALSE, HKEY_LOCAL_MACHINE);

	if(!sshclient.IsEmpty())
	{
		if (ms_bCygwinGit)
			sshclient.Replace(L'\\', L'/');
		m_Environment.SetEnv(L"GIT_SSH", sshclient);
		if (CStringUtils::EndsWithI(sshclient, L"tortoisegitplink") || CStringUtils::EndsWithI(sshclient, L"tortoisegitplink.exe"))
			m_Environment.SetEnv(L"GIT_SSH_VARIANT", L"ssh");
		m_Environment.SetEnv(L"SVN_SSH", sshclient);
	}
	else
	{
		wchar_t sPlink[MAX_PATH] = { 0 };
		GetModuleFileName(nullptr, sPlink, _countof(sPlink));
		LPWSTR ptr = wcsrchr(sPlink, L'\\');
		if (ptr) {
			wcscpy_s(ptr + 1, _countof(sPlink) - (ptr - sPlink + 1), L"TortoiseGitPlink.exe");
			if (ms_bCygwinGit)
				CPathUtils::ConvertToSlash(sPlink);
			m_Environment.SetEnv(L"GIT_SSH", sPlink);
			m_Environment.SetEnv(L"GIT_SSH_VARIANT", L"ssh");
			m_Environment.SetEnv(L"SVN_SSH", sPlink);
		}
	}

	{
		wchar_t sAskPass[MAX_PATH] = { 0 };
		GetModuleFileName(nullptr, sAskPass, _countof(sAskPass));
		LPWSTR ptr = wcsrchr(sAskPass, L'\\');
		if (ptr)
		{
			wcscpy_s(ptr + 1, _countof(sAskPass) - (ptr - sAskPass + 1), L"SshAskPass.exe");
			if (ms_bCygwinGit)
				CPathUtils::ConvertToSlash(sAskPass);
			m_Environment.SetEnv(L"DISPLAY",L":9999");
			m_Environment.SetEnv(L"SSH_ASKPASS",sAskPass);
			m_Environment.SetEnv(L"GIT_ASKPASS",sAskPass);
			m_Environment.SetEnv(L"GIT_ASK_YESNO", sAskPass);
			m_Environment.SetEnv(L"SSH_ASKPASS_REQUIRE", L"force"); // improve compatibility with Win32-OpenSSH, see issue #3996
		}
	}

	if (!FindAndSetGitExePath(bFallback))
		return FALSE;

	CString msysGitDir;
	PathCanonicalize(CStrBuf(msysGitDir, MAX_PATH), CGit::ms_LastMsysGitDir + L"\\..\\");
	static const CString prefixes[] = { L"mingw64\\etc", L"mingw32\\etc", L"etc" };
	static const int prefixes_len[] = { 8, 8, 0 };
	for (int i = 0; i < _countof(prefixes); ++i)
	{
#ifndef _WIN64
		if (i == 0)
			continue;
#endif
		if (PathIsDirectory(msysGitDir + prefixes[i])) {
			msysGitDir += prefixes[i].Left(prefixes_len[i]);
			break;
		}
	}
	if (ms_bMsys2Git) // in Msys2 git.exe is in usr\bin; this also need to be after the check for etc folder, as Msys2 also has mingw64\etc, but uses etc
		PathCanonicalize(CStrBuf(msysGitDir, MAX_PATH), CGit::ms_LastMsysGitDir + L"\\..\\..");
	CGit::ms_MsysGitRootDir = msysGitDir;

	if (static_cast<CString>(CRegString(REG_SYSTEM_GITCONFIGPATH, L"", FALSE)) != g_Git.GetGitSystemConfig())
		CRegString(REG_SYSTEM_GITCONFIGPATH, L"", FALSE) = g_Git.GetGitSystemConfig();

	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": ms_LastMsysGitDir = %s\n", static_cast<LPCWSTR>(CGit::ms_LastMsysGitDir));
	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": ms_MsysGitRootDir = %s\n", static_cast<LPCWSTR>(CGit::ms_MsysGitRootDir));
	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": System config = %s\n", static_cast<LPCWSTR>(g_Git.GetGitSystemConfig()));
	SetLibGit2SearchPath(GIT_CONFIG_LEVEL_PROGRAMDATA, L"");

	if (ms_bCygwinGit)
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": CygwinHack: true\n");
	if (ms_bMsys2Git)
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": Msys2Hack: true\n");

	ms_LastMsysGitVersion = CRegDWORD(L"Software\\TortoiseGit\\git_cached_version");

	// Configure libgit2 search paths
	SetLibGit2SearchPath(GIT_CONFIG_LEVEL_SYSTEM, CTGitPath(g_Git.GetGitSystemConfig()).GetContainingDirectory().GetWinPathString());
	SetLibGit2SearchPath(GIT_CONFIG_LEVEL_GLOBAL, g_Git.GetHomeDirectory());
	SetLibGit2SearchPath(GIT_CONFIG_LEVEL_XDG, g_Git.GetGitGlobalXDGConfig(true));
	static git_smart_subtransport_definition ssh_wintunnel_subtransport_definition = { [](git_smart_subtransport **out, git_transport* owner, void*) -> int { return git_smart_subtransport_ssh_wintunnel(out, owner, FindExecutableOnPath(g_Git.m_Environment.GetEnv(L"GIT_SSH"), g_Git.m_Environment.GetEnv(L"PATH")), g_Git.m_Environment); }, 0 };
	git_transport_register("ssh", git_transport_smart, &ssh_wintunnel_subtransport_definition);
	git_transport_register("ssh+git", git_transport_smart, &ssh_wintunnel_subtransport_definition);
	git_transport_register("git+ssh", git_transport_smart, &ssh_wintunnel_subtransport_definition);
	git_libgit2_opts(GIT_OPT_SET_USER_AGENT, "TortoiseGit libgit2");
	if (!(ms_bCygwinGit || ms_bMsys2Git))
		SetLibGit2TemplatePath(CGit::ms_MsysGitRootDir + L"share\\git-core\\templates");
	else
		SetLibGit2TemplatePath(CGit::ms_MsysGitRootDir + L"usr\\share\\git-core\\templates");

	m_Environment.AddToPath(CGit::ms_LastMsysGitDir);
	m_Environment.AddToPath(static_cast<CString>(CRegString(REG_MSYSGIT_EXTRA_PATH, L"", FALSE)));

#if !defined(TGITCACHE) && !defined(TORTOISESHELL)
	// register filter only once
	if (!git_filter_lookup("filter"))
	{
		CString sh;
		for (const CString& binDirPrefix : { L"\\..\\usr\\bin", L"\\..\\bin", L"" })
		{
			CString possibleShExe = CGit::ms_LastMsysGitDir + binDirPrefix + L"\\sh.exe";
			if (PathFileExists(possibleShExe))
			{
				CString temp;
				PathCanonicalize(CStrBuf(temp, MAX_PATH), possibleShExe);
				sh.Format(L"\"%s\"", static_cast<LPCWSTR>(temp));
				// we need to put the usr\bin folder on the path for Git for Windows based on msys2
				m_Environment.AddToPath(temp.Left(temp.GetLength() - static_cast<int>(wcslen(L"\\sh.exe"))));
				break;
			}
		}

		// Add %GIT_EXEC_PATH% to %PATH% when launching libgit2 filter executable
		// It is possible that the filter points to a git subcommand, that is located at libexec\git-core
		CString gitExecPath = CGit::ms_MsysGitRootDir;
		gitExecPath.Append(L"libexec\\git-core");
		m_Environment.AddToPath(gitExecPath);

		if (git_filter_register("filter", git_filter_filter_new(sh, m_Environment), GIT_FILTER_DRIVER_PRIORITY))
			return FALSE;
	}
#endif

	m_bInitialized = true;
	return true;
}

CString CGit::GetHomeDirectory() const
{
	static CString homeDirectory;

	if (!homeDirectory.IsEmpty())
		return homeDirectory;

	homeDirectory = m_Environment.GetEnv(L"HOME");
	if (!homeDirectory.IsEmpty())
		return homeDirectory;

	if (CString tmp = m_Environment.GetEnv(L"HOMEDRIVE"); !tmp.IsEmpty())
	{
		if (CString tmp2 = m_Environment.GetEnv(L"HOMEPATH"); !tmp2.IsEmpty())
		{
			tmp += tmp2;
			if (CString windowsSysDirectory; GetSystemDirectory(CStrBuf(windowsSysDirectory, 4096), 4096) != FALSE && windowsSysDirectory != tmp && PathIsDirectory(tmp))
			{
				homeDirectory = tmp;
				return homeDirectory;
			}
		}
	}

	if (CString tmp = m_Environment.GetEnv(L"USERPROFILE"); !tmp.IsEmpty())
	{
		homeDirectory = tmp;
		return homeDirectory;
	}

	return homeDirectory;
}

CString CGit::GetGitLocalConfig() const
{
	CString path;
	GitAdminDir::GetAdminDirPath(m_CurrentDir, path);
	path += L"config";
	return path;
}

CStringA CGit::GetGitPathStringA(const CString &path)
{
	return CUnicodeUtils::GetUTF8(CTGitPath(path).GetGitPathString());
}

CString CGit::GetGitGlobalConfig() const
{
	return g_Git.GetHomeDirectory() + L"\\.gitconfig";
}

CString CGit::GetGitGlobalXDGConfig(bool returnDirectory) const
{
	// Attention: also see GitWCRev/status.cpp!
	if (CString xdgPath = m_Environment.GetEnv(L"XDG_CONFIG_HOME"); !xdgPath.IsEmpty())
	{
		xdgPath += L"\\git";
		if (!returnDirectory)
			xdgPath += L"\\config";
		return xdgPath;
	}

	if (CString appData = m_Environment.GetEnv(L"APPDATA"); !appData.IsEmpty() && !ms_bCygwinGit && !ms_bMsys2Git && ms_LastMsysGitVersion >= ConvertVersionToInt(2, 46, 0))
	{
		appData += L"\\Git";
		CString appDataConfigFile = appData + L"\\config";
		if (PathFileExists(appDataConfigFile))
		{
			if (returnDirectory)
				return appData;
			return appDataConfigFile;
		}
	}

	CString xdgInHome = g_Git.GetHomeDirectory();
	xdgInHome += L"\\.config\\git";
	if (returnDirectory)
		return xdgInHome;

	return xdgInHome + L"\\config";
}

CString CGit::GetGitSystemConfig() const
{
	return wget_msysgit_etc(m_Environment);
}

CString CGit::GetNotesRef() const
{
	return CUnicodeUtils::GetUnicode(git_default_notes_ref());
}

BOOL CGit::CheckCleanWorkTree(bool stagedOk /* false */)
{
	if (UsingLibGit2(GIT_CMD_CHECK_CLEAN_WT))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return FALSE;

		if (git_repository_head_unborn(repo))
			return FALSE;

		git_status_options statusopt = GIT_STATUS_OPTIONS_INIT;
		statusopt.show = stagedOk ? GIT_STATUS_SHOW_WORKDIR_ONLY : GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
		statusopt.flags = GIT_STATUS_OPT_UPDATE_INDEX | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

		CAutoStatusList status;
		if (git_status_list_new(status.GetPointer(), repo, &statusopt))
			return FALSE;

		return (0 == git_status_list_entrycount(status));
	}

	if (Run(L"git.exe rev-parse --verify HEAD", nullptr, nullptr, CP_UTF8))
		return FALSE;

	if (Run(L"git.exe update-index --ignore-submodules --refresh", nullptr, nullptr, CP_UTF8))
		return FALSE;

	if (Run(L"git.exe diff-files --quiet --ignore-submodules", nullptr, nullptr, CP_UTF8))
		return FALSE;

	if (!stagedOk && Run(L"git.exe diff-index --cached --quiet HEAD --ignore-submodules --", nullptr, nullptr, CP_UTF8))
		return FALSE;

	return TRUE;
}

BOOL CGit::IsResultingCommitBecomeEmpty(bool amend /* = false */)
{
	CString cmd;
	cmd.Format(L"git.exe diff-index --cached --quiet HEAD%s --", amend ? L"~1" : L"");
	return Run(cmd, nullptr, nullptr, CP_UTF8) == 0;
}

int CGit::HasWorkingTreeConflicts(git_repository* repo)
{
	ATLASSERT(repo);

	CAutoIndex index;
	if (git_repository_index(index.GetPointer(), repo))
		return -1;

	return git_index_has_conflicts(index);
}

int CGit::HasWorkingTreeConflicts()
{
	if (UsingLibGit2(GIT_CMD_CHECKCONFLICTS))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		return HasWorkingTreeConflicts(repo);
	}

	CString output;
	gitLastErr.Empty();
	if (Run(L"git.exe ls-files -u -t -z", &output, &gitLastErr, CP_UTF8))
		return -1;

	return output.IsEmpty() ? 0 : 1;
}

bool CGit::IsFastForward(const CString &from, const CString &to, CGitHash * commonAncestor)
{
	if (UsingLibGit2(GIT_CMD_MERGE_BASE))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return false;

		CGitHash fromHash, toHash, baseHash;
		if (GetHash(repo, toHash, FixBranchName(to)))
			return false;

		if (GetHash(repo, fromHash, FixBranchName(from)))
			return false;

		git_oid baseOid;
		if (git_merge_base(&baseOid, repo, toHash, fromHash))
			return false;

		baseHash = baseOid;

		if (commonAncestor)
			*commonAncestor = baseHash;

		return fromHash == baseHash;
	}
	// else
	CString base;
	CGitHash basehash,hash;
	CString cmd;
	cmd.Format(L"git.exe merge-base %s %s", static_cast<LPCWSTR>(FixBranchName(to)), static_cast<LPCWSTR>(FixBranchName(from)));

	gitLastErr.Empty();
	if (Run(cmd, &base, &gitLastErr, CP_UTF8))
		return false;
	basehash = CGitHash::FromHexStr(base.Trim());

	GetHash(hash, from);

	if (commonAncestor)
		*commonAncestor = basehash;

	return hash == basehash;
}

unsigned int CGit::Hash2int(const CGitHash &hash)
{
	int ret=0;
	for (int i = 0; i < 4; ++i)
	{
		ret = ret << 8;
		ret |= hash.ToRaw()[i];
	}
	return ret;
}

int CGit::RefreshGitIndex()
{
	// HACK: don't use internal update-index if we have a git-lfs enabled repository as the libgit version fails when executing the filter, issue #3220
	if (g_Git.m_IsUseGitDLL && !CTGitPath(g_Git.m_CurrentDir).HasLFS())
	{
		CAutoLocker lock(g_Git.m_critGitDllSec);
		try
		{
			g_Git.CheckAndInitDll();

			int result = git_update_index();
			git_exit_cleanup();
			return result;

		}catch(...)
		{
			git_exit_cleanup();
			return -1;
		}

	}
	else
		return Run(L"git.exe update-index --refresh", nullptr, nullptr, CP_UTF8);
}

int CGit::GetOneFile(const CString &Refname, const CTGitPath &path, const CString &outputfile)
{
	if (UsingLibGit2(GIT_CMD_GETONEFILE))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CGitHash hash;
		if (GetHash(repo, hash, Refname + L"^{}")) // add ^{} in order to dereference signed tags
			return -1;

		CAutoCommit commit;
		if (git_commit_lookup(commit.GetPointer(), repo, hash))
			return -1;

		CAutoTree tree;
		if (git_commit_tree(tree.GetPointer(), commit))
			return -1;

		CAutoTreeEntry entry;
		if (auto ret = git_tree_entry_bypath(entry.GetPointer(), tree, CUnicodeUtils::GetUTF8(path.GetGitPathString())); ret)
			return ret;

		if (git_tree_entry_filemode(entry) == GIT_FILEMODE_COMMIT)
		{
			git_error_set_str(GIT_ERROR_NONE, "The requested object is a submodule and not a file.");
			return -1;
		}

		CAutoBlob blob;
		if (git_tree_entry_to_object(reinterpret_cast<git_object**>(blob.GetPointer()), repo, entry))
			return -1;

		CAutoFILE file = _wfsopen(outputfile, L"wb", SH_DENYWR);
		if (file == nullptr)
		{
			git_error_set_str(GIT_ERROR_NONE, "Could not create file.");
			return -1;
		}
		CAutoBuf buf;
		git_blob_filter_options opts = GIT_BLOB_FILTER_OPTIONS_INIT;
		opts.flags &= ~static_cast<uint32_t>(GIT_BLOB_FILTER_CHECK_FOR_BINARY);
		if (git_blob_filter(buf, blob, CUnicodeUtils::GetUTF8(path.GetGitPathString()), &opts))
			return -1;
		if (fwrite(buf->ptr, sizeof(char), buf->size, file) != buf->size)
		{
			git_error_set_str(GIT_ERROR_OS, "Could not write to file.");
			return -1;
		}

		return 0;
	}
	else if (g_Git.m_IsUseGitDLL)
	{
		CAutoLocker lock(g_Git.m_critGitDllSec);
		try
		{
			g_Git.CheckAndInitDll();
			CStringA ref, patha, outa;
			ref = CUnicodeUtils::GetUTF8(Refname);
			patha = CUnicodeUtils::GetUTF8(path.GetGitPathString());
			outa = CUnicodeUtils::GetUTF8(outputfile);
			::DeleteFile(outputfile);
			return git_checkout_file(ref, patha, CStrBufA(outa));
		}
		catch (const char * msg)
		{
			gitLastErr = L"gitdll.dll reports: " + CUnicodeUtils::GetUnicode(msg);
			return -1;
		}
		catch (...)
		{
			gitLastErr = L"An unknown gitdll.dll error occurred.";
			return -1;
		}
	}
	else
	{
		CString cmd;
		cmd.Format(L"git.exe cat-file -p %s:\"%s\"", static_cast<LPCWSTR>(Refname), static_cast<LPCWSTR>(path.GetGitPathString()));
		gitLastErr.Empty();
		return RunLogFile(cmd, outputfile, &gitLastErr);
	}
}

void CEnvironment::clear()
{
	__super::clear();
	baseptr = nullptr;
}

bool CEnvironment::empty() const
{
	return size() < 3; // three is minimum for an empty environment with an empty key and empty value: "=\0\0"
}

CEnvironment::operator LPWSTR()
{
	if (empty())
		return nullptr;
	return data();
}

CEnvironment::operator const LPWSTR*() const
{
	return &baseptr;
}

void CEnvironment::CopyProcessEnvironment()
{
	if (!empty())
		pop_back();
	wchar_t* porig = GetEnvironmentStrings();
	const wchar_t* p = porig;
	while(*p !=0 || *(p+1) !=0)
		this->push_back(*p++);

	push_back(L'\0');
	push_back(L'\0');
	baseptr = data();

	FreeEnvironmentStrings(porig);
}

CString CEnvironment::GetEnv(const wchar_t* name) const
{
	ASSERT(name);
	CString str;
	for (size_t i = 0; i < size(); ++i)
	{
		str = &(*this)[i];
		int start =0;
		CString sname = str.Tokenize(L"=", start);
		if(sname.CompareNoCase(name) == 0)
			return &(*this)[i+start];
		i+=str.GetLength();
	}
	return L"";
}

void CEnvironment::SetEnv(const wchar_t* name, const wchar_t* value)
{
	ASSERT(name);
	unsigned int i;
	for (i = 0; i < size(); ++i)
	{
		CString str = &(*this)[i];
		int start =0;
		CString sname = str.Tokenize(L"=", start);
		if(sname.CompareNoCase(name) == 0)
			break;
		i+=str.GetLength();
	}

	if(i == size())
	{
		if (!value) // as we haven't found the variable we want to remove, just return
			return;
		if (i == 0) // make inserting into an empty environment work
		{
			this->push_back(L'\0');
			++i;
		}
		i -= 1; // roll back terminate \0\0
		this->push_back(L'\0');
	}

	CEnvironment::iterator it;
	it=this->begin();
	it += i;

	while(*it && i<size())
	{
		this->erase(it);
		it=this->begin();
		it += i;
	}

	if (value == nullptr) // remove the variable
	{
		this->erase(it);
		if (empty())
			baseptr = nullptr;
		else
			baseptr = data();
		return;
	}

	while(*name)
	{
		this->insert(it,*name++);
		++i;
		it= begin()+i;
	}

	this->insert(it, L'=');
	++i;
	it= begin()+i;

	while(*value)
	{
		this->insert(it,*value++);
		++i;
		it= begin()+i;
	}
	baseptr = data();
}

void CEnvironment::AddToPath(CString value)
{
	value.TrimRight(L'\\');
	if (value.IsEmpty())
		return;

	CString path = GetEnv(L"PATH").TrimRight(L';') + L';';

	// do not double add paths to %PATH%
	if (path.Find(value + L';') >= 0 || path.Find(value + L"\\;") >= 0)
		return;

	path += value;

	SetEnv(L"PATH", path);
}

int CGit::GetShortHASHLength() const
{
	return 8;
}

CString CGit::GetShortName(const CString& ref, REF_TYPE *out_type)
{
	CString str=ref;
	CString shortname;
	REF_TYPE type = CGit::UNKNOWN;

	if (CGit::GetShortName(str, shortname, L"refs/heads/"))
		type = CGit::LOCAL_BRANCH;
	else if (CGit::GetShortName(str, shortname, L"refs/remotes/"))
		type = CGit::REMOTE_BRANCH;
	else if (CStringUtils::EndsWith(str, L"^{}") && CGit::GetShortName(str, shortname, L"refs/tags/"))
		type = CGit::ANNOTATED_TAG;
	else if (CGit::GetShortName(str, shortname, L"refs/tags/"))
		type = CGit::TAG;
	else if (CGit::GetShortName(str, shortname, L"refs/stash"))
	{
		type = CGit::STASH;
		shortname = L"stash";
	}
	else if (CGit::GetShortName(str, shortname, L"refs/bisect/"))
	{
		CString bisectGood;
		CString bisectBad;
		g_Git.GetBisectTerms(&bisectGood, &bisectBad);
		wchar_t c;
		if (CStringUtils::StartsWith(shortname, bisectGood) && ((c = shortname.GetAt(bisectGood.GetLength())) == '-' || c == '\0'))
		{
			type = CGit::BISECT_GOOD;
			shortname = bisectGood;
		}

		if (CStringUtils::StartsWith(shortname, bisectBad) && ((c = shortname.GetAt(bisectBad.GetLength())) == '-' || c == '\0'))
		{
			type = CGit::BISECT_BAD;
			shortname = bisectBad;
		}

		if (CStringUtils::StartsWith(shortname, L"skip") && ((c = shortname.GetAt(4)) == '-' || c == '\0'))
		{
			type = CGit::BISECT_SKIP;
			shortname = L"skip";
		}
	}
	else if (CGit::GetShortName(str, shortname, L"refs/notes/"))
		type = CGit::NOTES;
	else if (CGit::GetShortName(str, shortname, L"refs/"))
		type = CGit::UNKNOWN;
	else
	{
		type = CGit::UNKNOWN;
		shortname = ref;
	}

	if(out_type)
		*out_type = type;

	return shortname;
}

bool CGit::UsingLibGit2(LIBGIT2_CMD cmd) const
{
	return m_IsUseLibGit2 && ((1 << cmd) & m_IsUseLibGit2_mask) ? true : false;
}

void CGit::SetGit2CredentialCallback(void* callback)
{
	g_Git2CredCallback = static_cast<git_credential_acquire_cb>(callback);
}

void CGit::SetGit2CertificateCheckCertificate(void* callback)
{
	g_Git2CheckCertificateCallback = static_cast<git_transport_certificate_check_cb>(callback);
}

CString CGit::GetUnifiedDiffCmd(const CTGitPath& path, const CString& rev1, const CString& rev2, bool bMerge, bool bCombine, int diffContext, bool bNoPrefix)
{
	CString cmd;
	if (rev2 == GitRev::GetWorkingCopy())
		cmd.Format(L"git.exe diff --stat%s -p --end-of-options %s --", bNoPrefix ? L" --no-prefix" : L"", static_cast<LPCWSTR>(rev1));
	else if (rev1 == GitRev::GetWorkingCopy())
		cmd.Format(L"git.exe diff -R --stat%s -p --end-of-options %s --", bNoPrefix ? L" --no-prefix" : L"", static_cast<LPCWSTR>(rev2));
	else
	{
		CString merge;
		if (bMerge)
			merge += L" -m";

		if (bCombine)
			merge += L" -c";

		CString unified;
		if (diffContext >= 0)
			unified.Format(L" --unified=%d", diffContext);
		cmd.Format(L"git.exe diff-tree -r -p%s%s --stat%s --end-of-options %s %s --", static_cast<LPCWSTR>(merge), static_cast<LPCWSTR>(unified), bNoPrefix ? L" --no-prefix" : L"", static_cast<LPCWSTR>(rev1), static_cast<LPCWSTR>(rev2));
	}

	if (!path.IsEmpty())
	{
		cmd += L" \"";
		cmd += path.GetGitPathString();
		cmd += L'"';
	}

	return cmd;
}

static void UnifiedDiffStatToFile(const git_buf* text, void* payload)
{
	ATLASSERT(payload && text);
	auto file = static_cast<FILE*>(payload);
	fwrite(text->ptr, 1, text->size, file);
	fwrite("\n", 1, 1, file);
}

static int UnifiedDiffToFile(const git_diff_delta * /* delta */, const git_diff_hunk * /* hunk */, const git_diff_line * line, void *payload)
{
	ATLASSERT(payload && line);
	auto file = static_cast<FILE*>(payload);
	if (line->origin == GIT_DIFF_LINE_CONTEXT || line->origin == GIT_DIFF_LINE_ADDITION || line->origin == GIT_DIFF_LINE_DELETION)
		fwrite(&line->origin, 1, 1, file);
	fwrite(line->content, 1, line->content_len, file);
	return 0;
}

static int resolve_to_tree(git_repository *repo, const char *identifier, git_tree **tree)
{
	ATLASSERT(repo && identifier && tree);

	/* try to resolve identifier */
	CAutoObject obj;
	if (git_revparse_single(obj.GetPointer(), repo, identifier))
		return -1;

	if (obj == nullptr)
		return GIT_ENOTFOUND;

	int err = 0;
	switch (git_object_type(obj))
	{
	case GIT_OBJECT_TREE:
		*tree = reinterpret_cast<git_tree*>(obj.Detach());
		break;
	case GIT_OBJECT_COMMIT:
		err = git_commit_tree(tree, reinterpret_cast<git_commit*>(static_cast<git_object*>(obj)));
		break;
	default:
		err = GIT_ENOTFOUND;
	}

	return err;
}

/* use libgit2 get unified diff */
static int GetUnifiedDiffLibGit2(const CTGitPath& path, const CString& revOld, const CString& revNew, std::function<void(const git_buf*, void*)> statCallback, git_diff_line_cb callback, void* data, bool /* bMerge */, bool bNoPrefix)
{
	CStringA tree1 = CUnicodeUtils::GetUTF8(revNew);
	CStringA tree2 = CUnicodeUtils::GetUTF8(revOld);

	CAutoRepository repo(g_Git.GetGitRepository());
	if (!repo)
		return -1;

	int isHeadOrphan = git_repository_head_unborn(repo);
	if (isHeadOrphan == 1)
		return 0;
	else if (isHeadOrphan != 0)
		return -1;

	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	CStringA pathA = CUnicodeUtils::GetUTF8(path.GetGitPathString());
	char *buf = pathA.GetBuffer();
	if (!pathA.IsEmpty())
	{
		opts.pathspec.strings = &buf;
		opts.pathspec.count = 1;
	}
	if (bNoPrefix)
	{
		opts.new_prefix = "";
		opts.old_prefix = "";
	}
	CAutoDiff diff;

	if (revNew == GitRev::GetWorkingCopy() || revOld == GitRev::GetWorkingCopy())
	{
		CAutoTree t1;
		CAutoDiff diff2;

		if (revNew != GitRev::GetWorkingCopy() && resolve_to_tree(repo, tree1, t1.GetPointer()))
			return -1;

		if (revOld != GitRev::GetWorkingCopy() && resolve_to_tree(repo, tree2, t1.GetPointer()))
			return -1;

		if (git_diff_tree_to_index(diff.GetPointer(), repo, t1, nullptr, &opts))
			return -1;

		if (git_diff_index_to_workdir(diff2.GetPointer(), repo, nullptr, &opts))
			return -1;

		if (git_diff_merge(diff, diff2))
			return -1;
	}
	else
	{
		if (tree1.IsEmpty() && tree2.IsEmpty())
			return -1;

		if (tree1.IsEmpty())
		{
			tree1 = tree2;
			tree2.Empty();
		}

		CAutoTree t1;
		CAutoTree t2;
		if (!tree1.IsEmpty() && resolve_to_tree(repo, tree1, t1.GetPointer()))
			return -1;

		if (tree2.IsEmpty())
		{
			/* don't check return value, there are not parent commit at first commit*/
			resolve_to_tree(repo, tree1 + "~1", t2.GetPointer());
		}
		else if (resolve_to_tree(repo, tree2, t2.GetPointer()))
			return -1;
		if (git_diff_tree_to_tree(diff.GetPointer(), repo, t2, t1, &opts))
			return -1;
	}

	CAutoDiffStats stats;
	if (git_diff_get_stats(stats.GetPointer(), diff))
		return -1;
	CAutoBuf statBuf;
	if (git_diff_stats_to_buf(statBuf, stats, GIT_DIFF_STATS_FULL, 0))
		return -1;
	statCallback(statBuf, data);

	for (size_t i = 0; i < git_diff_num_deltas(diff); ++i)
	{
		CAutoPatch patch;
		if (git_patch_from_diff(patch.GetPointer(), diff, i))
			return -1;

		if (git_patch_print(patch, callback, data))
			return -1;
	}

	pathA.ReleaseBuffer();

	return 0;
}

int CGit::GetUnifiedDiff(const CTGitPath& path, const CString& rev1, const CString& rev2, CString patchfile, bool bMerge, bool bCombine, int diffContext, bool bNoPrefix)
{
	if (UsingLibGit2(GIT_CMD_DIFF))
	{
		CAutoFILE file = _wfsopen(patchfile, L"wb", SH_DENYRW);
		if (!file)
			return -1;
		return GetUnifiedDiffLibGit2(path, rev1, rev2, UnifiedDiffStatToFile, UnifiedDiffToFile, file, bMerge, bNoPrefix);
	}
	else
	{
		CString cmd;
		cmd = GetUnifiedDiffCmd(path, rev1, rev2, bMerge, bCombine, diffContext, bNoPrefix);
		gitLastErr.Empty();
		return RunLogFile(cmd, patchfile, &gitLastErr);
	}
}

static void UnifiedDiffStatToStringA(const git_buf* text, void* payload)
{
	ATLASSERT(payload && text);
	auto str = static_cast<CStringA*>(payload);
	str->Append(text->ptr, SafeSizeToInt(text->size));
	str->AppendChar('\n');
}

static int UnifiedDiffToStringA(const git_diff_delta * /*delta*/, const git_diff_hunk * /*hunk*/, const git_diff_line *line, void *payload)
{
	ATLASSERT(payload && line);
	auto str = static_cast<CStringA*>(payload);
	if (line->origin == GIT_DIFF_LINE_CONTEXT || line->origin == GIT_DIFF_LINE_ADDITION || line->origin == GIT_DIFF_LINE_DELETION)
		str->Append(&line->origin, 1);
	str->Append(line->content, SafeSizeToInt(line->content_len));
	return 0;
}

int CGit::GetUnifiedDiff(const CTGitPath& path, const CString& rev1, const CString& rev2, CStringA& buffer, bool bMerge, bool bCombine, int diffContext)
{
	if (UsingLibGit2(GIT_CMD_DIFF))
		return GetUnifiedDiffLibGit2(path, rev1, rev2, UnifiedDiffStatToStringA, UnifiedDiffToStringA, &buffer, bMerge, false);
	else
	{
		CString cmd;
		cmd = GetUnifiedDiffCmd(path, rev1, rev2, bMerge, bCombine, diffContext);
		BYTE_VECTOR vector;
		const int ret = Run(cmd, &vector);
		if (!vector.empty())
		{
			vector.push_back(0); // vector is not NUL terminated
			buffer.Append(vector.data());
		}
		return ret;
	}
}

int CGit::GitRevert(int parent, const CGitHash &hash)
{
	if (UsingLibGit2(GIT_CMD_REVERT))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CAutoCommit commit;
		if (git_commit_lookup(commit.GetPointer(), repo, hash))
			return -1;

		git_revert_options revert_opts = GIT_REVERT_OPTIONS_INIT;
		revert_opts.mainline = parent;
		return !git_revert(repo, commit, &revert_opts) ? 0 : -1;
	}
	else
	{
		CString cmd, merge;
		if (parent)
			merge.Format(L"-m %d ", parent);
		cmd.Format(L"git.exe revert --no-edit --no-commit %s%s", static_cast<LPCWSTR>(merge), static_cast<LPCWSTR>(hash.ToString()));
		gitLastErr = cmd + L'\n';
		if (Run(cmd, &gitLastErr, CP_UTF8))
			return -1;
		else
		{
			gitLastErr.Empty();
			return 0;
		}
	}
}

int CGit::DeleteRef(const CString& reference)
{
	if (UsingLibGit2(GIT_CMD_DELETETAGBRANCH))
	{
		CAutoRepository repo(GetGitRepository());
		if (!repo)
			return -1;

		CStringA refA;
		if (CStringUtils::EndsWith(reference, L"^{}"))
			refA = CUnicodeUtils::GetUTF8(reference.Left(reference.GetLength() - static_cast<int>(wcslen(L"^{}"))));
		else
			refA = CUnicodeUtils::GetUTF8(reference);

		CAutoReference ref;
		if (git_reference_lookup(ref.GetPointer(), repo, refA))
			return -1;

		int result = -1;
		if (git_reference_is_tag(ref))
			result = git_tag_delete(repo, git_reference_shorthand(ref));
		else if (git_reference_is_branch(ref))
			result = git_branch_delete(ref);
		else if (git_reference_is_remote(ref))
			result = git_branch_delete(ref);
		else
			result = git_reference_delete(ref);

		return result;
	}
	else
	{
		CString cmd, shortname;
		if (GetShortName(reference, shortname, L"refs/heads/"))
			cmd.Format(L"git.exe branch -D -- %s", static_cast<LPCWSTR>(shortname));
		else if (GetShortName(reference, shortname, L"refs/tags/"))
			cmd.Format(L"git.exe tag -d -- %s", static_cast<LPCWSTR>(shortname));
		else if (GetShortName(reference, shortname, L"refs/remotes/"))
			cmd.Format(L"git.exe branch -r -D -- %s", static_cast<LPCWSTR>(shortname));
		else
		{
			gitLastErr = L"unsupported reference type: " + reference;
			return -1;
		}

		gitLastErr.Empty();
		if (Run(cmd, &gitLastErr, CP_UTF8))
			return -1;

		gitLastErr.Empty();
		return 0;
	}
}

bool CGit::LoadTextFile(const CString &filename, CString &msg)
{
	if (!PathFileExists(filename))
		return false;

	CAutoFILE pFile = _wfsopen(filename, L"rb", SH_DENYWR);
	if (!pFile)
	{
		::MessageBox(nullptr, L"Could not open " + filename, L"TortoiseGit", MB_ICONERROR);
		return true; // load no further files
	}

	CStringA str;
	do
	{
		char s[8196] = { 0 };
		int read = static_cast<int>(fread(s, sizeof(char), sizeof(s), pFile));
		if (read == 0)
			break;
		str += CStringA(s, read);
	} while (true);
	msg += CUnicodeUtils::GetUnicode(str);
	msg.Replace(L"\r\n", L"\n");
	msg.TrimRight(L'\n');
	msg += L'\n';

	return true; // load no further files
}

int CGit::GetWorkingTreeChanges(CTGitPathList& result, bool amend, const CTGitPathList* filterlist, bool includedStaged /* = false */, bool getStagingStatus /* = false */)
{
	if (IsInitRepos())
		return GetInitAddList(result, getStagingStatus);

	BYTE_VECTOR out;

	int count = 1;
	if (filterlist)
		count = filterlist->GetCount();
	ATLASSERT(count > 0);

	CString head = L"HEAD";
	if (amend)
		head = L"HEAD~1";

	CString gitStatusParams;
	if (ms_LastMsysGitVersion >= ConvertVersionToInt(2, 17, 0))
		gitStatusParams = L" --no-ahead-behind";

	for (int i = 0; i < count; ++i)
	{
		ATLASSERT(!filterlist || !(*filterlist)[i].GetGitPathString().IsEmpty()); // pathspec must not be empty, be compatible with Git >= 2.16.0
		BYTE_VECTOR cmdout;
		CString cmd;
		if (ms_bCygwinGit || ms_bMsys2Git)
		{
			// Prevent showing all files as modified when using cygwin's git
			if (!filterlist)
				cmd.Format(L"git.exe status%s --", static_cast<LPCWSTR>(gitStatusParams));
			else
				cmd.Format(L"git.exe status%s -- \"%s\"", static_cast<LPCWSTR>(gitStatusParams), static_cast<LPCWSTR>((*filterlist)[i].GetGitPathString()));
			Run(cmd, &cmdout);
			cmdout.clear();
		}

		// also list staged files which will be in the commit
		if (includedStaged || !filterlist)
			Run(L"git.exe diff-index --cached --raw " + head + L" --numstat -C -M -z --", &cmdout);
		else
		{
			cmd.Format(L"git.exe diff-index --cached --raw %s --numstat -C -M -z -- \"%s\"", static_cast<LPCWSTR>(head), static_cast<LPCWSTR>((*filterlist)[i].GetGitPathString()));
			Run(cmd, &cmdout);
		}

		if (!filterlist)
			cmd.Format(L"git.exe diff-index --raw %s --numstat -C%d%% -M%d%% -z --", static_cast<LPCWSTR>(head), ms_iSimilarityIndexThreshold, ms_iSimilarityIndexThreshold);
		else
			cmd.Format(L"git.exe diff-index --raw %s --numstat -C%d%% -M%d%% -z -- \"%s\"", static_cast<LPCWSTR>(head), ms_iSimilarityIndexThreshold, ms_iSimilarityIndexThreshold, static_cast<LPCWSTR>((*filterlist)[i].GetGitPathString()));

		BYTE_VECTOR cmdErr;
		if (Run(cmd, &cmdout, &cmdErr))
		{
			CString str{ cmdErr };
			if (str.IsEmpty())
				str.Format(L"\"%s\" exited with an error code, but did not output any error message", static_cast<LPCWSTR>(cmd));
			MessageBox(nullptr, str, L"TortoiseGit", MB_OK | MB_ICONERROR);
		}

		out.append(cmdout, 0);
	}
	result.ParserFromLog(out);

	if (getStagingStatus)
	{
		// This will show staged files regardless of any filterlist, so that it has the same behavior that the commit window has when staging support is disabled
		BYTE_VECTOR cmdStagedUnfilteredOut, cmdUnstagedUnfilteredOut;
		CString cmd = L"git.exe diff-index --cached --raw " + head + L" --numstat -C -M -z --";
		Run(cmd, &cmdStagedUnfilteredOut);

		CTGitPathList stagedUnfiltered;
		stagedUnfiltered.ParserFromLog(cmdStagedUnfilteredOut);

		cmd = L"git.exe diff-files --raw --numstat -C -M -z --";
		Run(cmd, &cmdUnstagedUnfilteredOut);

		CTGitPathList unstagedUnfiltered;
		unstagedUnfiltered.ParserFromLog(cmdUnstagedUnfilteredOut); // Necessary to detect partially staged files outside the filterlist

		// File shows up both in the output of diff-index --cached and diff-files: partially staged
		// File shows up only in the output of diff-index --cached: totally staged
		// File shows up only in the output of diff-files: totally unstaged
		// TODO: This is inefficient. It would be better if ParserFromLog received the output of each command
		// separately and did this processing there, dropping the new function UpdateStagingStatusFromPath entirely.
		for (int j = 0; j < stagedUnfiltered.GetCount(); ++j)
		{
			CString path = stagedUnfiltered[j].GetGitPathString();
			if (unstagedUnfiltered.LookForGitPath(path))
				result.UpdateStagingStatusFromPath(path, CTGitPath::StagingStatus::PartiallyStaged);
			else
				result.UpdateStagingStatusFromPath(path, CTGitPath::StagingStatus::TotallyStaged);
		}
		for (int j = 0; j < unstagedUnfiltered.GetCount(); ++j)
		{
			CString path = unstagedUnfiltered[j].GetGitPathString();
			if (!stagedUnfiltered.LookForGitPath(path))
				result.UpdateStagingStatusFromPath(path, CTGitPath::StagingStatus::TotallyUnstaged);
		}
		// make sure conflicted files show up as unstaged instead of partially staged
		for (int j = 0; j < result.GetCount(); ++j)
		{
			if (result[j].m_Action & CTGitPath::LOGACTIONS_UNMERGED)
				const_cast<CTGitPath&>(result[j]).m_stagingStatus = CTGitPath::StagingStatus::TotallyUnstaged;
		}
	}

	std::map<CString, int> duplicateMap;
	for (int i = 0; i < result.GetCount(); ++i)
		duplicateMap.insert(std::pair<CString, int>(result[i].GetGitPathString(), i));

	// handle delete conflict case, when remote : modified, local : deleted.
	for (int i = 0; i < count; ++i)
	{
		BYTE_VECTOR cmdout;
		CString cmd;

		if (!filterlist)
			cmd = L"git.exe ls-files -u -t -z";
		else
			cmd.Format(L"git.exe ls-files -u -t -z -- \"%s\"", static_cast<LPCWSTR>((*filterlist)[i].GetGitPathString()));

		Run(cmd, &cmdout);

		CTGitPathList conflictlist;
		conflictlist.ParserFromLsFile(cmdout);
		for (int j = 0; j < conflictlist.GetCount(); ++j)
		{
			auto existing = duplicateMap.find(conflictlist[j].GetGitPathString());
			if (existing != duplicateMap.end())
			{
				CTGitPath& p = const_cast<CTGitPath&>(result[existing->second]);
				p.m_Action |= CTGitPath::LOGACTIONS_UNMERGED;
			}
			else
			{
				// should we ever get here?
				ASSERT(false);
				result.AddPath(conflictlist[j]);
				duplicateMap.insert(std::pair<CString, int>(result[i].GetGitPathString(), result.GetCount() - 1));
			}
		}
	}

	static bool useOldLSFilesDBehaviorKS = CRegDWORD(L"Software\\TortoiseGit\\OldLSFilesDBehaviorKS", FALSE, false, HKEY_LOCAL_MACHINE) == TRUE; // TODO: remove kill-switch
	// handle source files of file renames/moves (issue #860)
	// if a file gets renamed and the new file "git add"ed, diff-index doesn't list the source file anymore
	for (int i = 0; i < count; ++i)
	{
		BYTE_VECTOR cmdout;
		CString cmd;

		if (!filterlist)
		{
			if (!useOldLSFilesDBehaviorKS)
				cmd = L"git.exe diff --name-only --diff-filter=D -z";
			else
				cmd = L"git.exe ls-files -d -z";
		}
		else
		{
			if (!useOldLSFilesDBehaviorKS)
				cmd.Format(L"git.exe diff --name-only --diff-filter=D -z -- \"%s\"", static_cast<LPCWSTR>((*filterlist)[i].GetGitPathString()));
			else
				cmd.Format(L"git.exe ls-files -d -z -- \"%s\"", static_cast<LPCWSTR>((*filterlist)[i].GetGitPathString()));
		}

		Run(cmd, &cmdout);

		CTGitPathList deletelist;
		deletelist.ParserFromLsFileSimple(cmdout, CTGitPath::LOGACTIONS_DELETED | CTGitPath::LOGACTIONS_MISSING);
		for (int j = 0; j < deletelist.GetCount(); ++j)
		{
			auto existing = duplicateMap.find(deletelist[j].GetGitPathString());
			if (existing == duplicateMap.end())
			{
				result.AddPath(deletelist[j]);
				duplicateMap.insert(std::pair<CString, int>(result[i].GetGitPathString(), result.GetCount() - 1));
			}
			else
			{
				CTGitPath& p = const_cast<CTGitPath&>(result[existing->second]);
				p.m_Action |= CTGitPath::LOGACTIONS_MISSING;
				result.m_Action |= CTGitPath::LOGACTIONS_MISSING;
			}
		}
	}

	return 0;
}

void CGit::GetBisectTerms(CString* good, CString* bad)
{
	static CString lastGood;
	static CString lastBad;
	static ULONGLONG lastRead = 0;

	SCOPE_EXIT
	{
		if (bad)
			*bad = lastBad;
		if (good)
			*good = lastGood;
	};

#ifndef GOOGLETEST_INCLUDE_GTEST_GTEST_H_
	// add some caching here, because this method might be called multiple times in a short time from LogDlg and RevisionGraph
	// as we only read a small file the performance effects should be negligible
	if (lastRead + 5000 > GetTickCount64())
		return;
#endif

	lastGood = L"good";
	lastBad = L"bad";

	CString adminDir;
	if (!GitAdminDir::GetAdminDirPath(m_CurrentDir, adminDir))
		return;

	CString termsFile = adminDir + L"BISECT_TERMS";
	CAutoFILE fp;
	_wfopen_s(fp.GetPointer(), termsFile, L"rb");
	if (!fp)
		return;
	char badA[MAX_PATH] = { 0 };
	fgets(badA, sizeof(badA), fp);
	size_t len = strlen(badA);
	if (len > 0 && badA[len - 1] == '\n')
		badA[len - 1] = '\0';
	char goodA[MAX_PATH] = { 0 };
	fgets(goodA, sizeof(goodA), fp);
	len = strlen(goodA);
	if (len > 0 && goodA[len - 1] == '\n')
		goodA[len - 1] = '\0';
	lastGood = CUnicodeUtils::GetUnicode(goodA);
	lastBad = CUnicodeUtils::GetUnicode(badA);
	lastRead = GetTickCount64();
}

int CGit::GetGitVersion(CString* versiondebug, CString* errStr)
{
	CString version, err;
	if (Run(L"git.exe --version", &version, &err, CP_UTF8))
	{
		if (errStr)
			*errStr = err;
		return -1;
	}

	int ver = 0;
	if (versiondebug)
		*versiondebug = version;

	try
	{
		int start = 0;
		CString str = version.Tokenize(L".", start);
		int space = str.ReverseFind(L' ');
		str = str.Mid(space + 1, start);
		ver = _wtol(str);
		ver <<= 24;

		version = version.Mid(start);
		start = 0;

		str = version.Tokenize(L".", start);
		ver |= (_wtol(str) & 0xFF) << 16;

		str = version.Tokenize(L".", start);
		ver |= (_wtol(str) & 0xFF) << 8;

		str = version.Tokenize(L".", start);
		ver |= (_wtol(str) & 0xFF);
	}
	catch (...)
	{
		if (!ver)
			return -1;
	}

	return ver;
}

int CGit::GetGitNotes(const CGitHash& hash, CString& notes)
{
	CAutoRepository repo(GetGitRepository());
	if (!repo)
		return -1;

	CAutoNote note;
	const int ret = git_note_read(note.GetPointer(), repo, nullptr, hash);
	if (ret == GIT_ENOTFOUND)
	{
		notes.Empty();
		return 0;
	}
	else if (ret)
		return -1;
	notes = CUnicodeUtils::GetUnicode(git_note_message(note));
	return 0;
}

int CGit::SetGitNotes(const CGitHash& hash, const CString& notes)
{
	CAutoRepository repo(GetGitRepository());
	if (!repo)
		return -1;

	CAutoSignature signature;
	if (git_signature_default(signature.GetPointer(), repo) < 0)
		return -1;

	git_oid oid;
	if (git_note_create(&oid, repo, nullptr, signature, signature, hash, CUnicodeUtils::GetUTF8(notes), 1) < 0)
		return -1;

	return 0;
}

int CGit::GetSubmodulePointer(SubmoduleInfo& submoduleinfo) const
{
	submoduleinfo.Empty();

	if (CRegDWORD(L"Software\\TortoiseGit\\LogShowSuperProjectSubmodulePointer", TRUE) != TRUE)
		return 0;

	if (GitAdminDir::IsBareRepo(g_Git.m_CurrentDir))
		return -1;
	CString superprojectRoot;
	GitAdminDir::HasAdminDir(g_Git.m_CurrentDir, false, &superprojectRoot);
	if (superprojectRoot.IsEmpty())
		return -1;

	CAutoRepository repo(superprojectRoot);
	if (!repo)
		return -1;
	CAutoIndex index;
	if (git_repository_index(index.GetPointer(), repo))
		return -1;

	CString submodulePath;
	if (superprojectRoot[superprojectRoot.GetLength() - 1] == L'\\')
		submodulePath = g_Git.m_CurrentDir.Right(g_Git.m_CurrentDir.GetLength() - superprojectRoot.GetLength());
	else
		submodulePath = g_Git.m_CurrentDir.Right(g_Git.m_CurrentDir.GetLength() - superprojectRoot.GetLength() - 1);
	submodulePath.Replace(L'\\', L'/');

	// if current submodule is in conflict state, return the relevant hashes
	const git_index_entry* ancestor{};
	const git_index_entry* our{};
	const git_index_entry* their{};
	if (!git_index_conflict_get(&ancestor, &our, &their, index, CUnicodeUtils::GetUTF8(submodulePath)))
	{
		if (our)
			submoduleinfo.mergeconflictMineHash = our->id;
		if (their)
			submoduleinfo.mergeconflictTheirsHash = their->id;

		CTGitPath superProject{superprojectRoot};
		if (superProject.IsRebaseActive())
		{
			submoduleinfo.mineLabel = L"super-project-rebase-head";
			submoduleinfo.theirsLabel = L"super-project-head";
		}
		else
		{
			submoduleinfo.mineLabel = L"super-project-head";
			submoduleinfo.theirsLabel = L"super-project-merge-head";
		}
		return 0;
	}

	// determine hash of submodule
	const git_index_entry* entry = git_index_get_bypath(index, CUnicodeUtils::GetUTF8(submodulePath), 0);
	if (!entry)
		return -1;

	submoduleinfo.superProjectHash = entry->id;
	return 0;
}

// similar code in CTGitPath::ParserFromLsFile
int CGit::ParseConflictHashesFromLsFile(const BYTE_VECTOR& out, CGitHash& baseHash, bool& baseIsFile, CGitHash& mineHash, bool& mineIsFile, CGitHash& remoteHash, bool& remoteIsFile)
{
	size_t pos = 0;
	const size_t end = out.size();
	if (end == 0)
		return 1;
	while (pos < end)
	{
		const size_t lineStart = pos;

		if (out[pos] != 'M')
		{
			ASSERT(false && "this should never happen as this method should only be called for output of git ls-files -u -t -z");
			return -1;
		}

		pos = out.find(' ', pos); // advance to mode
		if (pos == CGitByteArray::npos)
			return -1;

		const size_t modeStart = pos + 1;
		pos = out.find(' ', modeStart); // advance to hash
		if (pos == CGitByteArray::npos)
			return -1;

		const size_t hashStart = pos + 1;
		pos = out.find(' ', hashStart); // advance to Stage
		if (pos == CGitByteArray::npos)
			return -1;

		const size_t stageStart = pos + 1;
		pos = out.find('\t', stageStart); // advance to filename
		if (pos == CGitByteArray::npos || stageStart - 1 - hashStart >= INT_MAX)
			return -1;

		++pos;
		const size_t fileNameEnd = out.find(0, pos);
		if (fileNameEnd == CGitByteArray::npos || fileNameEnd == pos || pos - lineStart != 52)
			return -1;

		std::string_view hash{ &out[hashStart], stageStart - 1 - hashStart };
		long mode = strtol(&out[modeStart], nullptr, 10);
		const int stage = strtol(&out[stageStart], nullptr, 10);
		if (stage == 0)
			return -1;
		else if (stage == 1)
		{
			baseHash = CGitHash::FromHexStr(hash);
			baseIsFile = mode != 160000;
		}
		else if (stage == 2)
		{
			mineHash = CGitHash::FromHexStr(hash);
			mineIsFile = mode != 160000;
		}
		else if (stage == 3)
		{
			remoteHash = CGitHash::FromHexStr(hash);
			remoteIsFile = mode != 160000;
		}

		pos = out.findNextString(pos);
	}

	return 0;
}
