//
//  Home.swift —— 主屏：时钟 / 本轮进度 / 待办看板 / 底部动作条
//
//  版式：上半幅是横贯整个窗口的时钟（主视觉），下半幅是一台仪表盘：
//  左边分段圆盘和本轮读数，右边是待办看板。
//

import AppKit
import Foundation

enum HomeHit: Equatable {
    case none
    case button(Int)
    case dial
    case row(Int)
    case check(Int)
    case pin
}

final class Scene {
    var theme: ThemeKind = .light
    var pomo = Pomodoro()
    var store = TodoStore()
    var settings = AppSettings()

    var listAlpha: Double = 0
    var pinned = false
    var selected = 0
    var scroll = 0
    var listClock: Double = 0

    var hoverBtn = -1
    var pressBtn = -1
    var btnHover: [Double] = Array(repeating: 0, count: 8)
    var hoverRow = -1
    var hoverCheck = -1
    var hoverPin = false

    var panelOpen = false
    var panelT: Double = 0
    var panelHover: PanelHit = .none
    var editing: Int? = nil

    var wallpaper: NSImage?
    var now = Date()
}

private let fmtDate: DateFormatter = {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "yyyy.MM.dd"
    return f
}()

private let fmtWeekday: DateFormatter = {
    let f = DateFormatter()
    f.locale = Locale(identifier: "zh_CN")
    f.dateFormat = "EEEE"
    return f
}()

private let fmtClock: DateFormatter = {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "HH:mm:ss"
    return f
}()

func isoWeek(_ d: Date) -> Int {
    var cal = Calendar(identifier: .iso8601)
    cal.timeZone = .current
    return cal.component(.weekOfYear, from: d)
}

func timezoneLabel() -> String {
    let seconds = TimeZone.current.secondsFromGMT()
    let h = seconds / 3600
    let m = abs(seconds % 3600) / 60
    return String(format: "UTC%+03d:%02d", h, m)
}

// MARK: - 版式

struct HomeLayout {
    var w: CGFloat = 0
    var h: CGFloat = 0
    var k: CGFloat = 1
    var pad: CGFloat = 48
    var content: CGRect = .zero
    var strip: CGRect = .zero
    var bar: CGRect = .zero
    var bottomRuleY: CGFloat = 0
    var buttons: [ButtonSpec] = []

    var eyebrowBaseline: CGFloat = 0
    var titleBaseline: CGFloat = 0
    var clockSize: CGFloat = 200
    var clockBaseline: CGFloat = 0
    var dateBaseline: CGFloat = 0
    var ruleY: CGFloat = 0

    var deck: CGRect = .zero
    var dialCenter: CGPoint = .zero
    var dialRadius: CGFloat = 100
    var group2X: CGFloat = 0
    var group3X: CGFloat = 0
    var showGroup3 = true

    var board: CGRect = .zero
    var pinButton: CGRect = .zero
    var rowH: CGFloat = 56
    var headerH: CGFloat = 92
    var footerH: CGFloat = 44
    var visibleRows: Int = 5
}

