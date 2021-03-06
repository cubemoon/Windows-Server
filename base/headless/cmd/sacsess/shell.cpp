/*++

Copyright (c) 1999-2001  Microsoft Corporation

Module Name:

    shell.cpp

Abstract:

    Class for creating a command console shell

Author:

    Brian Guarraci (briangu) 2001.

Revision History:

--*/

#include <cmnhdr.h>
#include <utils.h>
#include <Shell.h>
#include <Session.h>
 
//
// default shell
//
#define DEFAULT_SHELL  L"cmd.exe"

CShell::CShell()
/*++

Routine Description:

    Constructor              
                  
Arguments:

    None           
          
Return Value:

    N/A    

--*/
{
    m_hProcess      = NULL;
    m_bHaveProfile  = FALSE;                                         
    m_hProfile      = INVALID_HANDLE_VALUE;
    m_hWinSta       = NULL;
    m_hDesktop      = NULL;

}

CShell::~CShell()
/*++

Routine Description:

    Destructor              
                  
Arguments:

    N/A    
          
Return Value:

    N/A    

--*/
{
    NOTHING;    
}

BOOL 
CShell::StartUserSession (
    CSession    *session,
    HANDLE      hToken
    )
/*++

Routine Description:

    This routine launches the user-mode shell process, which
    will serve as the session process.

Arguments:

    session - the session to associate this process with
    hToken  - authenticated credentials to start the process with                                      
                                      
Return Value:

    TRUE    - the user-mode process was started successfully
    FALSE   - otherwise
                                                                     
--*/
{
    BOOL    bSuccess;

    //
    // Attempt to launch the shell process
    //
    bSuccess = StartProcess( hToken );
    ASSERT_STATUS(bSuccess, FALSE);

    //
    // Tell the session to wait on the process handle
    // This way, if the process exits, the session will
    // know about it.
    //
    session->AddHandleToWaitOn( m_hProcess );

    return( bSuccess );

}

BOOL
CShell::CreateIOHandles(
    OUT PHANDLE ConOut,
    OUT PHANDLE ConIn
    )
/*++

Routine Description:

    Allocate a new console and create the IO handles
    that will be used by the command console process

    Note: the console out handle created here is only
          valid for the screen buffer used by the command
          console process. If the user runs the an app
          which uses the CreateConsoleScreenBuffer and
          SetConsoleActiveScreenBuffer APIs, the scraper
          must create a new CONOUT$ handle to point to 
          the new screen buffer.

Arguments:

    ConOut   - the new console out handle
    ConIn    - the new console in handle
          
Return Value:

    TRUE    - the handles were created
    FALSE   - otherwise

--*/
{
    SECURITY_ATTRIBUTES sa;

    INHERITABLE_NULL_DESCRIPTOR_ATTRIBUTE( sa );
    
    //
    // default: we didnt open the stdio handles
    //
    *ConOut = INVALID_HANDLE_VALUE;
    *ConIn = INVALID_HANDLE_VALUE;

    //
    // We don't need to create a new console because
    // the process was created with CREATE_NEW_CONSOLE
    //

    //
    // Open the console input handle
    //
    *ConIn = CreateFile(
        L"CONIN$", 
        GENERIC_READ | GENERIC_WRITE, 
        0, 
        &sa, 
        OPEN_EXISTING, 
        FILE_ATTRIBUTE_NORMAL, 
        NULL 
        );
    ASSERT( *ConIn != INVALID_HANDLE_VALUE );
    if ( INVALID_HANDLE_VALUE == *ConIn) {
        goto ExitOnError;
    }

    //
    // Open the console output handle
    //
    *ConOut = CreateFile(
        L"CONOUT$", 
        GENERIC_READ | GENERIC_WRITE, 
        0, 
        &sa,
        OPEN_EXISTING, 
        FILE_ATTRIBUTE_NORMAL, 
        NULL 
        );
    ASSERT( *ConOut != INVALID_HANDLE_VALUE );
    if ( INVALID_HANDLE_VALUE == *ConOut ) {
        goto ExitOnError;
    }

    return(TRUE);

ExitOnError :    

    if (*ConOut != INVALID_HANDLE_VALUE) {
        CloseHandle(*ConOut);
    }
    if (*ConIn != INVALID_HANDLE_VALUE) {
        CloseHandle(*ConIn);
    }
    
    return( FALSE );
}

BOOL 
CShell::StartProcess (
    HANDLE  hToken
    )
