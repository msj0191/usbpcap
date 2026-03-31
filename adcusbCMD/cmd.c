/*
 * Copyright (c) 2013-2018 Tomasz Mon <desowin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <initguid.h>
#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <Shellapi.h>
#include <Shlwapi.h>
#include <Usbiodef.h>
#include "filters.h"
#include "thread.h"
#include "enum.h"
#include "getopt.h"
#include "roothubs.h"
#include "version.h"
#include "descriptors.h"
#include "adcusb.h"
#include "iocontrol.h"
#include "log.h"

#define INPUT_BUFFER_SIZE 1024

#define DEFAULT_INTERNAL_KERNEL_BUFFER_SIZE (1024*1024)
#define DEFAULT_SNAPSHOT_LENGTH             (65535)

static BOOL IsElevated()
{
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize))
        {
            fRet = Elevation.TokenIsElevated;
        }
        else
        {
            DWORD err = GetLastError();
            if (err == ERROR_INVALID_PARAMETER)
            {
                /* Running on Windows XP.
                 * Check if executed as administrator by reading Local Service key.
                 */
                HKEY key;
                if (ERROR_SUCCESS == RegOpenKey(HKEY_USERS, "S-1-5-19", &key))
                {
                    fRet = TRUE;
                }
                else
                {
                    /* If we were executed with SW_HIDE then the runas window won't be shown.
                     * In such case pretend here that we are running as administrator so
                     * the process will fail (and not simply be waiting indefinitely
                     * without giving any clue).
                     */
                    STARTUPINFO info;
                    GetStartupInfo(&info);
                    if (info.wShowWindow == SW_HIDE)
                    {
                        fRet = TRUE;
                    }
                }
            }
            else
            {
                fprintf(stderr, "GetTokenInformation failed with code %d\n", err);
            }
        }
    }

    if (hToken)
    {
        CloseHandle(hToken);
    }

    return fRet;
}

/*
 * GetModuleFullName:
 *
 *    Gets the full path and file name of the specified module and returns the length on success,
 *    (which does not include the terminating NUL character) 0 otherwise.  Use GetLastError() to
 *    get extended error information.
 *
 *       hModule              [in] Handle to a module loaded by the calling process, or NULL to
 *                            use the current process module handle.  This function does not
 *                            retrieve the name for modules that were loaded using LoadLibraryEx
 *                            with the LOAD_LIBRARY_AS_DATAFILE flag. For more information, see
 *                            LoadLibraryEx.
 *
 *       pszBuffer            [out] Pointer to the buffer which receives the module full name.
 *                            This paramater may be NULL, in which case the function returns the
 *                            size of the buffer in characters required to contain the full name,
 *                            including a NUL terminating character.
 *
 *       nMaxChars            [in] Specifies the size of the buffer in characters.  This must be
 *                            0 when pszBuffer is NULL, otherwise the function fails.
 *
 *       ppszFileName         [out] On return, the referenced pointer is assigned a position in
 *                            the buffer to the module's file name only.  This parameter may be
 *                            NULL if the file name is not required.
 */
EXTERN_C int WINAPI GetModuleFullName(__in HMODULE hModule, __out LPWSTR pszBuffer,
                                      __in int nMaxChars, __out LPWSTR* ppszFileName)
{
    /* Determine required buffer size when requested */
    int nLength = 0;
    DWORD dwStatus = NO_ERROR;

    /* Validate parameters */
    if (dwStatus == NO_ERROR)
    {
        if (pszBuffer == NULL && (nMaxChars != 0 || ppszFileName != NULL))
        {
             dwStatus = ERROR_INVALID_PARAMETER;
        }
        else if (pszBuffer != NULL && nMaxChars < 1)
        {
             dwStatus = ERROR_INVALID_PARAMETER;
        }
    }

    if (dwStatus == NO_ERROR)
    {
        if (pszBuffer == NULL)
        {
            HANDLE hHeap = GetProcessHeap();

            WCHAR  cwBuffer[2048] = { 0 };
            LPWSTR pszBuffer      = cwBuffer;
            DWORD  dwMaxChars     = _countof(cwBuffer);
            DWORD  dwLength       = 0;

            LPWSTR pszNew;
            SIZE_T nSize;

            for (;;)
            {
                /* Try to get the module's full path and file name */
                dwLength = GetModuleFileNameW(hModule, pszBuffer, dwMaxChars);

                if (dwLength == 0)
                {
                    dwStatus = GetLastError();
                    break;
                }

                /* If succeeded, return buffer size requirement:
                 *    o  Adds one for the terminating NUL character.
                 */
                if (dwLength < dwMaxChars)
                {
                    nLength = (int)dwLength + 1;
                    break;
                }

                /* Check the maximum supported full name length:
                 *    o  Assumes support for HPFS, NTFS, or VTFS of ~32K.
                 */
                if (dwMaxChars >= 32768U)
                {
                    dwStatus = ERROR_BUFFER_OVERFLOW;
                    break;
                }

                /* Double the size of our buffer and try again */
                dwMaxChars *= 2;

                pszNew = (pszBuffer == cwBuffer ? NULL : pszBuffer);
                nSize  = (SIZE_T)dwMaxChars * sizeof(WCHAR);

                if (pszNew == NULL)
                {
                    pszNew = (LPWSTR)HeapAlloc(hHeap, 0, nSize);
                }
                else
                {
                    LPWSTR pszTmp;
                    pszTmp = (LPWSTR)HeapReAlloc(hHeap, 0, pszNew, nSize);
                    if (pszTmp == NULL)
                    {
                        HeapFree(hHeap, 0, pszNew);
                        if (pszNew == pszBuffer)
                        {
                            pszBuffer = NULL;
                        }
                        pszNew = NULL;
                    }
                    else
                    {
                        pszNew = pszTmp;
                    }
                }

                if (pszNew == NULL)
                {
                    dwStatus = ERROR_OUTOFMEMORY;
                    break;
                }

                pszBuffer = pszNew;
            }

            /* Free the temporary buffer if allocated */
            if (pszBuffer != cwBuffer)
            {
                if (!HeapFree(hHeap, 0, pszBuffer))
                {
                   dwStatus = GetLastError();
                }
            }
        }
    }

    /* Get the module's full name and pointer to file name when requested */
    if (dwStatus == NO_ERROR)
    {
        if (pszBuffer != NULL)
        {
            nLength = (int)GetModuleFileNameW(hModule, pszBuffer, nMaxChars);

            if (nLength <= 0 || nLength == nMaxChars)
            {
                dwStatus = GetLastError();
            }
            else if (ppszFileName != NULL)
            {
                LPWSTR pszItr;
                *ppszFileName = pszBuffer;

                for (pszItr = pszBuffer; *pszItr != L'\0'; ++pszItr)
                {
                    if (*pszItr == L'\\' || *pszItr == L'/')
                    {
                        *ppszFileName = pszItr + 1;
                    }
               }
            }
         }
    }

    /* Return full name length or 0 on error */
    if (dwStatus != NO_ERROR)
    {
        nLength = 0;

        SetLastError(dwStatus);
    }

    return nLength;
}

/**
 *  Generates command line for worker process.
 *
 *  \param[in] data thread_data containing capture configuration.
 *  \param[out] appPath pointer to store application path. Must be freed using free().
 *  \param[out] appCmdLine commandline for worker process. Must be freed using free().
 *  \param[out] pcap_handle handle to pcap pipe (used if filename is "-"),
 *              if not writing to standard output it is set to INVALID_HANDLE_VALUE.
 *
 * \return BOOL TRUE on success, FALSE otherwise.
 */