func homeLayout(size: CGSize, scene: Scene) -> HomeLayout {
    var L = HomeLayout()
    let w = size.width, h = size.height
    let k = max(0.60, min(1.45, h / 1080))
    let pad = (48 * k).rounded()
    L.w = w
    L.h = h
    L.k = k
    L.pad = pad

    let stripH: CGFloat = 3
    let barH = (54 * k).rounded()
    let bottomH = (92 * k).rounded()
    L.strip = CGRect(x: 0, y: 0, width: w, height: stripH)
    L.bar = CGRect(x: 0, y: stripH, width: w, height: barH)
    L.content = CGRect(x: pad, y: stripH + barH, width: w - pad * 2,
                       height: h - bottomH - stripH - barH)
    L.bottomRuleY = h - bottomH

    // 上半幅：眉标 → 标题 → 时钟 → 日期
    L.eyebrowBaseline = L.content.minY + 42 * k
    L.titleBaseline = L.eyebrowBaseline + 40 * k
    L.clockSize = max(72, min(300 * k, L.content.width / 4.71))
    L.clockBaseline = L.titleBaseline + 24 * k + L.clockSize * 0.79
    L.dateBaseline = L.clockBaseline + 56 * k
    L.ruleY = L.dateBaseline + 30 * k

    // 下半幅：仪表盘
    L.deck = CGRect(x: pad, y: L.ruleY, width: L.content.width, height: L.content.maxY - L.ruleY)
    L.dialRadius = max(48, min(130 * k, L.deck.height * 0.30))
    L.dialCenter = CGPoint(x: pad + L.dialRadius, y: L.deck.midY)
    L.group2X = pad + L.dialRadius * 2 + 60 * k
    L.group3X = L.group2X + 360 * k

    // 待办看板：占满右侧一列
    let boardW = min(700 * k, max(340, L.content.width * 0.38))
    let boardY = L.deck.minY + 22 * k
    L.board = CGRect(x: w - pad - boardW, y: boardY, width: boardW, height: L.content.maxY - boardY)
    L.showGroup3 = (L.board.minX - L.group3X) > 250 * k
    L.pinButton = CGRect(x: L.board.minX + 26 * k, y: L.board.maxY - 34 * k,
                         width: 64 * k, height: 26 * k)

    L.headerH = 92 * k
    L.footerH = 46 * k
    let room = L.board.height - L.headerH - L.footerH
    let n = max(scene.store.items.count, 1)
    let maxRows = max(1, Int(room / (52 * k)))
    L.visibleRows = min(n, maxRows)
    // 行高按可用高度撑开，最多 68
    L.rowH = min(68 * k, max(52 * k, room / CGFloat(L.visibleRows)))

    // 底部动作条
    let bh = (42 * k).rounded()
    let by = L.bottomRuleY + 26 * k
    let widths: [CGFloat] = [132, 96, 96, 116, 96, 84]
    var x = pad
    var specs: [ButtonSpec] = []
    let labels = buttonLabels(scene)
    for i in 0..<6 {
        let bw = (widths[i] * k).rounded()
        specs.append(ButtonSpec(label: labels[i],
                                rect: CGRect(x: x, y: by, width: bw, height: bh),
                                primary: i == 0,
                                toggled: i == 3 && scene.pinned,
                                cut: 8 * k))
        x += bw + (i == 0 ? 20 * k : 14 * k)
    }
    L.buttons = specs
    return L
}

func buttonLabels(_ s: Scene) -> [String] {
    ["\(s.pomo.running ? "暂停" : "开始")", "重置", "跳过",
     s.pinned ? "收起待办" : "待办", "设置", "退出"]
}

// MARK: - 绘制

func drawHome(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    // 纸面
    fillRect(CGRect(x: 0, y: 0, width: L.w, height: L.h), P.paper)

    // 自定义背景图：压得很淡，只留一点温度
    if let img = s.wallpaper {
        drawWallpaper(img, in: CGRect(x: 0, y: 0, width: L.w, height: L.h), alpha: P.isDark ? 0.16 : 0.12)
    }

    // 等高线：只在右上角一小片
    contourMarks(center: CGPoint(x: L.w - L.pad + 26, y: L.content.minY + 40),
                 radii: [96, 138, 180], from: 96, to: 186,
                 color: P.ink, alpha: 0.10)
    contourMarks(center: CGPoint(x: L.w - L.pad + 26, y: L.content.minY + 40),
                 radii: [222, 264], from: 120, to: 158,
                 color: P.ink, alpha: 0.07)

    drawCommandBar(s, L, P)
    drawClockBanner(s, L, P)
    drawDeck(s, L, P)
    drawBoard(s, L, P)
    drawActionBar(s, L, P)
}

