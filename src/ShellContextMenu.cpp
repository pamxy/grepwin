// grepWin - regex search and replace for Windows

// Copyright (C) 2007-2013 - Stefan Kueng

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
#include "ShellContextMenu.h"
#include <shellapi.h>
#include "StringUtils.h"
#include "Registry.h"
#include "SearchInfo.h"
#include "LineData.h"
#include "ResString.h"
#include "resource.h"
#include <set>
#include <algorithm>

#define MIN_ID 6
#define MAX_ID 10000

IContextMenu2 * g_IContext2 = NULL;
IContextMenu3 * g_IContext3 = NULL;
WNDPROC g_OldWndProc = NULL;

struct icompare
{
    bool operator() (const std::wstring& lhs, const std::wstring& rhs) const
    {
        return _wcsicmp(lhs.c_str(), rhs.c_str()) < 0;
    }
};


CShellContextMenu::CShellContextMenu() 
    : m_pFolderhook(NULL)
    , m_psfFolder(NULL)
    , m_pidlArray(NULL)
    , m_pidlArrayItems(0)
    , nItems(0)
    , bDelete(FALSE)
    , m_Menu(NULL)
{
}

CShellContextMenu::~CShellContextMenu()
{
    // free all allocated data
    delete m_pFolderhook;
    if (m_psfFolder && bDelete)
        m_psfFolder->Release ();
    m_psfFolder = NULL;
    FreePIDLArray (m_pidlArray, m_pidlArrayItems);
    m_pidlArray = NULL;
    m_pidlArrayItems = 0;

    if (m_Menu)
        DestroyMenu(m_Menu);
}


// this functions determines which version of IContextMenu is available for those objects (always the highest one)
// and returns that interface
BOOL CShellContextMenu::GetContextMenu(HWND hWnd, void ** ppContextMenu, int & iMenuType)
{
    *ppContextMenu = NULL;
    if (m_pFolderhook)
        return FALSE;
    if (m_psfFolder == NULL)
        return FALSE;
    if (m_strVector.empty())
        return FALSE;

    HKEY ahkeys[16];
    SecureZeroMemory(ahkeys, _countof(ahkeys)*sizeof(HKEY));
    int numkeys = 0;
    if (RegOpenKey(HKEY_CLASSES_ROOT, L"*", &ahkeys[numkeys++]) != ERROR_SUCCESS)
        numkeys--;
    if (RegOpenKey(HKEY_CLASSES_ROOT, L"AllFileSystemObjects", &ahkeys[numkeys++]) != ERROR_SUCCESS)
        numkeys--;
    if (PathIsDirectory(m_strVector[0].filepath.c_str()))
    {
        if (RegOpenKey(HKEY_CLASSES_ROOT, L"Folder", &ahkeys[numkeys++]) != ERROR_SUCCESS)
            numkeys--;
        if (RegOpenKey(HKEY_CLASSES_ROOT, L"Directory", &ahkeys[numkeys++]) != ERROR_SUCCESS)
            numkeys--;
    }
    // find extension
    size_t dotpos = m_strVector[0].filepath.find_last_of('.');
    std::wstring ext;
    if (dotpos != std::string::npos)
    {
        ext = m_strVector[0].filepath.substr(dotpos);
        if (RegOpenKey(HKEY_CLASSES_ROOT, ext.c_str(), &ahkeys[numkeys++]) == ERROR_SUCCESS)
        {
            WCHAR buf[MAX_PATH] = {0};
            DWORD dwSize = MAX_PATH;
            if (RegQueryValueEx(ahkeys[numkeys-1], L"", NULL, NULL, (LPBYTE)buf, &dwSize) == ERROR_SUCCESS)
            {
                if (RegOpenKey(HKEY_CLASSES_ROOT, buf, &ahkeys[numkeys++]) != ERROR_SUCCESS)
                    numkeys--;
            }
        }
    }

    delete m_pFolderhook;
    m_pFolderhook = new CIShellFolderHook(m_psfFolder, this);

    LPCONTEXTMENU icm1 = NULL;
    if (FAILED(CDefFolderMenu_Create2(NULL, hWnd, (UINT)m_pidlArrayItems, (LPCITEMIDLIST*)m_pidlArray, m_pFolderhook, dfmCallback, numkeys, ahkeys, &icm1)))
        return FALSE;
    for (int i = 0; i < numkeys; ++i)
        RegCloseKey(ahkeys[i]);

    if (icm1)
    {   // since we got an IContextMenu interface we can now obtain the higher version interfaces via that
        if (icm1->QueryInterface(IID_IContextMenu3, ppContextMenu) == S_OK)
            iMenuType = 3;
        else if (icm1->QueryInterface(IID_IContextMenu2, ppContextMenu) == S_OK)
            iMenuType = 2;

        if (*ppContextMenu)
            icm1->Release(); // we can now release version 1 interface, cause we got a higher one
        else
        {
            // since no higher versions were found
            // redirect ppContextMenu to version 1 interface
            iMenuType = 1;
            *ppContextMenu = icm1;
        }
    }
    else
        return FALSE;

    return TRUE;
}


