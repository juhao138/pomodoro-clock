//
//  App.swift —— 窗口、交互、计时循环
//

import AppKit
import QuartzCore

struct Options {
    var fullscreen = true
    var mute = false
    var opacity = 0
    var work = 25, short = 5, long = 15
    var listEvery = 30, listDur = 8
    var todoFile: String? = nil
    var bgImage: String? = nil
    var theme: ThemeKind = .light
    var selftest = false
    var help = false
    var testChime = false
    var snapshot: String? = nil
    var snapshotDark = false
    var panelSnapshot = false
    var uicheck = false
    var exitAfter: Double = 0
    var selfShot: String? = nil
    var toggleFS = false
    var dataDir: String? = nil
    var demo = false
    var size = CGSize(width: 1600, height: 1000)
}

final class BorderlessWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

final class ClockView: NSView {
    weak var ctl: AppController?
    private var tracking: NSTrackingArea?

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override var needsPanelToBecomeKey: Bool { false }

    override func draw(_ dirtyRect: NSRect) {
        ctl?.paint(size: bounds.size)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = tracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
                               options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }

    override func mouseMoved(with event: NSEvent) {
        ctl?.mouseMoved(convert(event.locationInWindow, from: nil))
    }

    override func mouseExited(with event: NSEvent) {
        ctl?.mouseMoved(convert(event.locationInWindow, from: nil), leaving: true)
    }

    override func mouseDown(with event: NSEvent) {
        ctl?.mouseDown(convert(event.locationInWindow, from: nil))
    }

    override func mouseUp(with event: NSEvent) {
        ctl?.mouseUp(convert(event.locationInWindow, from: nil))
    }

    override func scrollWheel(with event: NSEvent) {
        ctl?.scroll(delta: event.scrollingDeltaY)
    }

    override func keyDown(with event: NSEvent) {
        if ctl?.key(event) == true { return }
        super.keyDown(with: event)
    }

    override func accessibilityLabel() -> String? { "桌面番茄钟" }

    override func accessibilityValue() -> Any? {
        guard let c = ctl else { return nil }
        return "\(c.scene.pomo.phase.name) \(fmtSeconds(c.scene.pomo.remaining))，\(c.scene.pomo.running ? "运行中" : "已暂停")，待办 \(c.scene.store.items.count) 条"
    }

    override func cancelOperation(_ sender: Any?) {
        ctl?.escape()
    }
}

final class AppController: NSObject, NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate {
    let scene = Scene()
    let opts: Options

    private var window: NSWindow!
    private var view: ClockView!
    private var timer: Timer?
    private var lastTick: CFTimeInterval = CACurrentMediaTime()
    private var lastSecond = ""
    private var field: NSTextField?
    private var fullscreen = true
    private var pressedBtn = -1
    private var pressedRow = -1
    private var panelPress: PanelHit = .none

    init(options: Options) {
        self.opts = options
        super.init()
        if let d = options.dataDir { AppSettings.overrideDir = d }
        scene.settings.load()
        if !scene.settings.exists { scene.settings.save() }
        scene.settings.listEvery = options.listEvery
        scene.settings.listDur = options.listDur
        scene.settings.theme = options.theme
        scene.settings.muted = options.mute
        scene.settings.opacityIndex = options.opacity
        if let b = options.bgImage { scene.settings.bgImage = b }
        scene.theme = scene.settings.theme
        scene.pomo.workMin = options.work
        scene.pomo.shortMin = options.short
        scene.pomo.longMin = options.long
        scene.pomo.total = scene.pomo.seconds(.work)
        scene.pomo.remaining = scene.pomo.total

        if let f = options.todoFile {
            scene.store.path = f
        } else {
            scene.store.path = AppSettings.supportDir() + "/todo.txt"
        }
        scene.store.load()
        scene.listClock = Double(scene.settings.listDur)
        loadWallpaper()
        fullscreen = options.fullscreen
    }