private func drawWallpaper(_ img: NSImage, in rect: CGRect, alpha: CGFloat) {
    let size = img.size
    guard size.width > 0, size.height > 0 else { return }
    let scale = max(rect.width / size.width, rect.height / size.height)
    let dw = size.width * scale, dh = size.height * scale
    let dr = CGRect(x: rect.midX - dw / 2, y: rect.midY - dh / 2, width: dw, height: dh)
    NSGraphicsContext.saveGraphicsState()
    NSBezierPath(rect: rect).addClip()
    img.draw(in: dr, from: .zero, operation: .sourceOver, fraction: alpha)
    NSGraphicsContext.restoreGraphicsState()
}

// MARK: 命令栏

private func drawCommandBar(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    let k = L.k
    fillRect(L.bar, P.cmdBar)
    if P.isDark { hRule(0, L.w, L.bar.maxY - 1, P.rule.alpha(0.6)) }
    triStrip(L.strip, middle: P.signal)

    let mid = L.bar.midY
    let base = centerBaseline(rect: L.bar, font: Typo.zh(14 * k, .semibold))

    // 左：品牌
    fillRect(CGRect(x: L.pad, y: mid - 4.5 * k, width: 9 * k, height: 9 * k), P.signal)
    let eyeFont = Typo.saira(10.5 * k, weight: 600, width: 125, tabular: false)
    let eye = drawText("// POMODORO", TextStyle(eyeFont, P.cmdText.alpha(0.55), tracking: 10.5 * k * 0.14),
                       x: L.pad + 18 * k, baseline: base)
    drawText("番茄钟", TextStyle(Typo.zh(14 * k, .semibold), P.cmdText),
             x: L.pad + 18 * k + eye + 12 * k, baseline: base)

    // 中：日期字段
    let fieldFont = Typo.saira(12.5 * k, weight: 500, width: 112)
    let zhFont = Typo.zh(12.5 * k, .regular)
    let parts: [(String, NSFont, NSColor)] = [
        ("专注 \(s.pomo.workMin)", zhFont, P.cmdText.alpha(0.62)),
        ("│", fieldFont, P.cmdText.alpha(0.20)),
        ("短休 \(s.pomo.shortMin)", zhFont, P.cmdText.alpha(0.62)),
        ("│", fieldFont, P.cmdText.alpha(0.20)),
        ("长休 \(s.pomo.longMin)", zhFont, P.cmdText.alpha(0.62))
    ]
    var total: CGFloat = 0
    for (t, f, _) in parts { total += textWidth(t, TextStyle(f, P.cmdText)) + 16 * k }
    var px = (L.w - total + 16 * k) / 2
    for (t, f, c) in parts {
        let wd = drawText(t, TextStyle(f, c), x: px, baseline: base)
        px += wd + 16 * k
    }

    // 右：运行状态
    var rx = L.w - L.pad
    let stFont = Typo.zh(12.5 * k, .semibold)
    rx -= drawTextRight(s.pomo.running ? "运行中" : "已暂停",
                        TextStyle(stFont, s.pomo.running ? P.signal : P.cmdText.alpha(0.55)),
                        right: rx, baseline: base)
    if s.pomo.running {
        fillRect(CGRect(x: rx - 13 * k, y: mid - 3 * k, width: 6 * k, height: 6 * k), P.signal)
    } else {
        strokeRect(CGRect(x: rx - 13 * k, y: mid - 3 * k, width: 6 * k, height: 6 * k), P.cmdText.alpha(0.45), 1)
    }
    rx -= 21 * k
    rx -= drawTextRight("第 \(min(s.pomo.groupDone + 1, s.pomo.longEvery)) 组 / \(s.pomo.longEvery)",
                        TextStyle(Typo.zh(12.5 * k, .regular), P.cmdText.alpha(0.55)), right: rx, baseline: base)
    rx -= 18 * k
    drawTextRight("│", TextStyle(fieldFont, P.cmdText.alpha(0.20)), right: rx, baseline: base)
}

// MARK: 时钟横幅

