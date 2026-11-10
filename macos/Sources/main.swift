//
//  main.swift —— 入口：命令行、自测、离线截图、启动窗口
//

import AppKit
import Foundation

func parseOptions() -> Options {
    var o = Options()
    let args = CommandLine.arguments
    var i = 1
    func next() -> String? {
        if i + 1 < args.count { i += 1; return args[i] }
        return nil
    }
    while i < args.count {
        let a = args[i]
        switch a {
        case "--window": o.fullscreen = false
        case "--mute": o.mute = true
        case "--selftest": o.selftest = true
        case "--help", "-h": o.help = true
        case "--test-chime": o.testChime = true
        case "--demo": o.demo = true
        case "--dark": o.snapshotDark = true
        case "--opacity":
            if let v = next(), let n = Int(v) { o.opacity = max(0, n) }
        case "--work":
            if let v = next(), let n = Int(v), n > 0, n <= 600 { o.work = n }
        case "--short":
            if let v = next(), let n = Int(v), n > 0, n <= 600 { o.short = n }
        case "--long":
            if let v = next(), let n = Int(v), n > 0, n <= 600 { o.long = n }
        case "--list-every":
            if let v = next(), let n = Int(v) { o.listEvery = n }
        case "--list-dur":
            if let v = next(), let n = Int(v) { o.listDur = n }
        case "--todo-file":
            if let v = next() { o.todoFile = v }
        case "--data-dir":
            if let v = next() { o.dataDir = v }
        case "--bg-image":
            if let v = next() { o.bgImage = v }
        case "--theme":
            if let v = next() { o.theme = (v == "dark") ? .dark : .light }
        case "--snapshot":
            if let v = next() { o.snapshot = v }
        case "--panel":
            o.panelSnapshot = true
        case "--uicheck":
            o.uicheck = true
        case "--exit-after":
            if let v = next(), let n = Double(v) { o.exitAfter = n }
        case "--self-shot":
            if let v = next() { o.selfShot = v }
        case "--toggle-fs":
            o.toggleFS = true
        case "--size":
            if let v = next() {
                let parts = v.lowercased().split(separator: "x")
                if parts.count == 2, let w = Double(parts[0]), let h = Double(parts[1]) {
                    o.size = CGSize(width: w, height: h)
                }
            }
        default:
            break
        }
        i += 1
    }
    return o
}

func printHelp() {
    print("""
    用法: 桌面番茄钟 [选项]

      --work N          专注分钟（默认 25）
      --short N         短休息分钟（默认 5）
      --long N          长休息分钟（默认 15）
      --list-every N    每隔 N 秒浮出待办（默认 30，0 = 不自动浮出）
      --list-dur N      每次浮出 M 秒（默认 8）
      --todo-file P     指定待办清单文件
      --data-dir P      把 todo.txt / settings.json 放到指定目录（便携模式）
      --bg-image P      指定背景图片
      --window          以 1120×720 窗口启动
      --opacity N       启动时的透明度档位（0 = 不透明）
      --theme light|dark  启动主题
      --mute            不响提示音
      --test-chime      试听提示音后退出
      --selftest        只跑自测，不开窗口
      --snapshot P      把界面渲染成 PNG 后退出（可加 --dark / --demo / --panel / --size 1600x1000）
      --uicheck         检查点击命中区域是否落在正确的部件上
      --exit-after N    启动 N 秒后自动退出（做冒烟测试用）

    按键: 空格 开始/暂停 · R 重置 · N 跳过 · L 待办常驻 · E 设置
          T 透明度 · D 深浅 · F11 窗口/全屏 · ↑↓ 选择 · Esc 退出
    """)
}

// MARK: - 自测