static BOOL generate_worker_command_line(struct thread_data *data,
                                         PWSTR *appPath,
                                         PWSTR *appCmdLine,
                                         HANDLE *pcap_handle)
{
    PWSTR exePath;
    int exePathLen;
    PWSTR cmdLine = NULL;
    int cmdLineLen;
    PWSTR pipeName = NULL;
    int nChars;

    *pcap_handle = INVALID_HANDLE_VALUE;

    exePathLen = GetModuleFullName(NULL, NULL, 0, NULL);
    exePath = (WCHAR *)malloc(exePathLen * sizeof(WCHAR));

    if (exePath == NULL)
    {
        fprintf(stderr, "Failed to get module path\n");
        return FALSE;
    }

    GetModuleFullName(NULL, exePath, exePathLen, NULL);

    if (strncmp(data->filename, "-", 2) == 0)
    {
        /* Need to create pipe */
        WCHAR *tmp;
        int nChars = sizeof("\\\\.\\pipe\\") + strlen(data->device) + 1;
        pipeName = malloc((nChars + 1) * sizeof(WCHAR));
        if (pipeName == NULL)
        {
            fprintf(stderr, "Failed to allocate pipe name\n");
            free(exePath);
            return FALSE;
        }
        swprintf_s(pipeName, nChars,  L"\\\\.\\pipe\\%S", data->device);
        for (tmp = &pipeName[sizeof("\\\\.\\pipe\\")]; *tmp; tmp++)
        {
            if (*tmp == L'\\')
            {
                *tmp = L'_';
            }
        }

        *pcap_handle = CreateNamedPipeW(pipeName,
                                        /* Pipe is used for elevated worker -> caller process communication.
                                         * It is full duplex to allow caller to notice elevated worker that
                                         * it should terminate (read from this pipe in elevated worker will
                                         * result in ERROR_BROKEN_PIPE).
                                         */
                                        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                        2 /* Max instances of pipe */,
                                        data->bufferlen, data->bufferlen,
                                        0, NULL);


        if (*pcap_handle == INVALID_HANDLE_VALUE)
        {
            fprintf(stderr, "Failed to create named pipe - %d\n", GetLastError());
            free(exePath);
            free(pipeName);
            return FALSE;
        }
    }
    else
    {
        *pcap_handle = INVALID_HANDLE_VALUE;
    }

#define WORKER_CMD_LINE_FORMATTER             L"-d %S -b %u -o %S"
#define WORKER_CMD_LINE_FORMATTER_PIPE        L"-d %S -b %u -o %s"

#define WORKER_CMD_LINE_FORMATTER_SNAPLEN     L" -s %u"
#define WORKER_CMD_LINE_FORMATTER_DEVICES     L" --devices %S"
#define WORKER_CMD_LINE_FORMATTER_CAPTURE_ALL L" --capture-from-all-devices"
#define WORKER_CMD_LINE_FORMATTER_CAPTURE_NEW L" --capture-from-new-devices"
#define WORKER_CMD_LINE_FORMATTER_INJECT_DESCRIPTORS L" --inject-descriptors"

    cmdLineLen = MultiByteToWideChar(CP_ACP, 0, data->device, -1, NULL, 0);
    cmdLineLen += (pipeName == NULL) ? strlen(data->filename) : wcslen(pipeName);
    cmdLineLen += wcslen(WORKER_CMD_LINE_FORMATTER);
    cmdLineLen += 9 /* maximum bufferlen in characters */;
    cmdLineLen += 1 /* NULL termination */;
    cmdLineLen += wcslen(WORKER_CMD_LINE_FORMATTER_SNAPLEN);
    cmdLineLen += 10 /* maximum snaplen in characters */;
    cmdLineLen += wcslen(WORKER_CMD_LINE_FORMATTER_DEVICES);
    cmdLineLen += wcslen(WORKER_CMD_LINE_FORMATTER_CAPTURE_ALL);
    cmdLineLen += wcslen(WORKER_CMD_LINE_FORMATTER_CAPTURE_NEW);
    cmdLineLen += wcslen(WORKER_CMD_LINE_FORMATTER_INJECT_DESCRIPTORS);
    cmdLineLen += (data->address_list == NULL) ? 0 : strlen(data->address_list);

    cmdLine = (PWSTR)malloc(cmdLineLen * sizeof(WCHAR));

    if (cmdLine == NULL)
    {
        fprintf(stderr, "Failed to allocate command line\n");
        free(exePath);
        free(pipeName);
        return FALSE;
    }

    if (pipeName == NULL)
    {
        nChars = swprintf_s(cmdLine,
                            cmdLineLen,
                            WORKER_CMD_LINE_FORMATTER,
                            data->device,
                            data->bufferlen,
                            data->filename);
    }
    else
    {
        nChars = swprintf_s(cmdLine,
                            cmdLineLen,
                            WORKER_CMD_LINE_FORMATTER_PIPE,
                            data->device,
                            data->bufferlen,
                            pipeName);
    }

    if (data->snaplen != DEFAULT_SNAPSHOT_LENGTH)
    {
        nChars += swprintf_s(&cmdLine[nChars],
                             cmdLineLen - nChars,
                             WORKER_CMD_LINE_FORMATTER_SNAPLEN,
                             data->snaplen);
    }

    if (data->address_list != NULL)
    {
        nChars += swprintf_s(&cmdLine[nChars],
                             cmdLineLen - nChars,
                             WORKER_CMD_LINE_FORMATTER_DEVICES,
                             data->address_list);
    }

    if (data->capture_all)
    {
        nChars += swprintf_s(&cmdLine[nChars],
                             cmdLineLen - nChars,
                             WORKER_CMD_LINE_FORMATTER_CAPTURE_ALL);
    }

    if (data->capture_new)
    {
        nChars += swprintf_s(&cmdLine[nChars],
                             cmdLineLen - nChars,
                             WORKER_CMD_LINE_FORMATTER_CAPTURE_NEW);
    }

    if (data->inject_descriptors)
    {
        nChars += swprintf_s(&cmdLine[nChars],
                             cmdLineLen - nChars,
                             WORKER_CMD_LINE_FORMATTER_INJECT_DESCRIPTORS);
    }
#undef WORKER_CMD_LINE_FORMATTER_PIPE
#undef WORKER_CMD_LINE_FORMATTER

#undef WORKER_CMD_LINE_FORMATTER_INJECT_DESCRIPTORS
#undef WORKER_CMD_LINE_FORMATTER_CAPTURE_NEW
#undef WORKER_CMD_LINE_FORMATTER_CAPTURE_ALL
#undef WORKER_CMD_LINE_FORMATTER_DEVICES
#undef WORKER_CMD_LINE_FORMATTER_SNAPLEN

    free(pipeName);

    *appPath = exePath;
    *appCmdLine = cmdLine;
    return TRUE;
}

/**
 *  Creates elevated worker process.
 *
 *  \param[in] appPath path to elevated worker module
 *  \param[in] cmdLine commandline to start elevated worker with
 *
 *  \return Handle to created process.
 */
static HANDLE create_elevated_worker(PWSTR appPath, PWSTR cmdLine)
{
    BOOL bSuccess = FALSE;
    SHELLEXECUTEINFOW exInfo = { 0 };

    exInfo.cbSize = sizeof(exInfo);
    exInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    exInfo.hwnd = NULL;
    exInfo.lpVerb = L"runas";
    exInfo.lpFile = appPath;
    exInfo.lpParameters = cmdLine;
    exInfo.lpDirectory = NULL;
    exInfo.nShow = SW_HIDE;
    /* exInfo.hInstApp is output parameter */
    /* exInfo.lpIDList, exInfo.lpClass, exInfo.hkeyClass, exInfo.dwHotKey, exInfo.DUMMYUNIONNAME
     * are ignored for our fMask value.
     */
    /* exInfo.hProcess is output parameter */

    bSuccess = ShellExecuteExW(&exInfo);

    if (FALSE == bSuccess)
    {
        fprintf(stderr, "Failed to create worker process!\n");
        return INVALID_HANDLE_VALUE;
    }

    return exInfo.hProcess;
}

/**
 *  Creates intermediate worker process that creates elevated worker process
 *  inside a job that will terminate all processes on close.
 *
 *  On success it modifies data->worker_process_thread handle.
 *
 *  \param[inout] data thread_data containing capture configuration.
 *  \param[in] appPath path to elevated worker module
 *  \param[in] cmdLine commandline to start elevated worker with
 *
 *  \return Handle to created process.
 */
