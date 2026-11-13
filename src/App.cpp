#include <afxwin.h>
#include <afxole.h>
#include <atlimage.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include "Engine.h"
#include "Protocol.h"
#include <memory>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace pb;

static void loadImage(CImage &image, UINT resource)
{
    HINSTANCE module = AfxGetResourceHandle();
    HRSRC r = FindResourceW(module, MAKEINTRESOURCEW(resource), RT_RCDATA);
    if (!r)
        throw std::runtime_error("Imagem do launcher ausente.");
    ComPtr<IStream> stream;
    stream.Attach(
        SHCreateMemStream((const BYTE *)LockResource(LoadResource(module, r)), SizeofResource(module, r)));
    if (!stream || FAILED(image.Load(stream.Get())))
        throw std::runtime_error("Imagem do launcher inválida.");
}
enum
{
    CheckId = 201,
    PrimaryId,
    SettingsId,
    MinId,
    CloseId,
    StatusId,
    FileId
};

class ButtonHover
{
    HWND current = nullptr;

  public:
    bool isOver(HWND button) const { return current == button; }

    static void trackLeave(HWND button)
    {
        TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, button, 0};
        ::TrackMouseEvent(&event);
    }
    void set(HWND button)
    {
        if (button == current)
            return;
        HWND previous = current;
        current = button;
        constexpr UINT paint = RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE;
        if (previous)
            ::RedrawWindow(previous, nullptr, nullptr, paint);
        if (button)
        {
            trackLeave(button);
            ::RedrawWindow(button, nullptr, nullptr, paint);
        }
    }
    bool sync(HWND owner)
    {
        POINT point{};
        HWND button = nullptr;
        if (::IsWindowVisible(owner) && ::IsWindowEnabled(owner) && !::IsIconic(owner))
        {
            if (!::GetCursorPos(&point))
                return false;
            HWND hit = ::WindowFromPoint(point);
            HWND capture = ::GetCapture();
            if (hit && ::GetParent(hit) == owner && ::IsWindowEnabled(hit) && (!capture || capture == hit))
            {
                const int id = ::GetDlgCtrlID(hit);
                if (id >= CheckId && id <= CloseId)
                    button = hit;
            }
        }
        set(button);
        return true;
    }
};