LRESULT CALLBACK CShellContextMenu::HookWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MENUCHAR:   // only supported by IContextMenu3
        if (g_IContext3)
        {
            LRESULT lResult = 0;
            g_IContext3->HandleMenuMsg2 (message, wParam, lParam, &lResult);
            return (lResult);
        }
        break;

    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        if (wParam)
            break; // if wParam != 0 then the message is not menu-related

    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        if (g_IContext3)
        {
            LRESULT lResult = 0;
            g_IContext3->HandleMenuMsg2 (message, wParam, lParam, &lResult);
            return (lResult);
        }
        else if (g_IContext2)
            g_IContext2->HandleMenuMsg (message, wParam, lParam);

        return TRUE;
        break;

    default:
        break;
    }

    // call original WndProc of window to prevent undefined behavior of window
    return ::CallWindowProc (g_OldWndProc , hWnd, message, wParam, lParam);
}


UINT CShellContextMenu::ShowContextMenu(HWND hWnd, POINT pt)
{
    int iMenuType = 0;  // to know which version of IContextMenu is supported
    LPCONTEXTMENU pContextMenu; // common pointer to IContextMenu and higher version interface

    if (GetWindowLongPtr(hWnd, GWLP_WNDPROC) == (LONG_PTR)HookWndProc)
        return 0;

    if (!GetContextMenu (hWnd, (void**)&pContextMenu, iMenuType))
        return 0;

    if (!m_Menu)
    {
        DestroyMenu(m_Menu);
        m_Menu = CreatePopupMenu();
    }

    CRegStdString regEditorCmd(L"Software\\grepWin\\editorcmd");
    std::wstring editorcmd = regEditorCmd;
    if (bPortable)
        editorcmd = g_iniFile.GetValue(L"global", L"editorcmd", L"");

    if (m_strVector.size() == 1)
    {
        if (editorcmd.size())
        {
            ::InsertMenu(m_Menu, 1, MF_BYPOSITION | MF_STRING, 5, TranslatedString(hInst, IDS_OPENWITHEDITOR).c_str());
            ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
        }

        ::InsertMenu(m_Menu, 1, MF_BYPOSITION | MF_STRING, 1, TranslatedString(hInst, IDS_OPENCONTAININGFOLDER).c_str());
        ::InsertMenu(m_Menu, 2, MF_BYPOSITION | MF_STRING, 2, TranslatedString(hInst, IDS_COPYPATH).c_str());
        ::InsertMenu(m_Menu, 3, MF_BYPOSITION | MF_STRING, 3, TranslatedString(hInst, IDS_COPYFILENAME).c_str());
        if (!m_lineVector.empty())
            ::InsertMenu(m_Menu, 4, MF_BYPOSITION | MF_STRING, 4, TranslatedString(hInst, IDS_COPYRESULT).c_str());
        ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
    }
    else if (m_strVector.size() > 1)
    {
        if (editorcmd.size())
        {
            ::InsertMenu(m_Menu, 1, MF_BYPOSITION | MF_STRING, 5, TranslatedString(hInst, IDS_OPENWITHEDITOR).c_str());
            ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
        }
        ::InsertMenu(m_Menu, 2, MF_BYPOSITION | MF_STRING, 2, TranslatedString(hInst, IDS_COPYPATHS).c_str());
        ::InsertMenu(m_Menu, 3, MF_BYPOSITION | MF_STRING, 3, TranslatedString(hInst, IDS_COPYFILENAMES).c_str());
        if (!m_lineVector.empty())
            ::InsertMenu(m_Menu, 4, MF_BYPOSITION | MF_STRING, 4, TranslatedString(hInst, IDS_COPYRESULTS).c_str());
        ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
    }
    // lets fill the our popup menu
    pContextMenu->QueryContextMenu(m_Menu, GetMenuItemCount(m_Menu), MIN_ID, MAX_ID, CMF_NORMAL | CMF_EXPLORE);

    // subclass window to handle menu related messages in CShellContextMenu
    if (iMenuType > 1)  // only subclass if its version 2 or 3
    {
        if (GetWindowLongPtr(hWnd, GWLP_WNDPROC) != (LONG_PTR)HookWndProc)
            g_OldWndProc = (WNDPROC)SetWindowLongPtr (hWnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
        if (iMenuType == 2)
            g_IContext2 = (LPCONTEXTMENU2)pContextMenu;
        else    // version 3
            g_IContext3 = (LPCONTEXTMENU3)pContextMenu;
    }
    else
        g_OldWndProc = NULL;

    UINT idCommand = TrackPopupMenu(m_Menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);

    if (g_OldWndProc) // un-subclass
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR) g_OldWndProc);

    if (idCommand >= MIN_ID && idCommand <= MAX_ID) // see if returned idCommand belongs to shell menu entries
    {
        InvokeCommand(pContextMenu, idCommand - MIN_ID);    // execute related command
        idCommand = 0;
    }
    else
    {
        switch (idCommand)
        {
        case 1:
            {
                // This is the command line for explorer which tells it to select the given file
                std::wstring sFolder = _T( "/Select,\"" ) + m_strVector[0].filepath + _T("\"");

                // Prepare shell execution params
                SHELLEXECUTEINFO shExecInfo   = { 0 };
                shExecInfo.cbSize             = sizeof(shExecInfo);
                shExecInfo.lpFile             = _T("explorer.exe");
                shExecInfo.lpParameters       = sFolder.c_str();
                shExecInfo.nShow              = SW_SHOWNORMAL;
                shExecInfo.lpVerb             = _T("open"); // Context menu item
                shExecInfo.fMask              = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_DDEWAIT | SEE_MASK_FLAG_NO_UI;

                // Select file in explorer
                ShellExecuteEx(&shExecInfo);
            }
            break;
        case 2:
            {
                std::wstring pathnames;
                for (auto it = m_strVector.begin(); it != m_strVector.end(); ++it)
                {
                    if (pathnames.size())
                        pathnames += _T("\r\n");
                    pathnames += it->filepath;
                }
                WriteAsciiStringToClipboard(pathnames.c_str(), hWnd);
            }
            break;
        case 3:
            {
                std::wstring pathnames;
                for (auto it = m_strVector.begin(); it != m_strVector.end(); ++it)
                {
                    if (pathnames.size())
                        pathnames += _T("\r\n");
                    std::wstring p = it->filepath;
                    p = p.substr(p.find_last_of('\\')+1);
                    pathnames += p;
                }
                WriteAsciiStringToClipboard(pathnames.c_str(), hWnd);
            }
            break;
        case 4:
            {
                std::wstring lines;
                for (auto it = m_lineVector.begin(); it != m_lineVector.end(); ++it)
                {
                    if (lines.size())
                        lines += _T("\r\n");
                    for (auto it2 = it->lines.cbegin(); it2 != it->lines.cend(); ++it2)
                    {
                        std::wstring l = it2->text;
                        std::replace(l.begin(), l.end(), '\n', ' ');
                        std::replace(l.begin(), l.end(), '\r', ' ');

                        lines += l;
                    }
                }
                WriteAsciiStringToClipboard(lines.c_str(), hWnd);
            }
            break;
        case 5:
            {
                if (!m_lineVector.empty())
                {
                    for (auto it = m_lineVector.cbegin(); it != m_lineVector.cend(); ++it)
                    {
                        for (auto it2 = it->lines.cbegin(); it2 != it->lines.cend(); ++it2)
                        {
                            std::wstring cmd = regEditorCmd;
                            SearchReplace(cmd, L"%path%", it->path.c_str());
                            wchar_t buf[40] = {0};
                            swprintf_s(buf, L"%ld", it2->number);
                            SearchReplace(cmd, L"%line%", buf);

                            STARTUPINFO startupInfo;
                            PROCESS_INFORMATION processInfo;
                            memset(&startupInfo, 0, sizeof(STARTUPINFO));
                            startupInfo.cb = sizeof(STARTUPINFO);
                            memset(&processInfo, 0, sizeof(PROCESS_INFORMATION));
                            CreateProcess(NULL, const_cast<TCHAR*>(cmd.c_str()), NULL, NULL, FALSE, 0, 0, NULL, &startupInfo, &processInfo);
                            CloseHandle(processInfo.hThread);
                            CloseHandle(processInfo.hProcess);
                        }
                    }
                }
                else
                {
                    for (auto it = m_strVector.begin(); it != m_strVector.end(); ++it)
                    {
                        std::wstring cmd = regEditorCmd;
                        SearchReplace(cmd, L"%path%", it->filepath.c_str());
                        if (it->matchlinesnumbers.size())
                        {
                            wchar_t buf[40] = {0};
                            swprintf_s(buf, L"%ld", it->matchlinesnumbers[0]);
                            SearchReplace(cmd, L"%line%", buf);
                        }
                        else
                            SearchReplace(cmd, L"%line%", L"0");

                        STARTUPINFO startupInfo;
                        PROCESS_INFORMATION processInfo;
                        memset(&startupInfo, 0, sizeof(STARTUPINFO));
                        startupInfo.cb = sizeof(STARTUPINFO);
                        memset(&processInfo, 0, sizeof(PROCESS_INFORMATION));
                        CreateProcess(NULL, const_cast<TCHAR*>(cmd.c_str()), NULL, NULL, FALSE, 0, 0, NULL, &startupInfo, &processInfo);
                        CloseHandle(processInfo.hThread);
                        CloseHandle(processInfo.hProcess);
                    }
                }
            }
            break;
        }
    }

    pContextMenu->Release();
    g_IContext2 = NULL;
    g_IContext3 = NULL;
    delete m_pFolderhook;
    m_pFolderhook = NULL;
    return (idCommand);
}


