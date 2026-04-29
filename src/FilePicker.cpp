#include "FilePicker.h"
#include "DustLog.h"

#include <windows.h>
#include <shobjidl.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace FilePicker
{

enum class State : int { Idle = 0, Running = 1, Ready = 2 };

static std::atomic<int> gState{(int)State::Idle};
static std::thread       gThread;
static std::mutex        gResultMutex;
static std::string       gResult;

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wlen);
    return w;
}

static std::string WideToUtf8(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static void PickerThreadFunc(std::string title)
{
    std::string out;

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comOwned = SUCCEEDED(hrInit);

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg)
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

        if (!title.empty())
        {
            std::wstring wtitle = Utf8ToWide(title);
            dlg->SetTitle(wtitle.c_str());
        }

        hr = dlg->Show(nullptr);
        if (SUCCEEDED(hr))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item)
            {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
                {
                    out = WideToUtf8(psz);
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            Log("FilePicker: IFileOpenDialog::Show failed (hr=0x%08X)", (unsigned)hr);
        }
        dlg->Release();
    }
    else
    {
        Log("FilePicker: CoCreateInstance(IFileOpenDialog) failed (hr=0x%08X)", (unsigned)hr);
    }

    if (comOwned) CoUninitialize();

    {
        std::lock_guard<std::mutex> lk(gResultMutex);
        gResult = std::move(out);
    }
    gState.store((int)State::Ready, std::memory_order_release);
}

bool StartFolderPicker(const char* title)
{
    int expected = (int)State::Idle;
    if (!gState.compare_exchange_strong(expected, (int)State::Running))
        return false;

    if (gThread.joinable()) gThread.join();

    std::string t = title ? title : "";
    gThread = std::thread(PickerThreadFunc, std::move(t));
    return true;
}

bool Poll(std::string& outPath)
{
    if (gState.load(std::memory_order_acquire) != (int)State::Ready)
        return false;

    if (gThread.joinable()) gThread.join();

    {
        std::lock_guard<std::mutex> lk(gResultMutex);
        outPath = std::move(gResult);
        gResult.clear();
    }
    gState.store((int)State::Idle, std::memory_order_release);
    return true;
}

bool IsBusy()
{
    return gState.load(std::memory_order_acquire) == (int)State::Running;
}

void Shutdown()
{
    if (!gThread.joinable()) return;

    // If the dialog is still open at shutdown, joining would deadlock the
    // render thread until the user closes the dialog manually. Detach so the
    // OS reclaims the thread when the process exits — the dialog window dies
    // with the process. Only join when the worker is already done.
    if (gState.load(std::memory_order_acquire) == (int)State::Running)
        gThread.detach();
    else
        gThread.join();
}

} // namespace FilePicker