class SkinButton : public CButton
{
    ButtonHover &pointer;
    CImage *normal = nullptr;
    CImage *hover = nullptr;
    CImage *down = nullptr;
    CImage *inactive = nullptr;
    bool drawPauseCaption = false;
  public:
    explicit SkinButton(ButtonHover &pointer) : pointer(pointer) {}
    void skin(CImage *n, CImage *h, CImage *d, CImage *off, bool pauseCaption = false)
    {
        normal = n;
        hover = h;
        down = d;
        inactive = off;
        drawPauseCaption = pauseCaption;
        pointer.sync(::GetParent(m_hWnd));
        Invalidate(FALSE);
    }
    void DrawItem(LPDRAWITEMSTRUCT item) override
    {
        auto img = normal;
        if (!IsWindowEnabled() && inactive)
            img = inactive;
        else if (pointer.isOver(m_hWnd) && (item->itemState & ODS_SELECTED) && down)
            img = down;
        else if (pointer.isOver(m_hWnd) && hover)
            img = hover;
        if (!img)
            return;
        if (!drawPauseCaption)
        {
            img->Draw(item->hDC, item->rcItem);
            return;
        }
        // The pause BMPs have a one-pixel color key and an English caption on a horizontal band.
        CRect source(1, 1, img->GetWidth() - 1, img->GetHeight() - 1);
        CRect bounds(item->rcItem);
        constexpr int sideWidth = 34; // Each arrow fits in a 34-pixel strip.
        CRect band(bounds);
        band.DeflateRect(sideWidth, 0);
        img->Draw(item->hDC, CRect(bounds.left, bounds.top, band.left, bounds.bottom),
                  CRect(source.left, source.top, source.left + sideWidth, source.bottom));
        img->Draw(item->hDC, band, CRect(3, source.top, 4, source.bottom));
        img->Draw(item->hDC, CRect(band.right, bounds.top, bounds.right, bounds.bottom),
                  CRect(source.right - sideWidth, source.top, source.right, source.bottom));

        CDC *dc = CDC::FromHandle(item->hDC);
        int saved = dc->SaveDC();
        dc->SelectObject(GetFont());
        dc->SetBkMode(TRANSPARENT);
        dc->SetTextColor(img->GetPixel(45, 39));
        CString caption;
        GetWindowText(caption);
        caption.MakeUpper();
        dc->DrawText(caption, band, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        dc->RestoreDC(saved);
    }
    afx_msg void OnMouseMove(UINT f, CPoint p)
    {
        CRect bounds;
        GetClientRect(&bounds);
        if (bounds.PtInRect(p) && IsWindowEnabled() && GetParent()->IsWindowEnabled())
            pointer.set(m_hWnd);
        else if (pointer.isOver(m_hWnd))
            pointer.set(nullptr);
        CButton::OnMouseMove(f, p);
    }
    afx_msg LRESULT OnLeave(WPARAM, LPARAM)
    {
        if (pointer.isOver(m_hWnd) && pointer.sync(::GetParent(m_hWnd)) && pointer.isOver(m_hWnd))
            ButtonHover::trackLeave(m_hWnd);
        return 0;
    }
    afx_msg void OnEnable(BOOL enabled)
    {
        CButton::OnEnable(enabled);
        pointer.sync(::GetParent(m_hWnd));
        Invalidate(FALSE);
    }
    afx_msg void OnCaptureChanged(CWnd *target)
    {
        CButton::OnCaptureChanged(target);
        pointer.sync(::GetParent(m_hWnd));
    }
    afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }
    DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(SkinButton, CButton)
ON_WM_MOUSEMOVE()
ON_WM_ENABLE()
ON_WM_CAPTURECHANGED()
ON_WM_ERASEBKGND()
ON_MESSAGE(WM_MOUSELEAVE, OnLeave)
END_MESSAGE_MAP()

// Init.Designer.cs uses a fixed, captionless dialog with a 266 x 41 client area.
class StartupWindow final : public CWnd
{
    CStatic label;
    CFont font;
    HWND mainWindow = nullptr;

  public:
    bool open(HWND main, const std::wstring &text)
    {
        mainWindow = main;
        constexpr DWORD style = WS_POPUP | WS_BORDER | WS_CLIPCHILDREN;
        constexpr DWORD exStyle = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
        CRect frame(0, 0, 266, 41);
        ::AdjustWindowRectEx(&frame, style, FALSE, exStyle);
        auto cls = AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), ::GetSysColorBrush(COLOR_3DFACE));
        if (!CreateEx(exStyle, cls, L"", style, CRect(CPoint(0, 0), frame.Size()), nullptr, 0))
            return false;
        font.CreatePointFont(83, L"Microsoft Sans Serif");
        label.Create(text.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                     CRect(-4, 9, 270, 32), this, 1017);
        label.SetFont(&font);
        CenterWindow();
        ShowWindow(SW_SHOWNORMAL);
        UpdateWindow();
        return true;
    }
    afx_msg void OnClose() { ::PostMessageW(mainWindow, WM_CLOSE, 0, 0); }
    afx_msg HBRUSH OnCtlColor(CDC *dc, CWnd *, UINT)
    {
        dc->SetBkColor(::GetSysColor(COLOR_3DFACE));
        dc->SetTextColor(::GetSysColor(COLOR_BTNTEXT));
        return ::GetSysColorBrush(COLOR_3DFACE);
    }
    DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(StartupWindow, CWnd)
    ON_WM_CLOSE()
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