void CShellContextMenu::InvokeCommand(LPCONTEXTMENU pContextMenu, UINT idCommand)
{
    CMINVOKECOMMANDINFO cmi = {0};
    cmi.cbSize = sizeof (CMINVOKECOMMANDINFO);
    cmi.lpVerb = (LPSTR) MAKEINTRESOURCE (idCommand);
    cmi.nShow = SW_SHOWNORMAL;

    pContextMenu->InvokeCommand (&cmi);
}

void CShellContextMenu::SetObjects(const std::vector<CSearchInfo>& strVector, const std::vector<LineData>& lineVector)
{
    // free all allocated data
    if (m_psfFolder && bDelete)
        m_psfFolder->Release();
    m_psfFolder = NULL;
    FreePIDLArray(m_pidlArray, m_pidlArrayItems);
    m_pidlArray = NULL;

    // get IShellFolder interface of Desktop (root of shell namespace)
    SHGetDesktopFolder(&m_psfFolder);    // needed to obtain full qualified pidl

    // ParseDisplayName creates a PIDL from a file system path relative to the IShellFolder interface
    // but since we use the Desktop as our interface and the Desktop is the namespace root
    // that means that it's a fully qualified PIDL, which is what we need

    nItems = (int)strVector.size();
    m_pidlArray = (LPITEMIDLIST *)CoTaskMemAlloc((nItems + 10) * sizeof (LPITEMIDLIST));
    SecureZeroMemory(m_pidlArray, (nItems + 10) * sizeof (LPITEMIDLIST));
    m_pidlArrayItems = 0;
    int succeededItems = 0;
    LPITEMIDLIST pidl = NULL;
    m_strVector.clear();
    m_lineVector.clear();
    for (int i = 0; i < nItems; i++)
    {
        std::unique_ptr<WCHAR[]> filepath(new WCHAR[strVector[i].filepath.size()+3]);
        wcscpy_s(filepath.get(), strVector[i].filepath.size()+2, strVector[i].filepath.c_str());
        if (SUCCEEDED(m_psfFolder->ParseDisplayName(NULL, 0, filepath.get(), NULL, &pidl, NULL)))
        {
            m_pidlArray[succeededItems++] = pidl;           // copy pidl to pidlArray
            m_strVector.push_back(strVector[i]);
            if (lineVector.size() > (size_t)i)
                m_lineVector.push_back(lineVector[i]);
        }
    }
    m_pidlArrayItems = succeededItems;

    bDelete = TRUE; // indicates that m_psfFolder should be deleted by CShellContextMenu
}