func runSelftest() -> Int32 {
    var fails = 0
    func check(_ cond: Bool, _ what: String) {
        print("  [\(cond ? "PASS" : "FAIL")] \(what)")
        if !cond { fails += 1 }
    }
    print("番茄钟 + 待办清单 自测")
    print("----------------")

    var p = Pomodoro()
    check(p.phase == .work, "初始阶段是专注")
    check(p.remaining == 25 * 60, "专注默认 25 分钟")
    p.tick(10)
    check(p.remaining == 25 * 60, "暂停时推进不扣时间")
    p.running = true
    p.tick(60)
    check(abs(p.remaining - 24 * 60) < 1e-6, "运行 60 秒后剩 24:00")
    p.tick(24 * 60)
    check(p.phase == .shortBreak && p.remaining == 300, "专注结束进短休 5 分钟")
    p.tick(300)
    check(p.phase == .work, "短休结束回专注")
    for k in 0..<3 {
        p.tick(1500)
        if k < 2 { p.tick(300) }
    }
    check(p.completed == 4 && p.phase == .longBreak, "第 4 个专注后长休")
    p.phase = .work
    p.remaining = 1
    p.running = true
    let crossed = p.tick(100)
    check(crossed && p.remaining == 300 - 99, "超时时间结转到下一阶段")
    p.running = false
    let before = p.remaining
    p.tick(30)
    check(p.remaining == before, "暂停后不扣时间")
    p.reset()
    check(p.remaining == 1500 && p.phase == .work, "重置回初始态")

    print("-- 待办清单 --")
    let store = TodoStore()
    check(store.add("写周报") == 0, "新增第 1 条")
    check(store.add("复现 bug") == 1, "新增第 2 条")
    check(store.add("看书") == 2, "新增第 3 条")
    check(store.items.count == 3, "共 3 条")
    check(store.add("") == nil, "拒绝新增空条目")

    store.toggle(1)
    check(store.items[1].done && store.doneCount == 1, "标记完成后计数为 1")
    store.toggle(1)
    check(!store.items[1].done && store.doneCount == 0, "取消完成")

    store.update(0, "写季度周报")
    check(store.items[0].text == "写季度周报", "编辑第 1 条")
    store.update(9, "x")
    check(store.items[0].text == "写季度周报", "越界编辑被忽略")

    store.delete(1)
    check(store.items.count == 2 && store.items[1].text == "看书", "删除第 2 条后顺序正确")
    store.toggle(1)
    check(store.doneCount == 1, "再次标记完成")

    let tmp = NSTemporaryDirectory() + "pomodoro_selftest_todo.txt"
    store.path = tmp
    try? FileManager.default.removeItem(atPath: tmp)
    let saved = store.save()
    print("    (save 返回 \(saved), 路径 \(tmp))")
    check(saved && FileManager.default.fileExists(atPath: tmp), "清单写盘成功")

    let snapshot = store.items
    let reloaded = TodoStore()
    reloaded.path = tmp
    reloaded.load()
    check(reloaded.items.count == snapshot.count, "读盘条目数一致")
    check(reloaded.items.first?.text == snapshot.first?.text, "读回第 1 条文本一致")
    check(reloaded.items.last?.done == snapshot.last?.done, "读回完成状态一致")
    try? FileManager.default.removeItem(atPath: tmp)

    print("----------------")
    print(fails == 0 ? "全部通过" : "失败（\(fails) 项）")
    return fails == 0 ? 0 : 1
}

// MARK: - 界面命中自测

func runUICheck() -> Int32 {
    var fails = 0
    func check(_ cond: Bool, _ what: String) {
        print("  [\(cond ? "PASS" : "FAIL")] \(what)")
        if !cond { fails += 1 }
    }

    let scene = Scene()
    scene.store.items = [
        .init(text: "写周报", done: true),
        .init(text: "回复邮件"),
        .init(text: "补错误提示"),
        .init(text: "读第 6 章"),
        .init(text: "准备演示稿"),
        .init(text: "第六条"),
        .init(text: "第七条")
    ]
    scene.pinned = true
    scene.listAlpha = 1
    scene.pomo.running = true
    scene.pomo.completed = 2

    let size = CGSize(width: 1920, height: 1080)
    let L = homeLayout(size: size, scene: scene)

    print("主屏命中")
    for (i, b) in L.buttons.enumerated() {
        let hit = homeHitTest(CGPoint(x: b.rect.midX, y: b.rect.midY), size: size, scene: scene)
        check(hit == .button(i), "按钮 \(i) \(b.label) 命中")
    }
    check(homeHitTest(L.dialCenter, size: size, scene: scene) == .dial, "圆盘中心 = 播放/暂停")
    for v in 0..<L.visibleRows {
        let row = rowRect(v, L)
        let hitRow = homeHitTest(CGPoint(x: row.midX + 60, y: row.midY), size: size, scene: scene)
        check(hitRow == .row(v), "第 \(v) 行正文命中")
        let cb = boardCheckRect(v, L)
        let hitCheck = homeHitTest(CGPoint(x: cb.midX, y: cb.midY), size: size, scene: scene)
        check(hitCheck == .check(v), "第 \(v) 行勾选框命中")
    }
    check(homeHitTest(CGPoint(x: 900, y: 1080 - 60), size: size, scene: scene) == .none, "空白处不命中")

    print("设置面板命中")
    scene.panelOpen = true
    scene.panelT = 1
    let G = settingsGeometry(size: size, scene: scene)
    func pHit(_ r: CGRect) -> PanelHit {
        settingsHitTest(CGPoint(x: r.midX, y: r.midY), G)
    }
    check(pHit(G.close) == .close, "关闭按钮")
    check(pHit(G.done) == .done, "完成按钮")
    check(pHit(G.add) == .add, "添加按钮")
    check(pHit(G.pick) == .pick, "选择图片")
    check(pHit(G.clear) == .clear, "清除图片")
    check(pHit(G.themeChips[0]) == .theme(0), "浅色")
    check(pHit(G.themeChips[1]) == .theme(1), "深色")
    check(pHit(G.soundChips[0]) == .sound(0), "提示音开")
    check(pHit(G.soundChips[1]) == .sound(1), "提示音关")
    check(pHit(G.checks[0]) == .check(0), "第一条勾选框")
    check(pHit(G.deleteButtons[0]) == .delete(0), "第一条删除")
    let rowCenter = CGPoint(x: G.taskRows[1].midX - 100, y: G.taskRows[1].midY)
    check(settingsHitTest(rowCenter, G) == .row(1), "第二条正文")

    print("----------------")
    print(fails == 0 ? "全部通过" : "失败（\(fails) 项）")
    return fails == 0 ? 0 : 1
}