static HANDLE create_breakaway_worker_in_job(struct thread_data *data, PWSTR appPath, PWSTR appCmdLine)
{
    HANDLE process = INVALID_HANDLE_VALUE;
    STARTUPINFOW startupInfo;
    PROCESS_INFORMATION processInfo;
    PWSTR processCmdLine;
    int nChars;

    if (data->job_handle == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "create_breakaway_worker_in_job() cannot be called if data->job_handle is INVALID_HANDLE_VALUE!\n");
        return INVALID_HANDLE_VALUE;
    }

    memset(&startupInfo, 0, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    /* CreateProcessW works different to ShellExecuteExW.
     * It will always treat first token of command line as argv[0] in
     * created process.
     *
     * Hence create new string that will contain "appPath" appCmdLine.
     */
    nChars = wcslen(appPath) + wcslen(appCmdLine) +
             4 /* Two quotemarks, one space and NULL-terminator */;
    processCmdLine = (PWSTR)malloc(nChars * sizeof(WCHAR));
    if (processCmdLine == NULL)
    {
        fprintf(stderr, "Failed to allocate memory for processCmdLine!\n");
        return INVALID_HANDLE_VALUE;
    }

    swprintf_s(processCmdLine, nChars, L"\"%s\" %s",
               appPath, appCmdLine);

    /* We need to breakaway from parent job and assign to data->job_handle. */
    if (0 == CreateProcessW(NULL, processCmdLine, NULL, NULL, FALSE,
                            CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED,
                            NULL, NULL, &startupInfo, &processInfo))
    {
        data->process = FALSE;
    }
    else
    {
        process = processInfo.hProcess;
        /* processInfo.hThread needs to be closed. */
        data->worker_process_thread = processInfo.hThread;

        /* process is not assigned to any job. Assign it. */
        if (AssignProcessToJobObject(data->job_handle, process) == FALSE)
        {
            fprintf(stderr, "Failed to Assign process to job object - %d\n",
                    GetLastError());
            /* This is fatal error. */
            CloseHandle(process);
            CloseHandle(data->worker_process_thread);
            data->process = FALSE;
            process = INVALID_HANDLE_VALUE;
            data->worker_process_thread = INVALID_HANDLE_VALUE;
        }
        else
        {
            /* Process is assigned to proper job. Resume it. */
            ResumeThread(data->worker_process_thread);
        }
    }

    free(processCmdLine);

    return process;
}

int cmd_interactive(struct thread_data *data)
{
    int i = 0;
    int max_i;
    char buffer[INPUT_BUFFER_SIZE];
    BOOL finished;
    BOOL exit = FALSE;

    /* Detach from parent console window. Make sure to reopen stdout
     * and stderr as otherwise wide_print() does not corectly detect
     * console.
     */
    FreeConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    /* If we are running interactive then we should show console window.
     * We are not automatically allocated a console window because the
     * application type is set to windows. This prevents console
     * window from showing when adcusbCMD is used as extcap.
     * Since extcap is recommended cmd.exe users will notice a slight
     * inconvenience that adcusbCMD opens new window.
     *
     * Please note that is it impossible to get parent's cmd.exe stdin
     * handle if application type is not console. The difference is
     * that in case of console application cmd.exe waits until the
     * process finishes and in case of windows applications there is
     * no wait for process termination and the cmd.exe console immadietely
     * regains standard input functionality.
     */
    if (AllocConsole() == FALSE)
    {
        return -1;
    }

    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);

    data->filename = NULL;
    data->capture_all = TRUE;
    data->inject_descriptors = TRUE;

    filters_initialize();
    if (adcusbFilters[0] == NULL)
    {
        printf("No filter control devices are available.\n");

        if (is_adcusb_upper_filter_installed() == FALSE)
        {
            printf("Please reinstall adcusbDriver.\n");
            (void)getchar();
            filters_free();
            return -1;
        }

        printf("adcusb UpperFilter entry appears to be present.\n"
               "Most likely you have not restarted your computer after installation.\n"
               "It is possible to restart all USB devices to get adcusb working without reboot.\n"
               "\nWARNING:\n  Restarting all USB devices can result in data loss.\n"
               "  If you are unsure please answer 'n' and reboot in order to use adcusb.\n\n");

        finished = FALSE;
        do
        {
            printf("Do you want to restart all USB devices (y, n)? ");
            if (fgets(buffer, INPUT_BUFFER_SIZE, stdin) == NULL)
            {
                printf("Invalid input\n");
            }
            else
            {
                if (buffer[0] == 'y')
                {
                    finished = TRUE;
                    restart_all_usb_devices();
                    filters_free();
                    filters_initialize();
                }
                else if (buffer[0] == 'n')
                {
                    filters_free();
                    return -1;
                }
            }
        } while (finished == FALSE);
    }

    printf("Following filter control devices are available:\n");
    while (adcusbFilters[i] != NULL)
    {
        printf("%d %s\n", i+1, adcusbFilters[i]->device);
        enumerate_print_adcusb_interactive(adcusbFilters[i]->device);
        i++;
    }

    max_i = i;

    finished = FALSE;
    do
    {
        printf("Select filter to monitor (q to quit): ");
        if (fgets(buffer, INPUT_BUFFER_SIZE, stdin) == NULL)
        {
            printf("Invalid input\n");
        }
        else
        {
            if (buffer[0] == 'q')
            {
                finished = TRUE;
                exit = TRUE;
            }
            else
            {
                int value = atoi(buffer);

                if (value <= 0 || value > max_i)
                {
                    printf("Invalid input\n");
                }
                else
                {
                    data->device = _strdup(adcusbFilters[value-1]->device);
                    finished = TRUE;
                }
            }
        }
    } while (finished == FALSE);

    if (exit == TRUE)
    {
        filters_free();
        return -1;
    }

    finished = FALSE;
    do
    {
        printf("Output file name (.pcap): ");
        if (fgets(buffer, INPUT_BUFFER_SIZE, stdin) == NULL)
        {
            printf("Invalid input\n");
        }
        else if (buffer[0] == '\0')
        {
            printf("Empty filename not allowed\n");
        }
        else
        {
            for (i = 0; i < INPUT_BUFFER_SIZE; i++)
            {
                if (buffer[i] == '\n')
                {
                    buffer[i] = '\0';
                    break;
                }
            }
            data->filename = _strdup(buffer);
            finished = TRUE;
        }
    } while (finished == FALSE);

    filters_free();
    return 0;
}

/**
 * Wait for exit signal.
 *
 * Wait for either 'q' on standard input, data->exit_event or worker process termination.
 *
 * \param[in] data Thread data structure
 * \param[in] process Worker process handle
 *                    INVALID_HANDLE_VALUE if not using elevated worker.
 */