private func drawClockBanner(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    let k = L.k
    let x0 = L.pad

    let eyeFont = Typo.saira(11 * k, weight: 600, width: 125, tabular: false)
    drawText("// LOCAL TIME", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: x0, baseline: L.eyebrowBaseline)
    drawText("本地时间", TextStyle(Typo.zh(24 * k, .semibold), P.ink),
             x: x0, baseline: L.titleBaseline)

    // 主视觉
    let clockFont = Typo.saira(L.clockSize, weight: 760, width: 112, tabular: true)
    drawText(fmtClock.string(from: s.now), TextStyle(clockFont, P.ink), x: x0, baseline: L.clockBaseline)

    // 日期字段行
    let f = Typo.saira(15 * k, weight: 500, width: 112)
    let zh = Typo.zh(15 * k, .regular)
    var x = x0
    x += drawText(fmtDate.string(from: s.now), TextStyle(f, P.ink2), x: x, baseline: L.dateBaseline) + 14 * k
    x += drawText("│", TextStyle(f, P.ink2.alpha(0.35)), x: x, baseline: L.dateBaseline) + 14 * k
    x += drawText(fmtWeekday.string(from: s.now), TextStyle(zh, P.ink2), x: x, baseline: L.dateBaseline) + 14 * k
    x += drawText("│", TextStyle(f, P.ink2.alpha(0.35)), x: x, baseline: L.dateBaseline) + 14 * k
    drawText("W\(isoWeek(s.now))", TextStyle(f, P.ink2), x: x, baseline: L.dateBaseline)

    drawTextRight(timezoneLabel(), TextStyle(f, P.ink2.alpha(0.75)),
                  right: L.w - L.pad, baseline: L.dateBaseline)
}

// MARK: 仪表盘

private func drawDeck(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    let k = L.k
    let c = L.dialCenter
    let r = L.dialRadius
    let thick = max(10 * k, 18 * k)

    // 60 格分段圆盘：已过 = 墨，未到 = 浅灰，当前一格 = 黄
    let segs = 60
    let gone = 1 - s.pomo.fraction
    let head = min(segs - 1, max(0, Int(gone * Double(segs))))
    for i in 0..<segs {
        let a0 = -90.0 + Double(i) * 6.0 + 0.45
        let a1 = a0 + 5.1
        let color: NSColor
        if i < head { color = P.ink }
        else if i == head { color = P.signal }
        else { color = P.chip }
        color.setStroke()
        let p = NSBezierPath()
        p.appendArc(withCenter: c, radius: r, startAngle: CGFloat(a0), endAngle: CGFloat(a1), clockwise: false)
        p.lineWidth = thick
        p.stroke()
    }

    // 外框细线：给圆盘一个边界
    P.rule.setStroke()
    let ring = NSBezierPath()
    ring.appendArc(withCenter: c, radius: r + thick / 2 + 9 * k, startAngle: 0, endAngle: 360)
    ring.lineWidth = 1
    ring.stroke()

    // 环内读数
    drawTextCenter(s.pomo.phase.name, TextStyle(Typo.zh(12.5 * k, .semibold), P.ink2),
                   centerX: c.x, baseline: c.y - r * 0.36)
    let timeFont = Typo.saira(min(52 * k, r * 0.42), weight: 800, width: 112, tabular: true)
    drawTextCenter(fmtSeconds(s.pomo.remaining), TextStyle(timeFont, P.ink),
                   centerX: c.x, baseline: c.y + r * 0.17)
    drawTextCenter(s.pomo.running ? "进行中" : "已暂停",
                   TextStyle(Typo.zh(11 * k, .regular), s.pomo.running ? P.ink2 : P.ink2.alpha(0.55)),
                   centerX: c.x, baseline: c.y + r * 0.52)

    // 竖线：分组
    let ruleTop = c.y - r * 0.82
    let ruleBottom = c.y + r * 0.82
    vRule(L.group2X - 30 * k, ruleTop, ruleBottom, P.rule)
    if L.showGroup3 { vRule(L.group3X - 30 * k, ruleTop, ruleBottom, P.rule) }

    let eyeFont = Typo.saira(11 * k, weight: 600, width: 125, tabular: false)
    let fieldFont = Typo.saira(14 * k, weight: 500, width: 112)

    // 组二：本轮进度
    drawText("// SESSIONS", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: L.group2X, baseline: c.y - 44 * k)
    drawText("本轮进度", TextStyle(Typo.zh(22 * k, .semibold), P.ink),
             x: L.group2X, baseline: c.y - 10 * k)

    let done = s.pomo.groupDone
    let bw = 56 * k, bh = 14 * k, gap = 12 * k
    let blockY = c.y + 14 * k
    for i in 0..<s.pomo.longEvery {
        let br = CGRect(x: L.group2X + CGFloat(i) * (bw + gap), y: blockY, width: bw, height: bh)
        let active = (i == done && s.pomo.phase == .work)
        fillRect(br, i < done ? P.ink : (active ? P.signal : P.chip))
        if active { strokeRect(br, P.ink, 1) }
    }
    var fx = L.group2X
    fx += drawText("\(done) / \(s.pomo.longEvery)", TextStyle(fieldFont, P.ink2),
                   x: fx, baseline: blockY + bh + 28 * k) + 12 * k
    fx += drawText("│", TextStyle(fieldFont, P.ink2.alpha(0.35)), x: fx, baseline: blockY + bh + 28 * k) + 12 * k
    drawText("\(s.pomo.workMin) 分钟一组", TextStyle(Typo.zh(13 * k, .regular), P.ink2),
             x: fx, baseline: blockY + bh + 28 * k)

    // 组三：下一次
    guard L.showGroup3 else { return }
    drawText("// NEXT", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: L.group3X, baseline: c.y - 44 * k)
    drawText("下次 \(s.pomo.nextPhase.name)", TextStyle(Typo.zh(22 * k, .semibold), P.ink),
             x: L.group3X, baseline: c.y - 10 * k)
    drawText("\(s.pomo.nextPhaseMinutes) 分钟", TextStyle(Typo.zh(13 * k, .regular), P.ink2),
             x: L.group3X, baseline: c.y + 26 * k)
    drawText("已累计 \(s.pomo.completed * s.pomo.workMin) 分钟",
             TextStyle(Typo.zh(13 * k, .regular), P.ink2.alpha(0.85)),
             x: L.group3X, baseline: c.y + 54 * k)
}