// MARK: - 离线渲染

func renderSnapshot(_ scene: Scene, size: CGSize, to path: String) {
    let cs = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let ctx = CGContext(data: nil, width: Int(size.width), height: Int(size.height),
                              bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        print("无法创建位图上下文")
        return
    }
    ctx.translateBy(x: 0, y: size.height)
    ctx.scaleBy(x: 1, y: -1)
    let g = NSGraphicsContext(cgContext: ctx, flipped: true)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = g

    let P = Palette.of(scene.theme)
    let L = homeLayout(size: size, scene: scene)
    drawHome(scene, L, P)
    if scene.panelT > 0.001 {
        let G = settingsGeometry(size: size, scene: scene)
        drawSettings(scene, G, P)
    }

    NSGraphicsContext.restoreGraphicsState()

    guard let img = ctx.makeImage() else { return }
    let rep = NSBitmapImageRep(cgImage: img)
    guard let data = rep.representation(using: .png, properties: [:]) else { return }
    try? data.write(to: URL(fileURLWithPath: path))
    print("已渲染 \(Int(size.width))×\(Int(size.height)) → \(path)")
}

// MARK: - 入口

_ = NSApplication.shared
Typo.register()

let opts = parseOptions()

if opts.help {
    printHelp()
    exit(0)
}

if opts.selftest {
    exit(runSelftest())
}

if opts.uicheck {
    exit(runUICheck())
}

if opts.testChime {
    let (ms, peak, rms) = Chime.shared.stats()
    print("提示音: \(ms) ms, 22050 Hz, 峰值 \(Int(peak))% , RMS \(Int(rms))%")
    Chime.shared.play(muted: false)
    RunLoop.current.run(until: Date().addingTimeInterval(2.0))
    print("已播放")
    exit(0)
}

if let path = opts.snapshot {
    let scene = Scene()
    scene.theme = opts.snapshotDark ? .dark : opts.theme
    scene.settings.listEvery = opts.listEvery
    scene.pomo.workMin = opts.work
    scene.pomo.longMin = opts.long
    scene.pomo.shortMin = opts.short
    if opts.demo {
        scene.store.items = [
            .init(text: "写周报：整理这周的 3 个结论", done: true),
            .init(text: "回复客户关于交付日期的邮件"),
            .init(text: "把登录页的错误提示补上"),
            .init(text: "读《重构》第 6 章，做一页笔记"),
            .init(text: "给下周一的评审准备演示稿")
        ]
        scene.pomo.running = true
        scene.pomo.completed = 2
        scene.pomo.remaining = 18 * 60 + 42
        scene.pinned = true
        scene.listAlpha = 1
        scene.selected = 1
        scene.panelT = opts.panelSnapshot ? 1 : 0
        scene.panelOpen = opts.panelSnapshot
    }
    scene.now = Date()
    renderSnapshot(scene, size: opts.size, to: path)
    exit(0)
}

let delegate = AppController(options: opts)
NSApp.delegate = delegate
NSApp.setActivationPolicy(.regular)
NSApp.run()