/*++

Routine Description:

    This routine creates and initializes the shell process.  
    
    SECURITY: (dependencies)
        
        environmentblock    to get path of cmd.exe
            
        cmd.exe             the shell process
            
        CreateProcess...()  initiates shell process
            
        console             stdio pipes
    
        registry            determine:
         
                            1. if we should load profile
                            2. which create process method to use
        
        user profile        loaded and applied to user token
    
Arguments:

    hToken - if needed, the authenticated credentials to start the 
             process with.                              
          
Return Value:

    TRUE    - the process was started successfully
    FALSE   - otherwise                          

--*/
{
    STARTUPINFO         si;
    BOOL                bRetVal = TRUE;
    DWORD               dwExitCode = 0;
    BOOL                b;
    HANDLE              hStdError = INVALID_HANDLE_VALUE;
    HANDLE              hHandleToDuplicate = INVALID_HANDLE_VALUE;
    HANDLE              hConOut = INVALID_HANDLE_VALUE;
    HANDLE              hConIn = INVALID_HANDLE_VALUE;
    PTCHAR              pCmdBuf;
    BOOL                bHaveEnvironment;
    LPVOID              lpEnv;
    PWCHAR              DefaultShell = DEFAULT_SHELL;

    //
    // Allocate a new console and create the IO handles
    // that will be used by the command console process
    //
    b = CreateIOHandles(
        &hConOut,
        &hConIn
        );
    
    if (!b) {
        goto ExitOnError;
    }

    //
    // we want to use the console out as the std err
    //
    hHandleToDuplicate = hConOut;

    b = DuplicateHandle( 
        GetCurrentProcess(), 
        hHandleToDuplicate,
        GetCurrentProcess(), 
        &hStdError,
        0,
        TRUE, 
        DUPLICATE_SAME_ACCESS
        );
    
    if (!b) {
        hStdError = hConOut;
    }

    //
    // Restores normal processing of CTRL+C input,
    // (behavior is iherited by child process)
    //
    SetConsoleCtrlHandler( NULL, FALSE );

    //
    // If the admin/registry has specified that it is ok to load profiles,
    // the attempt to do so
    //
    if (IsLoadProfilesEnabled()) {
        
        //
        // Attempt to load the user's profile
        //    
        m_bHaveProfile = UtilLoadProfile(
            hToken, 
            &m_hProfile
            );

        //
        // Attempt to load the user's environment block
        //    
        bHaveEnvironment = UtilLoadEnvironment(
            hToken, 
            &lpEnv
            );
    
        if (!bHaveEnvironment) {
            lpEnv = NULL;
        }
        
    } else {

        //
        // nothing was loaded
        //
        lpEnv = NULL;
        bHaveEnvironment = FALSE;
        
        m_bHaveProfile = FALSE;
        
    }

    //
    // If the command console session requires authentication,
    // then create the cmd.exe process in the context that the
    // user authenticated in - otherwise, create the process
    // in the context that the service runs in.
    //
    if( NeedCredentials() ) {

        ASSERT( hToken != INVALID_HANDLE_VALUE);
        
        do {

            PROCESS_INFORMATION     pi;
            HWINSTA                 hOldWinSta;
            PWCHAR                  winStaName;

            //
            // We need to grant permission to the default desktop
            // 
            b = CreateSACSessionWinStaAndDesktop(
                hToken,
                &hOldWinSta,
                &m_hWinSta,
                &m_hDesktop,
                &winStaName
                );

            if (!b) {
                ASSERT(0);
                break;
            }

            do {

                //
                // configure the command console process startup
                // info to use the handles we want and other misc.
                // config details.
                //
                FillProcessStartupInfo( 
                    &si,
                    winStaName,
                    hConIn, 
                    hConOut, 
                    hStdError 
                    );

                //
                // get the pathname to the SAC session exe
                //
                pCmdBuf = GetPathOfTheExecutable();

                if (pCmdBuf == NULL) {
                    b = FALSE;
                    break;
                }

                //
                // Create the cmd.exe process as user referred by hToken
                //
                b = CreateProcessAsUser(
                    hToken,                     // HANDLE hToken
                    pCmdBuf,                    // application name
                    DefaultShell,               // command line
                    NULL,                       // process security descriptor
                    NULL,                       // thread security descriptor
                    TRUE,                       // handle inheritance?
                    CREATE_UNICODE_ENVIRONMENT | CREATE_SEPARATE_WOW_VDM, // creation flags
                    lpEnv,                      // environment block
                    NULL,                       // current directory
                    &si,                        // startup information
                    &pi );                      // process information

                //
                // NOTE: CreateProcessAsUser API Issue
                //
                // we must keep the handles to the desktop and window station
                // open until the cmd.exe process handle is signaled - the
                // process is closed.  The reason for this is that the
                // CreateProcessAsUser routine returns BEFORE the cmd.exe
                // process is fully initialized.  Hence, there are no
                // references to the winsta/desktop pair taken.  If we close
                // the handles they will be cleaned up because we are the only
                // reference.  The cmd.exe process will then try to reference
                // the winsta/desktop and fail because they are gone - it'll
                // get an "GDI out of resources" type message.  Typically,
                // with CreateProcessAsUser, you'd use WaitForProcessIdle to
                // ensure the process is initialized - you could then safely
                // close the handles to the winsta/desktop pair.  However,
                // for console apps, WaitForProcessIdle returns IMMEDIATELY.
                // Thus we are forced to hold the handles until we are sure
                // the process is doe with them.
                //

                //
                // release the executable path
                //
                delete [] pCmdBuf;

                if (!b) {
                    break;
                }

                //
                // Make sure the cmd.exe process didn't die
                //
                GetExitCodeProcess( pi.hProcess, &dwExitCode );

                if ( dwExitCode != STILL_ACTIVE ) {
                    b = FALSE;
                    bRetVal = FALSE;
                    break;
                }

                //
                // keep the handle to the cmd.exe process
                //
                m_hProcess = pi.hProcess;

                //
                // close the handle to the cmd.exe thread 
                //
                if (pi.hThread != INVALID_HANDLE_VALUE) {
                    CloseHandle( pi.hThread );
                }

            } while(FALSE);

            if (winStaName) {
                delete [] winStaName;
            }
            if (hOldWinSta) {
                SetProcessWindowStation(hOldWinSta);
            }
        } while ( FALSE );
        
    } else {
        
        do {

            PROCESS_INFORMATION pi;
            
            //
            // configure the command console process startup
            // info to use the handles we want and other misc.
            // config details.
            //
            FillProcessStartupInfo( 
                &si,
                L"winsta0\\default",
                hConIn, 
                hConOut, 
                hStdError 
                );

            //
            // get the pathname to the SAC session exe
            //
            pCmdBuf = GetPathOfTheExecutable();

            if (pCmdBuf == NULL) {
                b = FALSE;
                break;
            }

            //
            // Create the cmd.exe process as the same user running the service
            //
            b = CreateProcess(
                pCmdBuf,                    // application name
                DefaultShell,               // command line
                NULL,                       // process security descriptor
                NULL,                       // thread security descriptor
                TRUE,                       // handle inheritance?
                CREATE_UNICODE_ENVIRONMENT | CREATE_SEPARATE_WOW_VDM, // creation flags
                lpEnv,                      // environment block
                NULL,                       // current directory
                &si,                        // startup information
                &pi );                      // process information
        
            //
            // release the executable path
            //
            delete [] pCmdBuf;

            if (!b) {
                break;
            }

            //
            // Make sure the cmd.exe process didn't die
            //
            GetExitCodeProcess( pi.hProcess, &dwExitCode );

            if ( dwExitCode != STILL_ACTIVE ) {
                b = FALSE;
                bRetVal = FALSE;
                break;
            }

            //
            // keep the handle to the cmd.exe process
            //
            m_hProcess = pi.hProcess;

            //
            // close the handle to the cmd.exe thread 
            //
            if (pi.hThread != INVALID_HANDLE_VALUE) {
                CloseHandle( pi.hThread );
            }
        
        } while ( FALSE );
    
    }
    
    //
    // If we were able to load the user's environment,
    // then unload it
    //
    if (bHaveEnvironment) {

        UtilUnloadEnvironment((PVOID)lpEnv);

        bHaveEnvironment = FALSE;
        lpEnv = NULL;

    }

    //
    // Ignore ctrl+c input
    //
    SetConsoleCtrlHandler( NULL, TRUE );

    //
    // If we failed,
    // then cleanup
    //
    if( !b ) {
        bRetVal = FALSE;
        goto ExitOnError;
    }

    goto Done;

ExitOnError:
    
    //
    // If we were able to load the user's profile,
    // then unload it
    //
    if (m_bHaveProfile) {

        UtilUnloadProfile(
            hToken,
            m_hProfile
            );

        m_bHaveProfile = FALSE;

    }

Done:
    
    //
    // We don't need these handles anymore
    // 
    if ((hStdError != INVALID_HANDLE_VALUE) && (hStdError != hConOut)) {
        CloseHandle( hStdError );
    }
    if (hConIn != INVALID_HANDLE_VALUE) {
        CloseHandle( hConIn );
    }
    if (hConOut != INVALID_HANDLE_VALUE) {
        CloseHandle( hConOut );
    }
    
    return( bRetVal );
}