static void wait_for_exit_signal(struct thread_data *data, HANDLE process)
{
    HANDLE handle_table[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD dw;
    int count = 0;

    /* Verify that stdin_handle can be used. */
    if ((stdin_handle != NULL) && (stdin_handle != INVALID_HANDLE_VALUE))
    {
        dw = WaitForSingleObject(stdin_handle, 0);
        if (dw != WAIT_FAILED)
        {
            handle_table[count] = stdin_handle;
            count++;
        }
    }

    if ((data->exit_event != NULL) && (data->exit_event != INVALID_HANDLE_VALUE))
    {
        handle_table[count] = data->exit_event;
        count++;
    }

    if ((process != NULL) && (process != INVALID_HANDLE_VALUE))
    {
        handle_table[count] = process;
        count++;
    }

    if (count == 0)
    {
        fprintf(stderr, "Nothing to wait for in wait_for_exit_signal().\n");
    }

    /* Wait for exit condition. */
    while (data->process == TRUE)
    {
        dw = WaitForMultipleObjects(count, handle_table, FALSE, INFINITE);
#pragma warning(default : 4296)
        if ((dw >= WAIT_OBJECT_0) && dw < (WAIT_OBJECT_0 + count))
        {
            int i = dw - WAIT_OBJECT_0;
            if (handle_table[i] == stdin_handle)
            {
                /* There is something new on standard input. */
                INPUT_RECORD record;
                DWORD events_read;

                if (ReadConsoleInput(stdin_handle, &record, 1, &events_read))
                {
                    if (record.EventType == KEY_EVENT)
                    {
                        if ((record.Event.KeyEvent.bKeyDown == TRUE) &&
                            (record.Event.KeyEvent.uChar.AsciiChar == 'q'))
                        {
                            /* There is 'q' on standard input. Quit. */
                            break;
                        }
                    }
                }
            }
            else if (handle_table[i] == process)
            {
                /* Elevated worker process terminated. Quit. */
                break;
            }
            else if (handle_table[i] == data->exit_event)
            {
                /* Read thread has finished. Quit. */
                break;
            }
        }
        else if (dw == WAIT_FAILED)
        {
            fprintf(stderr, "WaitForMultipleObjects failed in wait_for_exit_signal(): %d\n", GetLastError());
            break;
        }
    }
}

static void start_capture(struct thread_data *data)
{
    HANDLE pipe_handle = INVALID_HANDLE_VALUE;
    HANDLE process = INVALID_HANDLE_VALUE;
    HANDLE thread = NULL;
    DWORD thread_id;

    /* Sanity check capture configuration. */
    if ((data->capture_all == FALSE) &&
        (data->capture_new == FALSE) &&
        (data->address_list == NULL))
    {
        fprintf(stderr, "Selected capture options result in empty capture.\n");
        fprintf(stderr, "Add command-line option -A to capture from all devices.\n");
        return;
    }

    if (FALSE == adcusbInitAddressFilter(&data->filter, data->address_list, data->capture_all))
    {
        fprintf(stderr, "adcusbInitAddressFilter failed!\n");
        return;
    }

    data->exit_event = CreateEvent(NULL, /* Handle cannot be inherited */
                                   TRUE, /* Manual Reset */
                                   FALSE, /* Default to not signalled */
                                   NULL);

    memset(&data->descriptors, 0, sizeof(data->descriptors));

    if (IsElevated() == TRUE)
    {
        data->read_handle = INVALID_HANDLE_VALUE;
        if (strncmp("-", data->filename, 2) == 0)
        {
            data->write_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        }
        else
        {
            data->write_handle = CreateFileA(data->filename,
                                             GENERIC_WRITE,
                                             0,
                                             NULL,
                                             CREATE_NEW,
                                             FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,
                                             NULL);
        }

        if (data->inject_descriptors)
        {
            data->descriptors.descriptors = descriptors_generate_pcap(data->device, &data->descriptors.descriptors_len,
                                                                      &data->filter);
            data->descriptors.buf_written = 0;
        }

        data->read_handle = create_filter_read_handle(data);

        thread = CreateThread(NULL, /* default security attributes */
                              0,    /* use default stack size */
                              read_thread,
                              data,
                              0,    /* use default creation flag */
                              &thread_id);

        if (thread == NULL)
        {
            fprintf(stderr, "Failed to create thread\n");
            data->process = FALSE;
        }
    }
    else
    {
        PWSTR appPath = NULL;
        PWSTR appCmdLine = NULL;

        BOOL in_job = FALSE;

        if (FALSE == generate_worker_command_line(data, &appPath, &appCmdLine, &pipe_handle))
        {
            fprintf(stderr, "Failed to generate command line\n");
            data->process = FALSE;
        }
        else
        {
            /* Default state is adcusbCMD running outside any job and hence
             * we need to create new job to take care of worker processes.
             */
            BOOL needs_breakaway = FALSE;
            BOOL needs_new_job = TRUE;

            /* We are not elevated. Check if we are running inside a job. */
            IsProcessInJob(GetCurrentProcess(), NULL, &in_job);

            if (in_job)
            {
                /* We are running inside a job. This can be Visual Studio debug session
                 * job or Windows 8.1 Wireshark job or adcusb job or anything else.
                 *
                 * If the job has JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, then assume
                 * that whoever create the job will get care of any dangling processes.
                 *
                 * If the job has JOB_OBJECT_LIMIT_BREAKAWAY_OK (which is the case for
                 * Visual Studio and Windows 8.1 jobs) then we need to create intermediate
                 * worker to launch elevated worker. The intermediate worker needs to
                 * break from parent job.
                 *
                 * If the job has JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK we could omit
                 * the intermediate worker, but keep it there so there's no race condion
                 * (if parent gets terminated after executing elevated worker but before
                 * the elevated worker is assigned to a job, then the elevated worker
                 * will need to be manually terminated). If we are not running inside
                 * a job this race condition is not a problem because we first assign
                 * our process to a job (and hence all newly created processes are
                 * automatically assigned to that job).
                 *
                 *
                 * All this is because ShellExecuteEx() does not support
                 * CREATE_BREAKAWAY_FROM_JOB nor CREATE_SUSPENDED flags.
                 * CreateProcess() supports CREATE_BREAKAWAY_FROM_JOB and CREATE_SUSPENDED
                 * flag but do not support "runas" option. adcusbCMD manifest does not
                 * require administrator access because that would result in UAC screen
                 * every time Wireshark gets extcap interface options.
                 */

                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

                memset(&info, 0, sizeof(info));
                if (0 == QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation,
                                                   &info, sizeof(info), NULL))
                {
                    fprintf(stderr, "Failed to query job information - %d\n", GetLastError());
                    /* This is fatal error. */
                    exit(-1);
                }

                if (info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
                {
                    /* There is no need to breakaway nor to create new job. */
                    needs_breakaway = FALSE;
                    needs_new_job = FALSE;
                }
                else if (info.BasicLimitInformation.LimitFlags &
                         (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK))
                {
                   needs_breakaway = TRUE;
                   needs_new_job = TRUE;
                }
                else
                {
                    fprintf(stderr, "Unhandled job limit flags 0x%08X\n", info.BasicLimitInformation.LimitFlags);
                    /* This is not fatal. We cannot perform job breakaway though! */
                    needs_breakaway = FALSE;
                    needs_new_job = FALSE;
                }
            }

            if (needs_new_job)
            {
                if (data->job_handle == INVALID_HANDLE_VALUE)
                {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

                    data->job_handle = CreateJobObject(NULL, NULL);
                    if (data->job_handle == NULL)
                    {
                        fprintf(stderr, "Failed to create job object!\n");
                        data->process = FALSE;
                        data->job_handle = INVALID_HANDLE_VALUE;
                        return;
                    }

                    memset(&info, 0, sizeof(info));
                    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    SetInformationJobObject(data->job_handle, JobObjectExtendedLimitInformation, &info, sizeof(info));
                }

                /* If breakaway is not needed for worker process, then assign ourselves to newly created job.
                 * This will result in automatic worker process assignment to newly created job.
                 */
                if (needs_breakaway == FALSE)
                {
                    if (AssignProcessToJobObject(data->job_handle, GetCurrentProcess()) == FALSE)
                    {
                        fprintf(stderr, "Failed to Assign process to job object - %d\n",
                                GetLastError());
                        /* This is fatal error. */
                        exit(-1);
                    }
                }
            }

            if (needs_breakaway == FALSE)
            {
                /* Create elevated worker process. It will automatically be assigned to proper job. */
                process = create_elevated_worker(appPath, appCmdLine);
            }
            else
            {
                process = create_breakaway_worker_in_job(data, appPath, appCmdLine);
            }

            /* Free worker path and command line strings as these are no longer needed. */
            free(appPath);
            free(appCmdLine);
            appPath = NULL;
            appCmdLine = NULL;

            if (process != INVALID_HANDLE_VALUE)
            {
                if (strncmp("-", data->filename, 2) == 0)
                {
                    data->write_handle = GetStdHandle(STD_OUTPUT_HANDLE);
                    data->read_handle = pipe_handle;

                    thread = CreateThread(NULL, /* default security attributes */
                                          0,    /* use default stack size */
                                          read_thread,
                                          data,
                                          0,    /* use default creation flag */
                                          &thread_id);
                }
                else
                {
                    /* Worker process saves directly to file */
                    data->write_handle = INVALID_HANDLE_VALUE;
                    data->read_handle = INVALID_HANDLE_VALUE;
                }
            }
            else
            {
                /* Worker couldn't be started. */
                data->process = FALSE;
                if (pipe_handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(pipe_handle);
                    pipe_handle = INVALID_HANDLE_VALUE;
                }
            }
        }
    }

    wait_for_exit_signal(data, process);
    data->process = FALSE;
    if (data->exit_event != INVALID_HANDLE_VALUE)
    {
        SetEvent(data->exit_event);
    }

    /* If we created worker thread, wait for it to terminate. */
    if (thread != NULL)
    {
        WaitForSingleObject(thread, INFINITE);
    }

    /* Closing read and write handles will terminate worker process. */

    if ((data->read_handle == INVALID_HANDLE_VALUE) &&
        (data->write_handle == INVALID_HANDLE_VALUE))
    {
        /* We should kill worker process if we created it.
         * We have no other way to let process know that it needs to quit.
         */
        if (process != INVALID_HANDLE_VALUE)
        {
            TerminateProcess(process, 0);
        }
    }

    if (data->read_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(data->read_handle);
    }

    if (data->write_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(data->write_handle);
    }

    /* If we created worker process, wait for it to terminate. */
    if (process != INVALID_HANDLE_VALUE)
    {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
    }

    if (data->descriptors.descriptors)
    {
        descriptors_free_pcap(data->descriptors.descriptors);
    }
}

static void print_extcap_version(void)
{
    printf("extcap {version=" ADCUSBCMD_VERSION_STR "}{help=http://desowin.org/adcusb/}\n");
}

static void print_extcap_interfaces(void)
{
    int i = 0;
    filters_initialize();

    while (adcusbFilters[i] != NULL)
    {
        char *tmp = strrchr(adcusbFilters[i]->device, '\\');
        if (tmp == NULL)
        {
            tmp = adcusbFilters[i]->device;
        }
        else
        {
            tmp++;
        }

        printf("interface {value=%s}{display=%s}\n",
               adcusbFilters[i]->device, tmp);
        i++;
    }

    filters_free();
}

static void print_extcap_dlts(void)
{
    printf("dlt {number=249}{name=ADCUSB}{display=adcusb}\n");
}

static int print_extcap_options(const char *device)
{
    if (device == NULL)
    {
        return -1;
    }

    printf("arg {number=0}{call=--snaplen}"
           "{display=Snapshot length}{tooltip=Snapshot length}"
           "{type=unsigned}{default=%d}\n", DEFAULT_SNAPSHOT_LENGTH);
    printf("arg {number=1}{call=--bufferlen}"
           "{display=Capture buffer length}"
           "{tooltip=adcusb kernel-mode capture buffer length in bytes}"
           "{type=integer}{range=0,134217728}{default=%d}\n",
           DEFAULT_INTERNAL_KERNEL_BUFFER_SIZE);
    printf("arg {number=2}{call=--capture-from-all-devices}"
           "{display=Capture from all devices connected}"
           "{tooltip=Capture from all devices connected despite other options}"
           "{type=boolflag}{default=true}\n");
    printf("arg {number=3}{call=--capture-from-new-devices}"
           "{display=Capture from newly connected devices}"
           "{tooltip=Automatically start capture on all newly connected devices}"
           "{type=boolflag}{default=true}\n");
    printf("arg {number=4}{call=--inject-descriptors}"
           "{display=Inject already connected devices descriptors into capture data}"
           "{type=boolflag}{default=true}\n");
    printf("arg {number=%d}{call=--devices}{display=Attached USB Devices}{tooltip=Select individual devices to capture from}{type=multicheck}\n",
           EXTCAP_ARGNUM_MULTICHECK);

    enumerate_print_extcap_config(device);

    return 0;
}

static int run_as_extcap = 0;
static int do_extcap_version = 0;
static int do_extcap_interfaces = 0;
static int do_extcap_dlts = 0;
static int do_extcap_config = 0;
static int do_extcap_capture = 0;
static const char *wireshark_version = NULL;
static const char *extcap_interface = NULL;
static const char *extcap_fifo = NULL;

int cmd_extcap(struct thread_data *data)
{
    int ret = -1;

    if (do_extcap_version)
    {
        print_extcap_version();
        ret = 0;
    }

    if (do_extcap_interfaces)
    {
        print_extcap_interfaces();
        ret = 0;
    }

    if (do_extcap_dlts)
    {
        print_extcap_dlts();
        ret = 0;
    }

    if (do_extcap_config)
    {
        ret = print_extcap_options(extcap_interface);
    }

    /* --capture */
    if (do_extcap_capture)
    {
        if ((extcap_fifo == NULL) || (extcap_interface == NULL))
        {
            /* No fifo nor interface to capture from. */
            return -1;
        }

        if (data->device != NULL)
        {
            free(data->device);
        }
        data->device = _strdup(extcap_interface);
        if (data->filename != NULL)
        {
            free(data->filename);
        }
        data->filename = _strdup(extcap_fifo);
        data->process = TRUE;

        data->read_handle = INVALID_HANDLE_VALUE;
        data->write_handle = INVALID_HANDLE_VALUE;

        start_capture(data);
        return 0;
    }

    return ret;
}

BOOLEAN IsHandleRedirected(DWORD handle)
{
    HANDLE h = GetStdHandle(handle);
    if (h)
    {
        BY_HANDLE_FILE_INFORMATION fi;
        if (GetFileInformationByHandle(h, &fi))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void attach_parent_console()
{
    HANDLE inHandle, outHandle, errHandle;
    BOOL outRedirected, errRedirected;

    inHandle = GetStdHandle(STD_INPUT_HANDLE);
    outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    errHandle = GetStdHandle(STD_ERROR_HANDLE);

    outRedirected = IsHandleRedirected(STD_OUTPUT_HANDLE);
    errRedirected = IsHandleRedirected(STD_ERROR_HANDLE);

    if (outRedirected && errRedirected)
    {
        /* Both standard output and error handles are redirected.
         * There is no point in attaching to parent process console.
         */
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
    {
        /* Console attach failed. */
        return;
    }

    if (inHandle != GetStdHandle(STD_INPUT_HANDLE))
    {
        /* Restore input handle. */
        SetStdHandle(STD_INPUT_HANDLE, inHandle);
    }

    /* Console attach succeded */
    if (outRedirected == FALSE)
    {
        freopen("CONOUT$", "w", stdout);
    }
    else if (GetStdHandle(STD_OUTPUT_HANDLE) != outHandle)
    {
        /* Attach Console changed STD_OUTPUT_HANDLE even though it is redirected.
         * Restore the redirected handle.
         */
        SetStdHandle(STD_OUTPUT_HANDLE, outHandle);
    }

    if (errRedirected == FALSE)
    {
        freopen("CONOUT$", "w", stderr);
    }
    else if (GetStdHandle(STD_ERROR_HANDLE) != errHandle)
    {
        /* Attach Console changed STD_ERROR_HANDLE even though it is redirected.
         * Restore the redirected handle.
         */
        SetStdHandle(STD_ERROR_HANDLE, errHandle);
    }
}

static HANDLE open_first_control_device(void)
{
	int i;

	if (adcusbFilters == NULL || adcusbFilters[0] == NULL)
	{
		adcusbLogError("open_first_control_device: no filters available", 0);
		return INVALID_HANDLE_VALUE;
	}

	for (i = 0; adcusbFilters[i] != NULL; i++)
	{
		HANDLE hDev;

		hDev = CreateFileA(adcusbFilters[i]->device,
		                   GENERIC_READ | GENERIC_WRITE,
		                   0,
		                   NULL,
		                   OPEN_EXISTING,
		                   0,
		                   NULL);
		if (hDev != INVALID_HANDLE_VALUE)
		{
			return hDev;
		}
	}

	adcusbLogError("open_first_control_device failed", GetLastError());
	return INVALID_HANDLE_VALUE;
}

static int handle_cmd_start(void)
{
	HANDLE hDev;
	BOOL ok;

	filters_initialize();
	hDev = open_first_control_device();
	if (hDev == INVALID_HANDLE_VALUE)
	{
		filters_free();
		return -1;
	}

	ok = adcusbSetProtection(hDev, 1);
	if (ok)
	{
		adcusbLogInfo("Protection started successfully");
	}
	else
	{
		adcusbLogError("Failed to start protection", GetLastError());
	}

	CloseHandle(hDev);
	filters_free();
	return ok ? 0 : -1;
}

static int handle_cmd_stop(void)
{
	HANDLE hDev;
	BOOL ok;

	filters_initialize();
	hDev = open_first_control_device();
	if (hDev == INVALID_HANDLE_VALUE)
	{
		filters_free();
		return -1;
	}

	ok = adcusbSetProtection(hDev, 0);
	if (ok)
	{
		adcusbLogInfo("Protection stopped successfully");
	}
	else
	{
		adcusbLogError("Failed to stop protection", GetLastError());
	}

	CloseHandle(hDev);
	filters_free();
	return ok ? 0 : -1;
}

static int handle_cmd_add(const char *szHardwareID)
{
	WCHAR wPattern[MAX_PATH];
	HANDLE hDev;
	BOOL ok;

	if (szHardwareID == NULL || szHardwareID[0] == '\0')
	{
		adcusbLogError("handle_cmd_add: HardwareID is empty", 0);
		return -1;
	}

	filters_initialize();
	hDev = open_first_control_device();
	if (hDev == INVALID_HANDLE_VALUE)
	{
		filters_free();
		return -1;
	}

	MultiByteToWideChar(CP_ACP, 0, szHardwareID, -1, wPattern, MAX_PATH);
	wPattern[MAX_PATH - 1] = L'\0';

	ok = adcusbAddBlockRule(hDev, wPattern);
	if (ok)
	{
		adcusbLogInfo("Block rule added: %s", szHardwareID);
	}
	else
	{
		adcusbLogError("Failed to add block rule", GetLastError());
	}

	CloseHandle(hDev);
	filters_free();
	return ok ? 0 : -1;
}

static int handle_cmd_init(void)
{
	BOOL ok;

	ok = adcusbInstallUpperFilter();
	if (ok)
	{
		adcusbLogInfo("UpperFilter installed successfully");
	}
	else
	{
		adcusbLogError("Failed to install UpperFilter", GetLastError());
	}

	return ok ? 0 : -1;
}

static int handle_cmd_uninit(void)
{
	BOOL ok;

	ok = adcusbUninstallUpperFilter();
	if (ok)
	{
		adcusbLogInfo("UpperFilter uninstalled successfully");
	}
	else
	{
		adcusbLogError("Failed to uninstall UpperFilter", GetLastError());
	}

	return ok ? 0 : -1;
}

static void log_service_error(const char *szAction, DWORD ulError)
{
	if (ulError == ERROR_ACCESS_DENIED)
	{
		adcusbLogError("Access denied. Please run as Administrator", ulError);
	}
	else if (ulError == ERROR_SERVICE_EXISTS)
	{
		adcusbLogInfo("Service already exists");
	}
	else if (ulError == ERROR_SERVICE_ALREADY_RUNNING)
	{
		adcusbLogInfo("Service is already running");
	}
	else if (ulError == ERROR_SERVICE_DOES_NOT_EXIST)
	{
		adcusbLogError("Service does not exist", ulError);
	}
	else if (ulError == ERROR_SERVICE_MARKED_FOR_DELETE)
	{
		adcusbLogError("Service is marked for deletion", ulError);
	}
	else if (ulError == ERROR_PATH_NOT_FOUND)
	{
		adcusbLogError("Driver path not found", ulError);
	}
	else
	{
		adcusbLogError(szAction, ulError);
	}
}

static int handle_cmd_load(void)
{
	SC_HANDLE hSCM;
	SC_HANDLE hService;
	char szPath[MAX_PATH];
	UINT ulSystemDirLen;
	int nPathAppendRet;
	DWORD ulError;
	BOOL bOk;
	int nRet;

	hSCM = NULL;
	hService = NULL;
	ulSystemDirLen = 0;
	nPathAppendRet = 0;
	ulError = ERROR_SUCCESS;
	bOk = FALSE;
	nRet = -1;

	ulSystemDirLen = GetSystemDirectoryA(szPath, MAX_PATH);
	if (ulSystemDirLen == 0 || ulSystemDirLen >= MAX_PATH)
	{
		ulError = GetLastError();
		if (ulError == ERROR_SUCCESS)
		{
			ulError = ERROR_PATH_NOT_FOUND;
		}
		log_service_error("Failed to get system directory", ulError);
		goto cleanup;
	}

	nPathAppendRet = _snprintf(szPath + ulSystemDirLen,
					  MAX_PATH - ulSystemDirLen,
					  "\\drivers\\adcusb.sys");
	szPath[MAX_PATH - 1] = '\0';
	if (nPathAppendRet < 0)
	{
		log_service_error("Driver path not found", ERROR_PATH_NOT_FOUND);
		goto cleanup;
	}

	hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		ulError = GetLastError();
		log_service_error("OpenSCManager failed", ulError);
		goto cleanup;
	}

	hService = CreateServiceA(hSCM,
				 "adcusb",
				 "adcusb",
				 SERVICE_ALL_ACCESS,
				 SERVICE_KERNEL_DRIVER,
				 SERVICE_DEMAND_START,
				 SERVICE_ERROR_NORMAL,
				 szPath,
				 NULL,
				 NULL,
				 NULL,
				 NULL,
				 NULL);
	if (hService == NULL)
	{
		ulError = GetLastError();
		if (ulError == ERROR_SERVICE_EXISTS)
		{
			log_service_error("CreateService reported existing service", ulError);
			hService = OpenServiceA(hSCM, "adcusb", SERVICE_ALL_ACCESS);
			if (hService == NULL)
			{
				ulError = GetLastError();
				log_service_error("OpenService failed", ulError);
				goto cleanup;
			}
		}
		else
		{
			log_service_error("CreateService failed", ulError);
			goto cleanup;
		}
	}

	bOk = StartService(hService, 0, NULL);
	if (!bOk)
	{
		ulError = GetLastError();
		if (ulError == ERROR_SERVICE_ALREADY_RUNNING)
		{
			log_service_error("StartService reported already running", ulError);
		}
		else
		{
			log_service_error("StartService failed", ulError);
			goto cleanup;
		}
	}
	else
	{
		adcusbLogInfo("Service started successfully");
	}

	bOk = adcusbInstallUpperFilter();
	if (!bOk)
	{
		adcusbLogError("Failed to install UpperFilter", GetLastError());
		goto cleanup;
	}

	adcusbLogInfo("UpperFilter installed successfully");
	nRet = 0;

cleanup:
	if (hService != NULL)
	{
		CloseServiceHandle(hService);
		hService = NULL;
	}
	if (hSCM != NULL)
	{
		CloseServiceHandle(hSCM);
		hSCM = NULL;
	}

	return nRet;
}

static int handle_cmd_unload(void)
{
	SC_HANDLE hSCM;
	SC_HANDLE hService;
	SERVICE_STATUS svcStatus;
	DWORD ulError;
	BOOL bFilterOk;
	BOOL bStopOk;
	BOOL bDeleteOk;
	int nRet;

	hSCM = NULL;
	hService = NULL;
	ZeroMemory(&svcStatus, sizeof(svcStatus));
	ulError = ERROR_SUCCESS;
	bFilterOk = FALSE;
	bStopOk = FALSE;
	bDeleteOk = FALSE;
	nRet = -1;

	bFilterOk = adcusbUninstallUpperFilter();
	if (bFilterOk)
	{
		adcusbLogInfo("UpperFilter uninstalled successfully");
	}
	else
	{
		adcusbLogError("Failed to uninstall UpperFilter", GetLastError());
	}

	hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		ulError = GetLastError();
		log_service_error("OpenSCManager failed", ulError);
		goto cleanup;
	}

	hService = OpenServiceA(hSCM, "adcusb", SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
	if (hService == NULL)
	{
		ulError = GetLastError();
		log_service_error("OpenService failed", ulError);
		goto cleanup;
	}

	bStopOk = ControlService(hService, SERVICE_CONTROL_STOP, &svcStatus);
	if (!bStopOk)
	{
		ulError = GetLastError();
		if (ulError == ERROR_SERVICE_NOT_ACTIVE)
		{
			adcusbLogInfo("Service is already stopped");
		}
		else
		{
			log_service_error("ControlService failed", ulError);
			goto cleanup;
		}
	}
	else
	{
		adcusbLogInfo("Service stopped successfully");
	}

	bDeleteOk = DeleteService(hService);
	if (!bDeleteOk)
	{
		ulError = GetLastError();
		if (ulError == ERROR_SERVICE_MARKED_FOR_DELETE)
		{
			log_service_error("DeleteService reported marked for delete", ulError);
		}
		else
		{
			log_service_error("DeleteService failed", ulError);
			goto cleanup;
		}
	}
	else
	{
		adcusbLogInfo("Service deleted successfully");
	}

	if (bFilterOk)
	{
		nRet = 0;
	}

cleanup:
	if (hService != NULL)
	{
		CloseServiceHandle(hService);
		hService = NULL;
	}
	if (hSCM != NULL)
	{
		CloseServiceHandle(hSCM);
		hSCM = NULL;
	}

	return nRet;
}

static void print_help(void)
{
    printf("Usage: adusbcli.exe [options]\n"
	   "  -h, -?, -help\n"
	   "    Prints this help.\n"
	   "  Example: adusbcli.exe -h\n"
	   "  -d <device>, -device <device>\n"
	   "    adcusb control device to open. Example: -d \\\\.\\adcusb1.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1\n"
	   "  -o <file>, -output <file>\n"
	   "    Output .pcap file name.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -o capture.pcap\n"
	   "  -s <len>, -snaplen <len>\n"
	   "    Sets snapshot length.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -s 65535\n"
	   "  -b <len>, -bufferlen <len>\n"
	   "    Sets internal capture buffer length. Valid range <4096,134217728>.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -b 1048576\n"
	   "  -A, -capture-from-all-devices\n"
	   "    Captures data from all devices connected to selected Root Hub.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -A\n"
	   "  -devices <list>\n"
	   "    Captures data only from devices with addresses present in list.\n"
	   "    List is comma separated list of values. Example -devices 1,2,3.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -devices 1,2,3\n"
	   "  -inject-descriptors\n"
	   "    Inject already connected devices descriptors into capture data.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -inject-descriptors\n"
	   "  -I,  -init-non-standard-hwids\n"
	   "    Initializes NonStandardHWIDs registry key used by adcusbDriver.\n"
	   "    This registry key is needed for USB 3.0 capture.\n"
	   "  Example: adusbcli.exe -I\n"
	   "  -block-hwid <pattern>\n"
	   "    Add HardwareID substring pattern to vmusb block list.\n"
	   "    Example: -block-hwid Vid_0E0F&Pid_0001\n"
	   "    Requires -d <device>. Max 16 rules, each up to 127 chars.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -block-hwid Vid_0E0F&Pid_0001\n"
	   "  -allow-hwid <pattern>\n"
	   "    Remove HardwareID substring pattern from vmusb block list.\n"
	   "  Example: adusbcli.exe -d \\\\.\\adcusb1 -allow-hwid Vid_0E0F&Pid_0001\n"
	   "    Requires -d <device>.\n"
	   "  -init\n"
	   "    Initialize adcusb driver (register as vmusb UpperFilter).\n"
	   "  Example: adusbcli.exe -init\n"
	   "  -uninit\n"
	   "    Uninitialize adcusb driver (remove from vmusb UpperFilter).\n"
	   "  Example: adusbcli.exe -uninit\n"
	   "  -start\n"
	   "    Start USB device protection.\n"
	   "  Example: adusbcli.exe -start\n"
	   "  -stop\n"
	   "    Stop USB device protection.\n"
	   "  Example: adusbcli.exe -stop\n"
	   "  -add <HardwareID>\n"
	   "    Add HardwareID to vmusb block list (requires driver loaded).\n"
	   "  Example: adusbcli.exe -add \"USB\\\\VID_1234&PID_5678\"\n"
	   "  -load\n"
	   "    Load adcusb driver service and install UpperFilter.\n"
	   "  Example: adusbcli.exe -load\n"
	   "  -unload\n"
	   "    Unload adcusb driver service and uninstall UpperFilter.\n"
	   "  Example: adusbcli.exe -unload\n");
}

/* Commandline arguments without short option */
#define ARG_DEVICES                    900
#define ARG_CAPTURE_FROM_NEW_DEVICES   901
#define ARG_INJECT_DESCRIPTORS         902
#define ARG_BLOCK_HWID                 910
#define ARG_ALLOW_HWID                 911
#define ARG_LIST_BLOCK_RULES           912
#define ARG_EXTCAP_VERSION            1000
#define ARG_EXTCAP_INTERFACES         1001
#define ARG_EXTCAP_INTERFACE          1002
#define ARG_EXTCAP_DLTS               1003
#define ARG_EXTCAP_CONFIG             1004
#define ARG_EXTCAP_CAPTURE            1005
#define ARG_EXTCAP_FIFO               1006

/* CLI command arguments (no short option) */
#define ARG_CMD_INIT                   256
#define ARG_CMD_UNINIT                 257
#define ARG_CMD_START                  258
#define ARG_CMD_STOP                   259
#define ARG_CMD_ADD                    260
#define ARG_CMD_LOAD                   261
#define ARG_CMD_UNLOAD                 262

#if _MSC_VER >= 1700
int __cdecl adcusbcmd_main(int argc, CHAR **argv)
#else
int __cdecl main(int argc, CHAR **argv)
#endif
{
    int ret = -1;
    struct thread_data data;
    static struct option long_options[] =
    {
        {"help", no_argument, 0, 'h'},
        {"device", required_argument, 0, 'd'},
        {"output", required_argument, 0, 'o'},
        {"snaplen", required_argument, 0, 's'},
        {"bufferlen", required_argument, 0, 'b'},
        {"init-non-standard-hwids", no_argument, 0, 'I'},
        /* Capture options. */
        {"devices", required_argument, 0, ARG_DEVICES},
        {"capture-from-all-devices", no_argument, 0, 'A'},
        {"capture-from-new-devices", no_argument, 0, ARG_CAPTURE_FROM_NEW_DEVICES},
        {"inject-descriptors", no_argument, 0, ARG_INJECT_DESCRIPTORS},
        {"block-hwid", required_argument, 0, ARG_BLOCK_HWID},
        {"allow-hwid", required_argument, 0, ARG_ALLOW_HWID},
        {"list-block-rules", no_argument, 0, ARG_LIST_BLOCK_RULES},
        /* Extcap interface. Please note that there are no short
         * options for these and the numbers are just gopt keys.
         */
        {"extcap-version", optional_argument, 0, ARG_EXTCAP_VERSION},
        {"extcap-interfaces", no_argument, &do_extcap_interfaces, ARG_EXTCAP_INTERFACES},
        {"extcap-interface", required_argument, 0, ARG_EXTCAP_INTERFACE},
        {"extcap-dlts", no_argument, &do_extcap_dlts, ARG_EXTCAP_DLTS},
        {"extcap-config", no_argument, &do_extcap_config, ARG_EXTCAP_CONFIG},
        {"capture", no_argument, &do_extcap_capture, ARG_EXTCAP_CAPTURE},
        {"fifo", required_argument, 0, ARG_EXTCAP_FIFO},
        {"init",   no_argument,       0, ARG_CMD_INIT},
        {"uninit", no_argument,       0, ARG_CMD_UNINIT},
        {"start",  no_argument,       0, ARG_CMD_START},
        {"stop",   no_argument,       0, ARG_CMD_STOP},
        {"add",    required_argument, 0, ARG_CMD_ADD},
        {"load",   no_argument,       0, ARG_CMD_LOAD},
        {"unload", no_argument,       0, ARG_CMD_UNLOAD},
        {0, 0, 0, 0}
    };
    int option_index = 0;
    int c;
    const char *block_hwid_str = NULL;
    const char *allow_hwid_str = NULL;
    int        list_block_rules_flag = 0;

    attach_parent_console();

    adcusbLogInit();

    data.filename = NULL;
    data.device = NULL;
    data.address_list = NULL;
    data.capture_all = FALSE;
    data.capture_new = FALSE;
    data.inject_descriptors = FALSE;
    data.snaplen = DEFAULT_SNAPSHOT_LENGTH;
    data.bufferlen = DEFAULT_INTERNAL_KERNEL_BUFFER_SIZE;
    data.job_handle = INVALID_HANDLE_VALUE;
    data.worker_process_thread = INVALID_HANDLE_VALUE;
    data.read_handle = INVALID_HANDLE_VALUE;
    data.write_handle = INVALID_HANDLE_VALUE;
    data.exit_event = INVALID_HANDLE_VALUE;

    if (argc == 1)
    {
        print_help();
        adcusbLogClose();
        return 0;
    }

    while (-1 != (c = getopt_long_only(argc, argv, "hd:o:s:b:IA", long_options, &option_index)))
    {
        switch (c)
        {
            case 0:
                /* getopt_long has set the flag. */
                break;
            case 'h': /* --help */
                print_help();
                adcusbLogClose();
                return 0;
            case 'd': /* --device */
#pragma warning(push)
#pragma warning(disable:28193)
                data.device = _strdup(optarg);
#pragma warning(pop)
                break;
            case 'o': /* --output */
#pragma warning(push)
#pragma warning(disable:28193)
                data.filename = _strdup(optarg);
#pragma warning(pop)
                break;
            case 's': /* --snaplen */
                data.snaplen = atol(optarg);
                if (data.snaplen == 0)
                {
                    fprintf(stderr, "Invalid snapshot length!\n");
                    adcusbLogClose();
                    return -1;
                }
                break;
            case 'b': /* --bufferlen */
                data.bufferlen = atol(optarg);
                /* Minimum buffer size if 4 KiB, maximum 128 MiB */
                if (data.bufferlen < 4096 || data.bufferlen > 134217728)
                {
                    fprintf(stderr, "Invalid buffer length! "
                                    "Valid range <4096,134217728>.\n");
                    adcusbLogClose();
                    return -1;
                }
                break;
            case 'I': /* --init-non-standard-hwids */
                init_non_standard_roothub_hwid();
                adcusbLogClose();
                return 0;
            case ARG_DEVICES:
                data.address_list = optarg;
                break;
            case 'A': /* --capture-from-all-devices */
                data.capture_all = TRUE;
                break;
            case ARG_CAPTURE_FROM_NEW_DEVICES:
                data.capture_new = TRUE;
                break;
            case ARG_INJECT_DESCRIPTORS:
                data.inject_descriptors = TRUE;
                break;
            case ARG_BLOCK_HWID:
                block_hwid_str = optarg;
                break;
            case ARG_ALLOW_HWID:
                allow_hwid_str = optarg;
                break;
            case ARG_LIST_BLOCK_RULES:
                list_block_rules_flag = 1;
                break;
            case ARG_EXTCAP_VERSION:
                do_extcap_version = 1;
                wireshark_version = optarg;
                break;
            case ARG_EXTCAP_INTERFACE:
                extcap_interface = optarg;
                break;
            case ARG_EXTCAP_FIFO:
                run_as_extcap = 1;
                extcap_fifo = optarg;
                break;

            case ARG_CMD_INIT:
            {
                int nRet = handle_cmd_init();
                adcusbLogClose();
                return nRet;
            }
            case ARG_CMD_UNINIT:
            {
                int nRet = handle_cmd_uninit();
                adcusbLogClose();
                return nRet;
            }
            case ARG_CMD_START:
            {
                int nRet = handle_cmd_start();
                adcusbLogClose();
                return nRet;
            }
            case ARG_CMD_STOP:
            {
                int nRet = handle_cmd_stop();
                adcusbLogClose();
                return nRet;
            }
            case ARG_CMD_ADD:
            {
                int nRet = handle_cmd_add(optarg);
                adcusbLogClose();
                return nRet;
            }
            case ARG_CMD_LOAD:
            {
                int nRet = handle_cmd_load();
                adcusbLogClose();
                return nRet;
            }
            case ARG_CMD_UNLOAD:
            {
                int nRet = handle_cmd_unload();
                adcusbLogClose();
                return nRet;
            }

            case ':':
            case '?':
                fprintf(stderr, "Try 'adusbcli.exe -help' for more information.\n");
                adcusbLogClose();
                return -1;

            default:
                printf("getopt_long() returned character code 0x%X. Please report.\n", c);
                adcusbLogClose();
                return -1;
        }
    }

    if (data.snaplen > (data.bufferlen - sizeof(pcaprec_hdr_t)))
    {
        fprintf(stderr, "Packets larger than %u bytes won't be captured due to too small buffer.\n",
                data.bufferlen - sizeof(pcaprec_hdr_t));
    }

    /* HardwareID /    */
    if (block_hwid_str != NULL || allow_hwid_str != NULL)
    {
        WCHAR wPattern[ADCUSB_BLOCK_RULE_PATTERN_LEN];
        const char *pattern_str;
        HANDLE hDev;
        BOOL ok;

        if (data.device == NULL)
        {
            fprintf(stderr, "Device not specified. Use -d <device>.\n");
            adcusbLogClose();
            return -1;
        }

        pattern_str = (block_hwid_str != NULL) ? block_hwid_str : allow_hwid_str;
        MultiByteToWideChar(CP_ACP, 0, pattern_str, -1, wPattern, ADCUSB_BLOCK_RULE_PATTERN_LEN);
        wPattern[ADCUSB_BLOCK_RULE_PATTERN_LEN - 1] = L'\0';

        hDev = CreateFileA(data.device,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
		if (hDev == INVALID_HANDLE_VALUE)
		{
			DWORD dwErr;
			int i;

			dwErr = GetLastError();
			fprintf(stderr, "Cannot open device %s (error %lu)\n", data.device, dwErr);
			switch (dwErr)
			{
			case ERROR_FILE_NOT_FOUND:
				fprintf(stderr, "  Device '%s' does not exist.\n", data.device);
				fprintf(stderr, "  Check driver status: sc query adcusb\n");
				fprintf(stderr, "  If driver is running, check available devices:\n");
				filters_initialize();
				if (adcusbFilters[0] != NULL)
				{
					fprintf(stderr, "  Available devices:\n");
					for (i = 0; adcusbFilters[i] != NULL; i++)
						fprintf(stderr, "    %s\n", adcusbFilters[i]->device);
				}
				else
				{
					fprintf(stderr, "  No adcusb devices found. Try:\n");
					fprintf(stderr, "    1. Verify driver: sc query adcusb\n");
					fprintf(stderr, "    2. Reboot after installing UpperFilter\n");
				}
				filters_free();
				break;
			case ERROR_ACCESS_DENIED:
				fprintf(stderr, "  Access denied. Run this command as Administrator.\n");
				break;
			default:
				fprintf(stderr, "  Unexpected error. Check driver installation.\n");
				break;
			}
			adcusbLogClose();
			return -1;
		}

        ok = (block_hwid_str != NULL) ? adcusbAddBlockRule(hDev, wPattern)
                                      : adcusbRemoveBlockRule(hDev, wPattern);
        if (ok)
        {
            printf("%s rule '%s': SUCCESS\n",
                   (block_hwid_str != NULL) ? "Block" : "Allow",
                   pattern_str);
        }
        else
        {
            fprintf(stderr, "%s rule '%s': FAILED (error %lu)\n",
                    (block_hwid_str != NULL) ? "Block" : "Allow",
                    pattern_str, GetLastError());
        }
        CloseHandle(hDev);
        adcusbLogClose();
        return ok ? 0 : -1;
    }

    /* --list-block-rules: query and display active block rules */
    if (list_block_rules_flag)
    {
        ADCUSB_BLOCK_RULE_LIST_RESPONSE response;
        HANDLE hDev;
        ULONG i;

        if (data.device == NULL)
        {
            fprintf(stderr, "Device not specified. Use -d <device>.\n");
            adcusbLogClose();
            return -1;
        }

        hDev = CreateFileA(data.device,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
		if (hDev == INVALID_HANDLE_VALUE)
		{
			DWORD dwErr;
			int i;

			dwErr = GetLastError();
			fprintf(stderr, "Cannot open device %s (error %lu)\n", data.device, dwErr);
			switch (dwErr)
			{
			case ERROR_FILE_NOT_FOUND:
				fprintf(stderr, "  Device '%s' does not exist.\n", data.device);
				fprintf(stderr, "  Check driver status: sc query adcusb\n");
				fprintf(stderr, "  If driver is running, check available devices:\n");
				filters_initialize();
				if (adcusbFilters[0] != NULL)
				{
					fprintf(stderr, "  Available devices:\n");
					for (i = 0; adcusbFilters[i] != NULL; i++)
						fprintf(stderr, "    %s\n", adcusbFilters[i]->device);
				}
				else
				{
					fprintf(stderr, "  No adcusb devices found. Try:\n");
					fprintf(stderr, "    1. Verify driver: sc query adcusb\n");
					fprintf(stderr, "    2. Reboot after installing UpperFilter\n");
				}
				filters_free();
				break;
			case ERROR_ACCESS_DENIED:
				fprintf(stderr, "  Access denied. Run this command as Administrator.\n");
				break;
			default:
				fprintf(stderr, "  Unexpected error. Check driver installation.\n");
				break;
			}
			adcusbLogClose();
			return -1;
		}

        memset(&response, 0, sizeof(response));
        if (adcusbListBlockRules(hDev, &response))
        {
            if (response.count == 0)
            {
                printf("No active block rules.\n");
            }
            else
            {
                printf("%lu active block rule(s):\n", response.count);
                for (i = 0; i < response.count; i++)
                {
                    printf("  [%lu] %ws\n", i + 1, response.patterns[i]);
                }
            }
        }
        else
        {
            fprintf(stderr, "List block rules failed (error %lu)\n", GetLastError());
        }
        CloseHandle(hDev);
        adcusbLogClose();
        return 0;
    }

    /* Handle extcap options separately from standard adcusbCMD options. */
    if (run_as_extcap || do_extcap_version || do_extcap_interfaces || do_extcap_dlts || do_extcap_config || do_extcap_capture)
    {
        ret = cmd_extcap(&data);
    }
    else
    {
        ret = 0;

        if ((data.filename == NULL) || (data.device == NULL))
        {
            if (data.filename != NULL)
            {
                free(data.filename);
                data.filename = NULL;
            }

            if (data.device != NULL)
            {
                free(data.device);
                data.device = NULL;
            }

            ret = cmd_interactive(&data);
        }

        if (ret == 0)
        {
            data.process = TRUE;
            start_capture(&data);
        }
    }

    /* Clean up */
    if (data.device != NULL)
    {
        free(data.device);
    }
    if (data.filename != NULL)
    {
        free(data.filename);
    }
    if (data.worker_process_thread != INVALID_HANDLE_VALUE)
    {
        CloseHandle(data.worker_process_thread);
    }
    if (data.job_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(data.job_handle);
    }
    if (data.exit_event != INVALID_HANDLE_VALUE)
    {
        CloseHandle(data.exit_event);
    }

    adcusbLogClose();
    return ret;
}

#if _MSC_VER >= 1700
int CALLBACK WinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPSTR lpCmdLine,
                     int nCmdShow)
{
    return adcusbcmd_main(__argc, __argv);
}
#endif