// MARK: 待办看板

func rowRect(_ i: Int, _ L: HomeLayout) -> CGRect {
    let top = L.board.minY + L.headerH
    return CGRect(x: L.board.minX, y: top + CGFloat(i) * L.rowH, width: L.board.width, height: L.rowH)
}

func boardCheckRect(_ i: Int, _ L: HomeLayout) -> CGRect {
    let row = rowRect(i, L)
    let k = L.k
    return CGRect(x: L.board.minX + 24 * k, y: row.midY - 9 * k, width: 18 * k, height: 18 * k)
}

private func drawBoard(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    let k = L.k
    let b = L.board
    let cut = 26 * k

    // 画框影子：硬边斜线纹
    hatch(b.offsetBy(dx: 9 * k, dy: 9 * k), P.ink, alpha: P.isDark ? 0.10 : 0.11, period: 7 * k, lineW: 1.5 * k)

    // 白板 + 朝中心的切角
    fillCut(b, cut, .tl, P.panel)
    strokeCut(b, cut, .tl, P.ink, 1)
    fillRect(CGRect(x: b.minX + cut, y: b.minY, width: b.width * 0.38, height: 5 * k), P.ink)

    let x0 = b.minX + 26 * k
    let x1 = b.maxX - 24 * k

    // 抬头
    let eyeFont = Typo.saira(11 * k, weight: 600, width: 125, tabular: false)
    drawText("// TASKS", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: x0, baseline: b.minY + 32 * k)
    drawText("待办", TextStyle(Typo.zh(22 * k, .semibold), P.ink),
             x: x0, baseline: b.minY + 62 * k)
    let countFont = Typo.saira(15 * k, weight: 700, width: 112)
    drawTextRight(String(format: "%02d / %02d", s.store.doneCount, s.store.items.count),
                  TextStyle(countFont, P.ink2), right: x1, baseline: b.minY + 62 * k)
    hRule(x0, x1, b.minY + L.headerH - 14 * k, P.rule)

    let live = s.listAlpha > 0.02
    if live {
        for r in 0..<L.visibleRows {
            let i = s.scroll + r
            guard i < s.store.items.count else { break }
            let p = max(0, min(1, (s.listAlpha - Double(r) * 0.055) / 0.62))
            if p <= 0.01 { continue }
            drawTodoRow(s, L, P, item: i, row: r, p: easeSnap(p))
        }
    } else {
        drawQuietState(s, L, P)
    }

    // 页脚
    let over = s.store.items.count - L.visibleRows
    let footFont = Typo.zh(11.5 * k, .regular)
    let footBase = b.maxY - 18 * k
    if over > 0 {
        drawText("还有 \(over) 条  ·  滚轮翻页", TextStyle(footFont, P.ink2.alpha(0.85)), x: x0, baseline: footBase)
        drawTextRight("E 打开编辑", TextStyle(footFont, P.ink2.alpha(0.85)), right: x1, baseline: footBase)
    } else if live {
        drawText(s.store.items.isEmpty ? "还没有条目" : "共 \(s.store.items.count) 条",
                 TextStyle(footFont, P.ink2.alpha(0.75)), x: x0, baseline: footBase)
        drawTextRight("E 打开设置", TextStyle(footFont, P.ink2.alpha(0.75)), right: x1, baseline: footBase)
    } else {
        drawButton(ButtonSpec(label: "常驻", rect: L.pinButton, cut: 7 * k),
                   hover: s.hoverPin ? 1 : 0, pressed: false, P, k)
        drawTextRight("E 打开设置", TextStyle(footFont, P.ink2.alpha(0.7)), right: x1, baseline: footBase)
    }
}