class Main final : public CFrameWnd
{
    CImage images[26];
    ButtonHover pointer;
    SkinButton check{pointer}, primary{pointer}, config{pointer}, minimize{pointer}, close{pointer};
    CStatic status, file, total, notice;
    CFont font, pauseFont;
    CBrush panel{RGB(18, 22, 29)};
    fs::path game;
    std::wstring news, message;
    Engine engine;
    EngineView view;
    enum class Phase { Splash, Connecting, Main, Closing };
    StartupWindow splash;
    Phase phase = Phase::Splash;
    bool inPrompt = false, pollingCheck = false;
    ULONGLONG splashSince = 0, connectedSince = 0;
    int showCommand = SW_SHOWNORMAL;
    int current = 0, overall = 0;
    static constexpr int barLeft = 14, barWidth = 463, barHeight = 9;
    static constexpr int fileBarY = 481, totalBarY = 513;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;

  public:
    Main(fs::path root, std::map<std::wstring, std::wstring> messages)
        : game(std::move(root)), engine(game, std::move(messages)) {}
    ~Main() override
    {
        *alive = false;
    }
    bool open()
    {
        auto cls =
            AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH),
                                ::LoadIconW(AfxGetInstanceHandle(), MAKEINTRESOURCE(1)));
        return CreateEx(WS_EX_APPWINDOW, cls, L"PBLauncher", WS_POPUP | WS_CLIPCHILDREN,
                        CRect(0, 0, 790, 531), nullptr, 0) != FALSE;
    }
    bool beginStartup(int command)
    {
        showCommand = command;
        if (!splash.open(m_hWnd, engine.messageText(L"INIT_WAIT_MSG", L"Por favor, aguarde.")))
            return false;
        splashSince = GetTickCount64();
        return SetTimer(1, 50, nullptr) != 0;
    }
    BOOL PreCreateWindow(CREATESTRUCT &cs) override
    {
        if (!CFrameWnd::PreCreateWindow(cs))
            return FALSE;
        cs.dwExStyle &= ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE);
        cs.style &= ~(WS_BORDER | WS_DLGFRAME | WS_THICKFRAME);
        return TRUE;
    }
    void loadButtons()
    {
        for (UINT i = 0; i < std::size(images); ++i)
            loadImage(images[i], 100 + i);
        auto make = [&](SkinButton &b, const wchar_t *label, CRect rect, UINT id, int n, int h, int d,
                        int off) {
            b.Create(label, WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, rect, this, id);
            b.skin(&images[n], &images[h], &images[d], &images[off]);
        };
        make(check, L"Check", {507, 450, 613, 526}, CheckId, 1, 2, 3, 4);
        make(primary, L"Update", {619, 450, 785, 526}, PrimaryId, 9, 10, 11, 12);
        make(config, L"Configurações", {709, 1, 734, 26}, SettingsId, 13, 14, 23, 13);
        make(minimize, L"Minimizar", {737, 1, 762, 26}, MinId, 15, 16, 24, 15);
        make(close, L"Fechar", {764, 1, 789, 26}, CloseId, 17, 18, 25, 17);
    }
    int OnCreate(LPCREATESTRUCT data)
    {
        if (CFrameWnd::OnCreate(data) == -1)
            return -1;
        try
        {
            loadButtons();
        }
        catch (...)
        {
            return -1;
        }
        font.CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft Tai Le");
        pauseFont.CreateFontW(-23, 0, 0, 0, FW_HEAVY, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
        primary.SetFont(&pauseFont);
        auto label = [&](CStatic &control, CRect bounds, UINT id) {
            control.Create(L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, bounds, this, id);
            control.SetFont(&font);
        };
        label(status, {17, 443, 477, 459}, StatusId);
        label(file, {17, 462, 477, 478}, FileId);
        label(total, {17, 493, 477, 509}, 220);
        notice.Create(L"", WS_CHILD | WS_CLIPCHILDREN | SS_CENTER | SS_CENTERIMAGE, {2, 26, 789, 442},
                      this, 221);
        notice.SetFont(&font);
        CenterWindow();
        check.EnableWindow(FALSE);
        primary.EnableWindow(FALSE);
        return 0;
    }
    void initWeb()
    {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        {
            notice.SetWindowTextW(L"WebView2 indisponível.");
            return;
        }
        fs::path profile = fs::path(raw) / L"PBLauncherMFC" / L"WebView2";
        CoTaskMemFree(raw);
        auto life = alive;
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, life](HRESULT hr, ICoreWebView2Environment *env) -> HRESULT {
                    if (!*life)
                        return S_OK;
                    if (FAILED(hr) || !env)
                    {
                        notice.SetWindowTextW(L"WebView2 indisponível.");
                        return S_OK;
                    }
                    HRESULT created = env->CreateCoreWebView2Controller(
                        notice.GetSafeHwnd(),
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this, life](HRESULT hr, ICoreWebView2Controller *ctl) -> HRESULT {
                                if (!*life)
                                    return S_OK;
                                if (FAILED(hr) || !ctl)
                                {
                                    notice.SetWindowTextW(L"WebView2 indisponível.");
                                    return S_OK;
                                }
                                controller = ctl;
                                ctl->put_IsVisible(FALSE);
                                ctl->get_CoreWebView2(&web);
                                if (!web)
                                {
                                    notice.SetWindowTextW(L"WebView2 indisponível.");
                                    return S_OK;
                                }
                                RECT rect{};
                                notice.GetClientRect(&rect);
                                ctl->put_Bounds(rect);
                                ComPtr<ICoreWebView2Controller2> colors;
                                if (SUCCEEDED(controller.As(&colors)))
                                    colors->put_DefaultBackgroundColor({255, 18, 22, 29});
                                ComPtr<ICoreWebView2Settings> options;
                                web->get_Settings(&options);
                                options->put_AreHostObjectsAllowed(FALSE);
                                options->put_IsWebMessageEnabled(FALSE);
                                options->put_AreDefaultContextMenusEnabled(FALSE);
                                options->put_AreDevToolsEnabled(FALSE);
                                options->put_IsBuiltInErrorPageEnabled(FALSE);
                                EventRegistrationToken token{};
                                web->add_NewWindowRequested(
                                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                        [this, life](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args)
                                            -> HRESULT {
                                            args->put_Handled(TRUE);
                                            BOOL clicked = FALSE;
                                            args->get_IsUserInitiated(&clicked);
                                            if (!*life || !clicked)
                                                return S_OK;
                                            LPWSTR rawUrl = nullptr;
                                            args->get_Uri(&rawUrl);
                                            std::wstring url = rawUrl ? rawUrl : L"";
                                            CoTaskMemFree(rawUrl);
                                            if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0)
                                                ::ShellExecuteW(m_hWnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                web->add_NavigationStarting(
                                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [](ICoreWebView2 *,
                                           ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                            LPWSTR uri = nullptr;
                                            args->get_Uri(&uri);
                                            std::wstring url = uri ? uri : L"";
                                            CoTaskMemFree(uri);
                                            if (url.rfind(L"https://", 0) && url.rfind(L"http://", 0) &&
                                                url != L"about:blank")
                                                args->put_Cancel(TRUE);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                web->add_ContentLoading(
                                    Callback<ICoreWebView2ContentLoadingEventHandler>(
                                        [this, life](ICoreWebView2 *, ICoreWebView2ContentLoadingEventArgs *args) -> HRESULT {
                                            BOOL errorPage = TRUE;
                                            if (*life && SUCCEEDED(args->get_IsErrorPage(&errorPage)) && !errorPage)
                                                controller->put_IsVisible(TRUE);
                                            return S_OK;
                                        }).Get(), &token);
                                web->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this,
                                         life](ICoreWebView2 *,
                                               ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                            if (!*life)
                                                return S_OK;
                                            BOOL ok = FALSE;
                                            args->get_IsSuccess(&ok);
                                            notice.SetWindowTextW(ok ? L"" : L"Notícias indisponíveis.");
                                            controller->put_IsVisible(ok);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                if (FAILED(web->Navigate(news.c_str())))
                                    notice.SetWindowTextW(L"Notícias indisponíveis.");
                                return S_OK;
                            })
                            .Get());
                    if (FAILED(created))
                        notice.SetWindowTextW(L"WebView2 indisponível.");
                    return S_OK;
                })
                .Get());
        if (FAILED(hr))
            notice.SetWindowTextW(L"Microsoft Edge WebView2 Runtime indisponível.");
    }
    void OnTimer(UINT_PTR timer)
    {
        if (timer != 1)
        {
            CFrameWnd::OnTimer(timer);
            return;
        }
        if (inPrompt)
            return;
        const auto now = GetTickCount64();
        if (phase == Phase::Splash)
        {
            phase = Phase::Connecting;
            try
            {
                engine.start();
            }
            catch (const std::exception &e)
            {
                KillTimer(1);
                inPrompt = true;
                splash.MessageBoxW(wide(e.what()).c_str(), L"PBLauncher", MB_OK);
                inPrompt = false;
                DestroyWindow();
            }
            return;
        }
        const bool finished = engine.finished();
        auto next = engine.poll();
        if (pollingCheck && (next.check || next.primaryEnabled || !next.error.empty()))
        {
            pollingCheck = false;
            SetTimer(1, 50, nullptr);
        }
        if (!next.error.empty() && phase != Phase::Closing)
        {
            auto *owner = splash.GetSafeHwnd() ? static_cast<CWnd *>(&splash) : this;
            inPrompt = true;
            owner->MessageBoxW(next.error.c_str(), L"PBLauncher", MB_OK);
            inPrompt = false;
        }
        if (finished)
        {
            KillTimer(1);
            DestroyWindow();
            return;
        }
        if (phase == Phase::Closing)
            return;
        if (phase == Phase::Connecting)
        {
            if (!next.ready)
            {
                connectedSince = 0;
                return;
            }
            if (!connectedSince)
            {
                connectedSince = now;
                if (!next.hideNews)
                {
                    notice.ShowWindow(SW_SHOWNOACTIVATE);
                    news = next.newsUrl;
                    if (!news.empty())
                        initWeb();
                }
            }
            if (now - splashSince < 1200 || now - connectedSince < 500)
                return;
        }
        auto command = next.primary;
        const wchar_t *caption = command == EngineCommand::Start   ? L"Start"
                                 : command == EngineCommand::Pause ? next.pauseLabel.c_str()
                                                                   : L"Update";
        if (command != view.primary || next.ready != view.ready ||
            (command == EngineCommand::Pause && next.pauseLabel != view.pauseLabel))
        {
            int i = command == EngineCommand::Start ? 5 : command == EngineCommand::Pause ? 19 : 9;
            primary.SetWindowTextW(caption);
            primary.skin(&images[i], &images[i + 1], &images[i + 2], &images[i + 3], command == EngineCommand::Pause);
        }
        if ((check.IsWindowEnabled() != FALSE) != next.check)
            check.EnableWindow(next.check);
        if ((primary.IsWindowEnabled() != FALSE) != next.primaryEnabled)
            primary.EnableWindow(next.primaryEnabled);
        if (next.status != message && !next.status.empty())
        {
            message = next.status;
            status.SetWindowTextW(message.c_str());
            status.Invalidate(FALSE);
        }
        if (next.title != view.title)
            SetWindowTextW(next.title.c_str());
        if (next.totalLabel != view.totalLabel)
        {
            total.SetWindowTextW(next.totalLabel.c_str());
            total.Invalidate(FALSE);
        }
        if (next.file != view.file || next.fileLabel != view.fileLabel)
        {
            file.SetWindowTextW(next.file.empty() ? next.fileLabel.c_str() : (next.fileLabel + L" " + next.file).c_str());
            file.Invalidate(FALSE);
        }
        updateBar(current, next.fileProgress, fileBarY);
        updateBar(overall, next.totalProgress, totalBarY);
        view = std::move(next);
        if (phase == Phase::Connecting)
        {
            phase = Phase::Main;
            splash.DestroyWindow();
            ShowWindow(showCommand);
            UpdateWindow();
        }
    }
    void OnPrimary()
    {
        engine.click(view.primary);
    }
    void OnCheck()
    {
        engine.click(EngineCommand::Check);
        pollingCheck = true;
        SetTimer(1, USER_TIMER_MINIMUM, nullptr);
    }
    void OnConfig()
    {
        auto exe = game / L"PBConfig.exe";
        if (!fs::is_regular_file(exe) ||
            reinterpret_cast<INT_PTR>(
                ::ShellExecuteW(m_hWnd, L"open", exe.c_str(), nullptr, game.c_str(), SW_SHOWNORMAL)) <= 32)
            MessageBoxW(L"PBConfig.exe não encontrado.", L"Erro");
    }
    void OnMin()
    {
        ShowWindow(SW_MINIMIZE);
    }
    void OnSize(UINT type, int cx, int cy)
    {
        CFrameWnd::OnSize(type, cx, cy);
        pointer.sync(m_hWnd);
    }
    void OnEnable(BOOL enabled)
    {
        CFrameWnd::OnEnable(enabled);
        pointer.sync(m_hWnd);
    }
    void OnClose()
    {
        if (inPrompt || phase == Phase::Closing)
            return;
        CWnd *owner = splash.GetSafeHwnd() ? static_cast<CWnd *>(&splash) : this;
        inPrompt = true;
        const auto question = engine.messageText(L"QUESTION_EXIT", L"Tem certeza de que deseja sair?");
        const int answer = owner->MessageBoxW(question.c_str(), L"PBLauncher", MB_YESNO);
        inPrompt = false;
        if (answer != IDYES)
            return;
        phase = Phase::Closing;
        if (!engine.active())
        {
            DestroyWindow();
            return;
        }
        owner->ShowWindow(SW_HIDE);
        engine.close();
    }
    void OnDestroy()
    {
        *alive = false;
        KillTimer(1);
        if (splash.GetSafeHwnd())
            splash.DestroyWindow();
        if (controller)
            controller->Close();
        web.Reset();
        controller.Reset();
        CFrameWnd::OnDestroy();
    }
    void updateBar(int &width, const ProgressRange &range, int y)
    {
        const auto position = (std::min)(range.position, range.maximum);
        const float fraction = range.maximum ? static_cast<float>(position) / static_cast<float>(range.maximum) : 0.0f;
        const int next = static_cast<int>(fraction * barWidth);
        if (width == next)
            return;
        InvalidateRect(CRect(barLeft + (std::min)(width, next), y,
                             barLeft + (std::max)(width, next), y + barHeight), FALSE);
        width = next;
    }
    void OnPaint()
    {
        CPaintDC dc(this);
        auto bar = [&](int y, int width) {
            CRect fill(barLeft, y, barLeft + width, y + barHeight);
            if (width && dc.RectVisible(fill))
            {
                dc.FillSolidRect(fill, RGB(111, 172, 240));
                dc.ExcludeClipRect(fill);
            }
        };
        bar(fileBarY, current);
        bar(totalBarY, overall);
        CRect clip;
        if (dc.GetClipBox(clip) != NULLREGION && !images[0].IsNull())
            images[0].Draw(dc, clip, clip);
    }
    BOOL OnEraseBkgnd(CDC *)
    {
        return TRUE;
    }
    HBRUSH OnCtlColor(CDC *dc, CWnd *w, UINT kind)
    {
        if (kind == CTLCOLOR_STATIC)
        {
            dc->SetTextColor(RGB(255, 255, 255));
            dc->SetBkColor(RGB(18, 22, 29));
            dc->SetBkMode(OPAQUE);
            return panel;
        }
        return CFrameWnd::OnCtlColor(dc, w, kind);
    }
    void OnLButtonDown(UINT f, CPoint pt)
    {
        if (pt.y < 26)
            SendMessage(WM_NCLBUTTONDOWN, HTCAPTION, 0);
        else
            CFrameWnd::OnLButtonDown(f, pt);
    }
    void OnMouseMove(UINT flags, CPoint point)
    {
        pointer.set(nullptr);
        CFrameWnd::OnMouseMove(flags, point);
    }
    BOOL PreTranslateMessage(MSG *msg) override
    {
        if (msg->message == WM_KEYDOWN && (msg->wParam == VK_TAB || msg->wParam == VK_RETURN) &&
            ::IsDialogMessageW(m_hWnd, msg))
            return TRUE;
        return CFrameWnd::PreTranslateMessage(msg);
    }
    DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(Main, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_CTLCOLOR()
    ON_WM_TIMER()
    ON_WM_ENABLE()
    ON_WM_SIZE()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONDOWN()
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_COMMAND(CheckId, OnCheck)
    ON_COMMAND(PrimaryId, OnPrimary)
    ON_COMMAND(SettingsId, OnConfig)
    ON_COMMAND(MinId, OnMin)
    ON_COMMAND(CloseId, OnClose)
END_MESSAGE_MAP()

class LauncherApp : public CWinApp
{
    HANDLE instance = nullptr, updater = nullptr;

    static HANDLE acquire(const wchar_t *name)
    {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
        if (!mutex) return nullptr;
        DWORD result = WaitForSingleObject(mutex, 1);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return mutex;
        CloseHandle(mutex);
        return nullptr;
    }
    static void release(HANDLE mutex)
    {
        if (mutex)
        {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }

    int DoMessageBox(LPCTSTR text, UINT flags, UINT) override
    {
        return ::MessageBoxW(::GetActiveWindow(), text, L"PBLauncher", flags & ~MB_ICONMASK);
    }

    BOOL InitInstance() override
    {
        CWinApp::InitInstance();
        instance = CreateMutexW(nullptr, FALSE, L"Local\\Zepetto.LauncherMFC.UI");
        bool duplicate = !instance || GetLastError() == ERROR_ALREADY_EXISTS;
        if (!duplicate)
        {
            HANDLE game = acquire(L"PointBlank");
            if (game)
            {
                release(game);
                updater = acquire(L"Zepetto Updater");
            }
            duplicate = !updater;
        }
        if (!AfxOleInit())
            return FALSE;
        try
        {
            auto dir = appFolder();
            std::map<std::wstring, std::wstring> messages;
            try
            {
                messages = loadTexts(dir / L"Messages.xml", L"message");
            }
            catch (const std::runtime_error &)
            {
                // The updater can restore a missing or damaged catalog after connecting.
            }
            if (duplicate)
            {
                auto detail = localizedText(messages, L"DUPLICATE_EXE", L"Não é possível abrir dois programas ao mesmo tempo.");
                ::MessageBoxW(nullptr, detail.c_str(), L"PBLauncher", MB_OK);
                return FALSE;
            }
            if (wcsstr(m_lpCmdLine, L"/check_string"))
            {
                for (size_t i = 0; i < std::size(messageKeys); ++i)
                {
                    auto found = messages.find(messageKeys[i]);
                    if (found == messages.end())
                        throw std::runtime_error("Mensagem ausente: " + utf8(messageKeys[i]));
                    auto body = L"Verificação das mensagens (" + std::to_wstring(i) + L"/47)\n\n" +
                                found->second + L"\n\nClique em Sim para continuar ou em Não para sair.";
                    if (::MessageBoxW(nullptr, body.c_str(), L"PBLauncher", MB_YESNO) == IDNO) break;
                }
                return FALSE;
            }
            auto main = new Main(dir, std::move(messages));
            if (!main->open())
                return FALSE;
            m_pMainWnd = main;
            if (!main->beginStartup(m_nCmdShow))
            {
                main->DestroyWindow();
                return FALSE;
            }
            return TRUE;
        }
        catch (const std::exception &e)
        {
            ::MessageBoxW(nullptr, wide(e.what()).c_str(), L"PBLauncher", MB_OK);
            return FALSE;
        }
    }
    int ExitInstance() override
    {
        release(updater);
        if (instance)
            CloseHandle(instance);
        return CWinApp::ExitInstance();
    }
} app;