void 
CShell::Shutdown (
    VOID
    )
/*++

Routine Description:

    This routine cleans up the shell process.    
        
Arguments:

    None                                             
          
Return Value:

    None
        
--*/
{

    //
    // if we have started the cmd process,
    // then terminate it
    //
    if (m_hProcess != INVALID_HANDLE_VALUE) {
        
        HANDLE  hToken = INVALID_HANDLE_VALUE;
        BOOL    bHaveToken;
        
        //
        // Load the user token for the cmd process
        // so we can unload the profile and environment
        //
        bHaveToken = OpenProcessToken(
            m_hProcess,
            TOKEN_ALL_ACCESS,
            &hToken
            );
        
        //
        // terminate the cmd process
        //
        TerminateProcess(m_hProcess, 0); 
        
        //
        // we are done with the process
        //
        CloseHandle( m_hProcess ); 
    
        //
        // unroll user token settings
        //
        if (bHaveToken) {

            //
            // If we were able to load the user's profile,
            // then unload it
            //
            if (m_bHaveProfile) {
                
                UtilUnloadProfile(
                    hToken,
                    m_hProfile
                    );
            
                m_bHaveProfile = FALSE;

            }

            //
            // we are done with the token
            //
            if (hToken != INVALID_HANDLE_VALUE) {
                CloseHandle(hToken);
            }

        }
    
    }

    //
    // now that the cmd.exe process is done (or died)
    // we can close the desktop and winsta handles
    //
    if (m_hDesktop != NULL) {
        CloseDesktop(m_hDesktop);
    }
    if (m_hWinSta != NULL) {
        CloseWindowStation(m_hWinSta);
    }

}                