private func drawQuietState(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    let k = L.k
    let b = L.board
    let x0 = b.minX + 26 * k
    let x1 = b.maxX - 24 * k
    let top = b.minY + L.headerH
    let boxH = 46 * k
    let boxY = b.maxY - L.footerH - 18 * k - boxH
    let band = CGRect(x: x0, y: top + 10 * k, width: x1 - x0,
                      height: max(40, boxY - 22 * k - (top + 10 * k)))
    hatch(band, P.ink, alpha: P.isDark ? 0.09 : 0.06, period: 7 * k, lineW: 1.5 * k)

    let msg = s.store.items.isEmpty
        ? "还没有待办。按 E 打开设置，写下第一条。"
        : "待办每 \(s.settings.listEvery) 秒出现一次，按 L 常驻。"
    let box = CGRect(x: x0, y: boxY, width: x1 - x0, height: boxH)
    fillRect(box, P.panel)
    strokeRect(box, P.rule, 1)
    fillRect(CGRect(x: box.minX + 14 * k, y: box.midY - 7 * k, width: 3 * k, height: 14 * k), P.signal)
    drawTextIn(msg, TextStyle(Typo.zh(13 * k, .regular), P.ink2),
               rect: CGRect(x: box.minX + 28 * k, y: box.minY, width: box.width - 40 * k, height: box.height))
}