    // MARK: 生命周期

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()
        let screen = NSScreen.main?.frame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)
        let frame = fullscreen ? screen : CGRect(x: 0, y: 0, width: 1120, height: 720)

        window = BorderlessWindow(contentRect: frame,
                                  styleMask: fullscreen ? [.borderless, .resizable] : [.titled, .closable, .resizable, .miniaturizable],
                                  backing: .buffered, defer: false)
        window.title = "桌面番茄钟"
        window.isReleasedWhenClosed = false
        window.backgroundColor = NSColor.windowBackgroundColor
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        window.delegate = self

        window.contentMinSize = NSSize(width: 900, height: 600)
        view = ClockView(frame: CGRect(origin: .zero, size: frame.size))
        view.ctl = self
        view.wantsLayer = true
        window.contentView = view
        window.setFrame(frame, display: true)
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        if !fullscreen { window.center() }
        applyPresentation()

        NSApp.activate(ignoringOtherApps: true)
        applyOpacity()
        updateTitle()

        print("窗口已就绪 \(Int(view.bounds.width))×\(Int(view.bounds.height)) 全屏=\(fullscreen)")
        if opts.panelSnapshot { openSettings() }
        if let shot = opts.selfShot {
            if opts.toggleFS {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.toggleFullscreen() }
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.9) { [weak self] in
                self?.captureView(shot)
                NSApp.terminate(nil)
            }
        }
        if opts.exitAfter > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + opts.exitAfter) {
                print("按时退出")
                NSApp.terminate(nil)
            }
        }

        let t = Timer(timeInterval: 1.0 / 60.0, target: self, selector: #selector(tick), userInfo: nil, repeats: true)
        t.tolerance = 0.004
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func windowWillClose(_ notification: Notification) {
        NSApp.terminate(nil)
    }

    // MARK: 菜单

    private func buildMenu() {
        let main = NSMenu()

        let appItem = NSMenuItem()
        main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "关于 桌面番茄钟",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        let set = NSMenuItem(title: "设置…", action: #selector(menuSettings), keyEquivalent: ",")
        set.target = self
        appMenu.addItem(set)
        let dark = NSMenuItem(title: "切换深浅", action: #selector(menuTheme), keyEquivalent: "d")
        dark.target = self
        appMenu.addItem(dark)
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "隐藏 桌面番茄钟", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let quit = NSMenuItem(title: "退出 桌面番茄钟", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appMenu.addItem(quit)
        appItem.submenu = appMenu

        let editItem = NSMenuItem()
        main.addItem(editItem)
        let edit = NSMenu(title: "编辑")
        edit.addItem(withTitle: "撤销", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "重做", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "剪切", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit

        let winItem = NSMenuItem()
        main.addItem(winItem)
        let win = NSMenu(title: "窗口")
        let mini = NSMenuItem(title: "窗口 / 全屏", action: #selector(menuFullscreen), keyEquivalent: "\u{F70F}")
        mini.keyEquivalentModifierMask = []
        mini.target = self
        win.addItem(mini)
        win.addItem(withTitle: "最小化", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        winItem.submenu = win

        NSApp.mainMenu = main
    }

    @objc private func menuSettings() { openSettings() }
    @objc private func menuTheme() { toggleTheme() }
    @objc private func menuFullscreen() { toggleFullscreen() }

    // MARK: 循环

    @objc private func tick() {
        let now = CACurrentMediaTime()
        var dt = now - lastTick
        lastTick = now
        if dt > 0.5 { dt = 0.5 }
        if dt < 0 { dt = 0 }

        scene.now = Date()

        if scene.pomo.tick(dt) {
            Chime.shared.play(muted: scene.settings.muted)
        }

        // 待办轮播
        let target: Double
        if scene.pinned {
            target = 1
        } else if scene.settings.listEvery > 0 {
            scene.listClock += dt
            let t = scene.listClock.truncatingRemainder(dividingBy: Double(scene.settings.listEvery))
            target = t < Double(scene.settings.listDur) ? 1 : 0
        } else {
            target = 0
        }
        var moving = false
        let reduceMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        let prevAlpha = scene.listAlpha
        let speed = dt / 0.34
        if scene.listAlpha < target { scene.listAlpha = min(target, scene.listAlpha + speed) }
        else if scene.listAlpha > target { scene.listAlpha = max(target, scene.listAlpha - speed) }
        if reduceMotion { scene.listAlpha = target }
        if abs(scene.listAlpha - prevAlpha) > 0.0001 { moving = true }

        // 面板
        let pTarget: Double = scene.panelOpen ? 1 : 0
        if reduceMotion {
            scene.panelT = pTarget
        } else if abs(scene.panelT - pTarget) > 0.0001 {
            let rate = scene.panelOpen ? dt / 0.28 : dt / 0.20
            scene.panelT += scene.panelOpen ? rate : -rate
            scene.panelT = max(0, min(1, scene.panelT))
            moving = true
        }

        // 按钮悬停擦入
        for i in 0..<scene.btnHover.count {
            let want: Double = (i == scene.hoverBtn) ? 1 : 0
            if reduceMotion {
                scene.btnHover[i] = want
            } else if abs(scene.btnHover[i] - want) > 0.005 {
                scene.btnHover[i] += (want - scene.btnHover[i]) * min(1, dt * 18)
                moving = true
            } else if scene.btnHover[i] != want {
                scene.btnHover[i] = want
                moving = true
            }
        }

        if scene.scroll > max(0, scene.store.items.count - 1) {
            scene.scroll = max(0, scene.store.items.count - 1)
            moving = true
        }

        let visRows = homeLayout(size: viewSize, scene: scene).visibleRows
        let maxScroll = max(0, scene.store.items.count - visRows)
        if scene.scroll > maxScroll {
            scene.scroll = maxScroll
            moving = true
        }

        syncField()

        let sec = clockString
        var need = moving
        if sec != lastSecond {
            lastSecond = sec
            need = true
        }
        if need { view.needsDisplay = true }
        updateTitle()
    }

    private var clockString: String {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f.string(from: scene.now)
    }

    private func updateTitle() {
        let t = "\(scene.pomo.phase.name) \(fmtSeconds(scene.pomo.remaining)) · \(scene.pomo.running ? "运行中" : "已暂停") · 完成 \(scene.pomo.completed) 次"
        if window.title != t { window.title = t }
    }

    // MARK: 自截图（用真实视图离屏缓存，含子视图与真实文字渲染）

    func captureView(_ path: String) {
        guard let v = view, let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) else {
            print("截图失败")
            return
        }
        v.cacheDisplay(in: v.bounds, to: rep)
        if let data = rep.representation(using: .png, properties: [:]) {
            try? data.write(to: URL(fileURLWithPath: path))
            print("已截图 → \(path)  \(rep.pixelsWide)×\(rep.pixelsHigh)")
        }
    }

    // MARK: 绘制

    func paint(size: CGSize) {
        let P = Palette.of(scene.theme)
        let L = homeLayout(size: size, scene: scene)
        drawHome(scene, L, P)
        if scene.panelT > 0.001 {
            let G = settingsGeometry(size: size, scene: scene)
            drawSettings(scene, G, P)
        }
    }

    // MARK: 命中测试

    private func homeHit(_ p: CGPoint, size: CGSize) -> HomeHit {
        homeHitTest(p, size: size, scene: scene)
    }

    private var viewSize: CGSize { view?.bounds.size ?? CGSize(width: 1600, height: 1000) }

    func mouseMoved(_ p: CGPoint, leaving: Bool = false) {
        guard let view = view else { return }
        if scene.panelOpen && scene.panelT > 0.4 {
            let G = settingsGeometry(size: viewSize, scene: scene)
            let hit = settingsHitTest(p, G)
            if hit != scene.panelHover {
                scene.panelHover = hit
                scene.hoverBtn = -1
                view.needsDisplay = true
            }
            if hit != .none { NSCursor.pointingHand.set() } else { NSCursor.arrow.set() }
            return
        }
        scene.panelHover = .none

        let hit = homeHit(p, size: viewSize)
        let wantBtn: Int
        let wantRow: Int
        let wantPin: Bool
        switch hit {
        case .button(let i): wantBtn = i; wantRow = -1; wantPin = false
        case .row(let i), .check(let i): wantBtn = -1; wantRow = i; wantPin = false
        case .pin: wantBtn = -1; wantRow = -1; wantPin = true
        case .dial, .none: wantBtn = -1; wantRow = -1; wantPin = false
        }
        if leaving {
            if scene.hoverBtn != -1 || scene.hoverRow != -1 || scene.hoverPin {
                scene.hoverBtn = -1
                scene.hoverRow = -1
                scene.hoverCheck = -1
                scene.hoverPin = false
                view.needsDisplay = true
            }
            NSCursor.arrow.set()
            return
        }
        var dirty = false
        if scene.hoverBtn != wantBtn { scene.hoverBtn = wantBtn; dirty = true }
        if scene.hoverRow != wantRow {
            scene.hoverRow = wantRow
            scene.hoverCheck = wantRow
            dirty = true
        }
        if scene.hoverPin != wantPin { scene.hoverPin = wantPin; dirty = true }
        if dirty { view.needsDisplay = true }
        switch hit {
        case .none: NSCursor.arrow.set()
        default: NSCursor.pointingHand.set()
        }
    }

    func mouseDown(_ p: CGPoint) {
        guard let view = view else { return }
        if scene.panelOpen && scene.panelT > 0.4 {
            let G = settingsGeometry(size: viewSize, scene: scene)
            panelPress = settingsHitTest(p, G)
            view.needsDisplay = true
            return
        }
        switch homeHit(p, size: viewSize) {
        case .button(let i):
            pressedBtn = i
            scene.pressBtn = i
        case .row(let i), .check(let i):
            pressedRow = i
        case .dial:
            pressedRow = -1
        case .pin, .none:
            break
        }
        view.needsDisplay = true
    }

    func mouseUp(_ p: CGPoint) {
        guard let view = view else { return }
        defer { view.needsDisplay = true }

        if scene.panelOpen && scene.panelT > 0.4 {
            let G = settingsGeometry(size: viewSize, scene: scene)
            let hit = settingsHitTest(p, G)
            if hit == panelPress {
                activatePanel(hit)
            } else if hit == .none, !G.panel.contains(p) {
                closeSettings()
            }
            panelPress = .none
            return
        }

        if pressedBtn >= 0 {
            let hit = homeHit(p, size: viewSize)
            if case .button(let i) = hit, i == pressedBtn { activateButton(i) }
        } else if pressedRow >= 0 {
            let hit = homeHit(p, size: viewSize)
            if case .row(let i) = hit, i == pressedRow { toggleTodo(i) }
            else if case .check(let i) = hit, i == pressedRow { toggleTodo(i) }
        } else {
            switch homeHit(p, size: viewSize) {
            case .dial: scene.pomo.running.toggle()
            case .pin: togglePin()
            default: break
            }
        }
        pressedBtn = -1
        pressedRow = -1
        scene.pressBtn = -1
    }

    func scroll(delta: CGFloat) {
        guard !scene.panelOpen else { return }
        guard scene.store.items.count > 0 else { return }
        let maxScroll = max(0, scene.store.items.count - 1)
        if delta < 0 { scene.scroll = min(maxScroll, scene.scroll + 1) }
        if delta > 0 { scene.scroll = max(0, scene.scroll - 1) }
        scene.pinned = true
        scene.listAlpha = 1
        view?.needsDisplay = true
    }

    // MARK: 动作

    private func activateButton(_ i: Int) {
        switch i {
        case 0: scene.pomo.running.toggle()
        case 1: scene.pomo.reset()
        case 2: scene.pomo.skip()
        case 3: togglePin()
        case 4: openSettings()
        case 5: NSApp.terminate(nil)
        default: break
        }
        updateTitle()
    }

    private func togglePin() {
        scene.pinned.toggle()
        if scene.pinned { scene.listClock = 0 } else { scene.listClock = Double(scene.settings.listDur) }
    }

    private func toggleTodo(_ i: Int) {
        scene.store.toggle(i)
        scene.store.save()
        scene.selected = i
    }

    func escape() {
        if scene.panelOpen { closeSettings() } else { NSApp.terminate(nil) }
    }

    func key(_ e: NSEvent) -> Bool {
        if scene.panelOpen {
            if e.keyCode == 53 { closeSettings(); return true }
            return false
        }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if mods.contains(.command) { return false }
        switch e.keyCode {
        case 49: scene.pomo.running.toggle(); return true                      // space
        case 15: scene.pomo.reset(); return true                               // r
        case 45: scene.pomo.skip(); return true                                // n
        case 37: togglePin(); return true                                      // l
        case 14: openSettings(); return true                                   // e
        case 17: cycleOpacity(); return true                                   // t
        case 2: toggleTheme(); return true                                     // d
        case 103: toggleFullscreen(); return true                              // F11
        case 126: moveSelection(-1); return true                               // up
        case 125: moveSelection(1); return true                                // down
        case 53: NSApp.terminate(nil); return true                             // esc
        default: return false
        }
    }

    private func moveSelection(_ d: Int) {
        guard !scene.store.items.isEmpty else { return }
        let n = scene.store.items.count
        scene.selected = (scene.selected + d + n) % n
        scene.pinned = true
        scene.listAlpha = 1
        // 让选中项留在可见窗口里
        let L = homeLayout(size: viewSize, scene: scene)
        if scene.selected < scene.scroll { scene.scroll = scene.selected }
        if scene.selected >= scene.scroll + L.visibleRows { scene.scroll = scene.selected - L.visibleRows + 1 }
        view?.needsDisplay = true
    }

    private func cycleOpacity() {
        let steps: [CGFloat] = [1.0, 0.91, 0.78, 0.66]
        scene.settings.opacityIndex = (scene.settings.opacityIndex + 1) % steps.count
        scene.settings.save()
        applyOpacity()
    }

    private func applyOpacity() {
        let steps: [CGFloat] = [1.0, 0.91, 0.78, 0.66]
        window.alphaValue = steps[max(0, min(steps.count - 1, scene.settings.opacityIndex))]
    }

    private func toggleTheme() {
        scene.theme = scene.theme.next
        scene.settings.theme = scene.theme
        scene.settings.save()
        syncField()
        view?.needsDisplay = true
    }

    private func applyPresentation() {
        if fullscreen {
            NSApp.presentationOptions = [.autoHideMenuBar, .autoHideDock]
        } else {
            NSApp.presentationOptions = []
        }
    }

    func toggleFullscreen() {
        guard let screen = NSScreen.main else { return }
        fullscreen.toggle()
        if fullscreen {
            window.styleMask = [.borderless, .resizable]
            window.setFrame(screen.frame, display: true)
            applyPresentation()
        } else {
            NSApp.presentationOptions = []
            window.styleMask = [.titled, .closable, .resizable, .miniaturizable]
            window.setContentSize(NSSize(width: 1120, height: 720))
            window.center()
            window.title = "桌面番茄钟"
        }
        updateTitle()
        view?.needsDisplay = true
    }

    // MARK: 设置面板

    func openSettings() {
        scene.panelOpen = true
        scene.editing = nil
        ensureField()
        field?.stringValue = ""
        field?.window?.makeFirstResponder(field)
        view?.needsDisplay = true
    }

    func closeSettings() {
        scene.panelOpen = false
        scene.editing = nil
        field?.isHidden = true
        field?.stringValue = ""
        window.makeFirstResponder(view)
        view?.needsDisplay = true
    }

    private func ensureField() {
        guard field == nil, let view = view else { return }
        let f = NSTextField(frame: .zero)
        f.isBezeled = false
        f.isBordered = false
        f.drawsBackground = false
        f.focusRingType = .none
        f.font = Typo.zh(14, .regular)
        f.placeholderAttributedString = NSAttributedString(
            string: "输入待办，回车添加",
            attributes: [.foregroundColor: NSColor.secondaryLabelColor,
                         .font: Typo.zh(13.5, .regular)])
        f.delegate = self
        f.target = self
        f.action = #selector(commitField)
        view.addSubview(f)
        field = f
    }

    private func syncField() {
        guard let f = field else { return }
        guard scene.panelOpen, scene.panelT > 0.35 else {
            f.isHidden = true
            return
        }
        f.isHidden = false
        let G = settingsGeometry(size: viewSize, scene: scene)
        let k = max(0.62, min(1.45, viewSize.height / 1080))
        f.frame = CGRect(x: G.input.minX + 10 * k, y: G.input.minY + 8 * k,
                         width: G.input.width - 20 * k, height: G.input.height - 16 * k)
        let P = Palette.of(scene.theme)
        f.textColor = P.ink
        f.font = Typo.zh(14 * k, .regular)
        f.placeholderAttributedString = NSAttributedString(
            string: scene.editing == nil ? "输入待办，回车添加" : "修改后回车保存",
            attributes: [.foregroundColor: P.ink2.alpha(0.8), .font: Typo.zh(13 * k, .regular)])
    }

    @objc private func commitField() {
        guard let text = field?.stringValue, !text.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        if let idx = scene.editing, scene.store.items.indices.contains(idx) {
            scene.store.update(idx, text)
        } else if let idx = scene.store.add(text) {
            scene.selected = idx
        }
        scene.store.save()
        scene.editing = nil
        field?.stringValue = ""
        if let f = field { window.makeFirstResponder(f) }
        view?.needsDisplay = true
    }

    private func activatePanel(_ hit: PanelHit) {
        switch hit {
        case .close, .done:
            closeSettings()
        case .add:
            commitField()
        case .delete(let i):
            scene.store.delete(i)
            scene.store.save()
            if scene.editing == i { scene.editing = nil; field?.stringValue = "" }
            if scene.selected >= scene.store.items.count { scene.selected = max(0, scene.store.items.count - 1) }
        case .row(let i):
            scene.editing = i
            field?.stringValue = scene.store.items[i].text
            if let f = field {
                window.makeFirstResponder(f)
                f.selectText(nil)
            }
        case .check(let i):
            scene.store.toggle(i)
            scene.store.save()
        case .theme(let i):
            scene.theme = (i == 0) ? .light : .dark
            scene.settings.theme = scene.theme
            scene.settings.save()
            syncField()
        case .sound(let i):
            scene.settings.muted = (i == 1)
            scene.settings.save()
        case .pick:
            pickImage()
        case .clear:
            scene.settings.bgImage = ""
            scene.wallpaper = nil
            scene.settings.save()
        case .none:
            break
        }
        view?.needsDisplay = true
    }

    private func pickImage() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.png, .jpeg, .bmp, .gif, .tiff, .heic]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.message = "选择一张背景图片"
        panel.prompt = "选择"
        if panel.runModal() == .OK, let url = panel.url {
            scene.settings.bgImage = url.path
            scene.settings.save()
            loadWallpaper()
        }
    }

    private func loadWallpaper() {
        let p = scene.settings.bgImage
        guard !p.isEmpty, let img = NSImage(contentsOfFile: p) else {
            scene.wallpaper = nil
            return
        }
        scene.wallpaper = img
    }

    // MARK: NSTextFieldDelegate

    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        if selector == #selector(NSResponder.cancelOperation(_:)) {
            closeSettings()
            return true
        }
        if selector == #selector(NSResponder.insertNewline(_:)) {
            commitField()
            return true
        }
        return false
    }
}