BOOL
CShell::IsLoadProfilesEnabled(
    VOID
    )

/*++

Routine Description:
                                   
    This routine determines if the profile loading behavior is enabled

Arguments:

    None.

Return Value:

    TRUE    - profile loading behavior is enabled
    FALSE   - otherwise              

--*/

{
    DWORD       rc;
    HKEY        hKey;
    DWORD       DWord;
    DWORD       dwsize;
    DWORD       DataType;

    //
    // See if the user gave us a registry key to disable the profile loading behavior
    //
    rc = RegOpenKeyEx( HKEY_LOCAL_MACHINE,
                       SACSVR_PARAMETERS_KEY,
                       0,
                       KEY_READ,
                       &hKey );
    
    if( rc == NO_ERROR ) {
        
        dwsize = sizeof(DWORD);
        
        rc = RegQueryValueEx(
                        hKey,
                        SACSVR_LOAD_PROFILES_DISABLED_VALUE,
                        NULL,
                        &DataType,
                        (LPBYTE)&DWord,
                        &dwsize );

        RegCloseKey( hKey );

        if ((rc == NO_ERROR) && 
            (DataType == REG_DWORD) && 
            (dwsize == sizeof(DWORD))
            ) {
            
            return DWord == 1 ? FALSE : TRUE;
        
        }
    
    }

    //
    // default: Loading profiles is enabled
    //
    return TRUE;

}

PTCHAR
CShell::GetPathOfTheExecutable(
    VOID
    )
/*++

Routine Description:

    Find out where the SAC session executable is located.

Arguments:

    NONE
                    
Return Value:

    Failure: NULL
    SUCCESS: pointer to path (caller must free)    

--*/
{
    PTCHAR  SystemDir;
    PTCHAR  pBuffer;
    ULONG   length;

    //
    // allocate the buffe we'll use to hold the system path
    //
    SystemDir = new TCHAR[MAX_PATH+1];

    //
    // default: we didnt create a new path
    //
    pBuffer = NULL;

    do {

        //
        // get the system path
        // 
        length = GetSystemDirectoryW(SystemDir, MAX_PATH+1);

        if (length == 0) {
            break;            
        }

        //
        // compute the length
        //
        length += 1; // backslash
        length += lstrlen(DEFAULT_SHELL);
        length += 1; // NULL termination

        //
        // allocate our new path
        //
        pBuffer = new TCHAR[length];

        if (pBuffer == NULL) {
            break;
        }

        //
        // create the path
        //
        wnsprintf(
            pBuffer,
            length,
            L"%s\\%s",
            SystemDir,
            DEFAULT_SHELL
            );

    } while ( FALSE );
    
    delete [] SystemDir;

    return pBuffer;
}