private func drawTodoRow(_ s: Scene, _ L: HomeLayout, _ P: Palette, item i: Int, row v: Int, p: Double) {
    let k = L.k
    let item = s.store.items[i]
    let selected = (i == s.selected) && s.pinned
    let shift = (selected ? 6 * k : 0) + CGFloat(1 - p) * 22 * k

    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current?.cgContext.setAlpha(CGFloat(p) * CGFloat(min(1, s.listAlpha * 1.15)))

    let row = rowRect(v, L)
    let x1 = L.board.maxX - 24 * k

    // 当前位置：左侧 trace 色条
    if selected {
        fillRect(CGRect(x: L.board.minX + 1, y: row.minY + 1, width: 4 * k, height: row.height - 2), P.trace)
    }

    // 悬停：淡底
    if s.hoverRow == i && !selected {
        fillRect(CGRect(x: L.board.minX + 12 * k, y: row.minY + 3 * k,
                        width: row.width - 24 * k, height: row.height - 6 * k), P.chip.alpha(0.55))
    }

    // 勾选框
    let cb = boardCheckRect(v, L).offsetBy(dx: shift, dy: 0)
    drawCheck(cb, checked: item.done, hot: s.hoverCheck == i, P, k)

    // 编号标签
    let tagW = 32 * k, tagH = 19 * k
    let tag = CGRect(x: cb.maxX + 14 * k, y: row.midY - tagH / 2, width: tagW, height: tagH)
    drawTag(String(format: "%02d", i + 1), rect: tag, current: selected, P, k)

    // 正文
    let tx = tag.maxX + 14 * k
    let tw = x1 - tx
    let f = Typo.zh(15 * k, .medium)
    let color = item.done ? P.ink2.alpha(0.9) : P.ink
    drawTextIn(item.text, TextStyle(f, color), rect: CGRect(x: tx, y: row.minY, width: tw, height: row.height))

    // 完成：划线
    if item.done {
        let w = min(tw, textWidth(item.text, TextStyle(f, color)))
        hRule(tx, tx + w, row.midY + 0.5, P.ink2.alpha(0.65), max(1, 1.1 * k))
    }

    if i < s.store.items.count - 1 {
        hRule(L.board.minX + 24 * k, x1, row.maxY, P.rule, 1)
    }
    NSGraphicsContext.restoreGraphicsState()
}

// MARK: 底部动作条

private func drawActionBar(_ s: Scene, _ L: HomeLayout, _ P: Palette) {
    let k = L.k
    hRule(0, L.w, L.bottomRuleY, P.rule)

    for (i, spec) in L.buttons.enumerated() {
        drawButton(spec, hover: s.btnHover[i], pressed: s.pressBtn == i, P, k)
    }

    guard L.w > 1400 * k else { return }
    let base = L.buttons[0].rect.midY + 4.5 * k
    let keys: [(String, String)] = [
        ("空格", "开始"), ("R", "重置"), ("N", "跳过"), ("L", "待办"),
        ("E", "设置"), ("T", "透明度"), ("D", "深浅"), ("F11", "窗口")
    ]
    let kf = Typo.saira(11.5 * k, weight: 600, width: 112)
    let vf = Typo.zh(12 * k, .regular)
    var total: CGFloat = 0
    for (key, val) in keys {
        total += textWidth(key, TextStyle(kf, P.ink)) + 6 * k
            + textWidth(val, TextStyle(vf, P.ink2)) + 20 * k
    }
    var x = L.w - L.pad - total + 20 * k
    for (key, val) in keys {
        x += drawText(key, TextStyle(kf, P.ink.alpha(0.85)), x: x, baseline: base) + 6 * k
        x += drawText(val, TextStyle(vf, P.ink2), x: x, baseline: base) + 20 * k
    }
}


// MARK: 命中测试

func homeHitTest(_ p: CGPoint, size: CGSize, scene: Scene) -> HomeHit {
    let L = homeLayout(size: size, scene: scene)
    for (i, b) in L.buttons.enumerated() where b.rect.contains(p) { return .button(i) }
    if !(scene.listAlpha > 0.5) {
        if L.pinButton.contains(p) { return .pin }
        if L.board.contains(p) { return .none }
    }
    let dx = p.x - L.dialCenter.x
    let dy = p.y - L.dialCenter.y
    if dx * dx + dy * dy <= pow(L.dialRadius * 1.22, 2) { return .dial }
    for v in 0..<L.visibleRows {
        let real = scene.scroll + v
        guard real < scene.store.items.count else { break }
        let row = rowRect(v, L)
        if row.contains(p) {
            if boardCheckRect(v, L).insetBy(dx: -8, dy: -10).contains(p) { return .check(real) }
            return .row(real)
        }
    }
    return .none
}