void CShellContextMenu::FreePIDLArray(LPITEMIDLIST *pidlArray, int nItems)
{
    if (!pidlArray)
        return;

    for (int i = 0; i < nItems; i++)
    {
        if (pidlArray[i])
            CoTaskMemFree(pidlArray[i]);
    }
    CoTaskMemFree(pidlArray);
}

HRESULT CShellContextMenu::dfmCallback( IShellFolder * /*psf*/, HWND /*hwnd*/, IDataObject * /*pdtobj*/, UINT uMsg, WPARAM /*wParam*/, LPARAM /*lParam*/ )
{
    switch (uMsg)
    {
    case DFM_MERGECONTEXTMENU:
        return S_OK;
    case DFM_INVOKECOMMAND:
    case DFM_INVOKECOMMANDEX:
    case DFM_GETDEFSTATICID: // Required for Windows 7 to pick a default
        return S_FALSE;
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CIShellFolderHook::GetUIObjectOf( HWND hwndOwner, UINT cidl, LPCITEMIDLIST *apidl, REFIID riid, UINT *rgfReserved, void **ppv )
{
    if(InlineIsEqualGUID(riid, IID_IDataObject))
    {
        HRESULT hres = m_iSF->GetUIObjectOf(hwndOwner, cidl, apidl, IID_IDataObject, NULL, ppv);
        if (FAILED(hres))
            return hres;

        IDataObject * idata = (LPDATAOBJECT)(*ppv);
        // the IDataObject returned here doesn't have a HDROP, so we create one ourselves and add it to the IDataObject
        // the HDROP is necessary for most context menu handlers

        // it seems the paths in the HDROP need to be sorted, otherwise
        // it might not work properly or even crash.
        // to get the items sorted, we just add them to a set - that way we g
        std::set<std::wstring, icompare> sortedpaths;
        for (auto it = m_pShellContextMenu->m_strVector.cbegin(); it != m_pShellContextMenu->m_strVector.cend(); ++it)
            sortedpaths.insert(it->filepath);

        int nLength = 0;
        for (auto it = sortedpaths.cbegin(); it != sortedpaths.cend(); ++it)
        {
            nLength += (int)it->size();
            nLength += 1; // '\0' separator
        }
        int nBufferSize = sizeof(DROPFILES) + ((nLength+5)*sizeof(TCHAR));
        std::unique_ptr<char[]> pBuffer(new char[nBufferSize]);
        SecureZeroMemory(pBuffer.get(), nBufferSize);
        DROPFILES* df = (DROPFILES*)pBuffer.get();
        df->pFiles = sizeof(DROPFILES);
        df->fWide = 1;
        TCHAR* pFilenames = (TCHAR*)((BYTE*)(pBuffer.get()) + sizeof(DROPFILES));
        TCHAR* pCurrentFilename = pFilenames;

        for (auto it = sortedpaths.cbegin(); it != sortedpaths.cend(); ++it)
        {
            wcscpy_s(pCurrentFilename, it->size()+1, it->c_str());
            pCurrentFilename += it->size();
            *pCurrentFilename = '\0'; // separator between file names
            pCurrentFilename++;
        }
        *pCurrentFilename = '\0'; // terminate array
        pCurrentFilename++;
        *pCurrentFilename = '\0'; // terminate array
        STGMEDIUM medium = {0};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = GlobalAlloc(GMEM_ZEROINIT|GMEM_MOVEABLE, nBufferSize+20);
        if (medium.hGlobal)
        {
            LPVOID pMem = ::GlobalLock(medium.hGlobal);
            if (pMem)
            {
                memcpy(pMem, pBuffer.get(), nBufferSize);
                GlobalUnlock(medium.hGlobal);
                FORMATETC formatetc = {0};
                formatetc.cfFormat = CF_HDROP;
                formatetc.dwAspect = DVASPECT_CONTENT;
                formatetc.lindex = -1;
                formatetc.tymed = TYMED_HGLOBAL;
                medium.pUnkForRelease = NULL;
                hres = idata->SetData(&formatetc, &medium, TRUE);
                return hres;
            }
        }
        return E_OUTOFMEMORY;
    }
    else 
    {
        // just pass it on to the base object
        return m_iSF->GetUIObjectOf(hwndOwner, cidl, apidl, riid, rgfReserved, ppv);
    }
}

