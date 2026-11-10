//
//  SettingsPanel.swift —— 设置面板（待办 / 背景图片 / 外观 / 提示音）
//

import AppKit

enum PanelHit: Equatable {
    case none
    case close
    case done
    case add
    case pick
    case clear
    case row(Int)
    case check(Int)
    case delete(Int)
    case theme(Int)
    case sound(Int)
}

struct SettingsGeometry {
    var k: CGFloat = 1
    var panel: CGRect = .zero
    var close: CGRect = .zero
    var taskRows: [CGRect] = []
    var deleteButtons: [CGRect] = []
    var checks: [CGRect] = []
    var input: CGRect = .zero
    var add: CGRect = .zero
    var pick: CGRect = .zero
    var clear: CGRect = .zero
    var themeChips: [CGRect] = []
    var soundChips: [CGRect] = []
    var done: CGRect = .zero
    var noteBaseline: CGFloat = 0
    var rowCount: Int = 0
    var extraCount: Int = 0
}

func settingsGeometry(size: CGSize, scene: Scene) -> SettingsGeometry {
    var G = SettingsGeometry()
    let k = max(0.60, min(1.45, size.height / 1080))
    G.k = k

    let n = scene.store.items.count
    let maxRows = 6
    let visible = min(max(n, 1), maxRows)
    let extra = max(0, n - maxRows)

    let pw = min(800 * k, size.width - 100 * k)
    let rowH = 44 * k
    let extraH: CGFloat = extra > 0 ? 24 * k : 0
    let ph = min(642 * k + CGFloat(max(0, visible - 4)) * rowH + extraH, size.height - 60 * k)

    let t = CGFloat(max(0, min(1, scene.panelT)))
    let offset = (1 - t) * 16 * k
    let panel = CGRect(x: (size.width - pw) / 2, y: (size.height - ph) / 2 - offset, width: pw, height: ph)
    G.panel = panel

    let inset = 36 * k
    let x0 = panel.minX + inset
    let x1 = panel.maxX - inset
    let top = panel.minY

    G.close = CGRect(x: x1 - 72 * k, y: top + 22 * k, width: 72 * k, height: 30 * k)

    // 待办段
    let rowsTop = top + 146 * k
    G.rowCount = min(n, maxRows)
    G.extraCount = extra
    for i in 0..<G.rowCount {
        let r = CGRect(x: x0, y: rowsTop + CGFloat(i) * rowH, width: x1 - x0, height: rowH)
        G.taskRows.append(r)
        G.checks.append(CGRect(x: r.minX, y: r.midY - 8 * k, width: 16 * k, height: 16 * k))
        G.deleteButtons.append(CGRect(x: r.maxX - 52 * k, y: r.midY - 12 * k, width: 52 * k, height: 24 * k))
    }
    G.noteBaseline = rowsTop + CGFloat(G.rowCount) * rowH + 14 * k

    let inputY = rowsTop + CGFloat(max(G.rowCount, 1)) * rowH + 10 * k + extraH
    G.add = CGRect(x: x1 - 88 * k, y: inputY, width: 88 * k, height: 38 * k)
    G.input = CGRect(x: x0, y: inputY, width: (x1 - 100 * k) - x0, height: 38 * k)

    // 背景图片段
    let wallY = inputY + 38 * k + 66 * k
    G.pick = CGRect(x: x1 - 100 * k, y: wallY, width: 100 * k, height: 36 * k)
    G.clear = CGRect(x: x1 - 100 * k - 68 * k - 8 * k, y: wallY, width: 68 * k, height: 36 * k)

    // 外观段
    let chipY = wallY + 36 * k + 56 * k
    let cw = 72 * k, chh = 32 * k, gap = 8 * k
    G.themeChips = [CGRect(x: x0 + 96 * k, y: chipY, width: cw, height: chh),
                    CGRect(x: x0 + 96 * k + cw + gap, y: chipY, width: cw, height: chh)]
    G.soundChips = [CGRect(x: x0 + 96 * k + 2 * cw + gap + 130 * k, y: chipY, width: 56 * k, height: chh),
                    CGRect(x: x0 + 96 * k + 2 * cw + gap + 130 * k + 56 * k + gap, y: chipY,
                           width: 56 * k, height: chh)]

    G.done = CGRect(x: x1 - 96 * k, y: panel.maxY - 52 * k, width: 96 * k, height: 34 * k)
    return G
}

func settingsHitTest(_ p: CGPoint, _ G: SettingsGeometry) -> PanelHit {
    if G.close.insetBy(dx: -4, dy: -4).contains(p) { return .close }
    if G.done.insetBy(dx: -4, dy: -4).contains(p) { return .done }
    if G.add.contains(p) { return .add }
    if G.pick.contains(p) { return .pick }
    if G.clear.contains(p) { return .clear }
    for (i, r) in G.taskRows.enumerated() where r.contains(p) {
        if G.deleteButtons[i].contains(p) { return .delete(i) }
        if G.checks[i].insetBy(dx: -8, dy: -10).contains(p) { return .check(i) }
        return .row(i)
    }
    for (i, r) in G.themeChips.enumerated() where r.contains(p) { return .theme(i) }
    for (i, r) in G.soundChips.enumerated() where r.contains(p) { return .sound(i) }
    return .none
}

func drawSettings(_ s: Scene, _ G: SettingsGeometry, _ P: Palette) {
    let t = CGFloat(max(0, min(1, s.panelT)))
    guard t > 0.001 else { return }
    let hv: (PanelHit) -> Double = { $0 == s.panelHover ? 1 : 0 }
    let k = G.k
    let panel = G.panel

    // 幕布
    let full = CGRect(x: 0, y: 0, width: 8000, height: 8000)
    fillRect(full, P.scrim.alpha(P.scrim.alphaComponent * t))

    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current?.cgContext.setAlpha(t)

    // 面板：白底细线 + 切角
    let cut = 26 * k
    fillCut(panel, cut, .br, P.panel)
    strokeCut(panel, cut, .br, P.ink, 1)
    fillRect(CGRect(x: panel.minX, y: panel.minY, width: panel.width * 0.34, height: 5 * k), P.ink)

    let x0 = panel.minX + 36 * k
    let x1 = panel.maxX - 36 * k

    // 抬头
    let eyeFont = Typo.saira(11 * k, weight: 600, width: 125, tabular: false)
    drawText("// SETTINGS", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: x0, baseline: panel.minY + 34 * k)
    drawText("设置", TextStyle(Typo.zh(26 * k, .semibold), P.ink),
             x: x0, baseline: panel.minY + 70 * k)
    drawButton(ButtonSpec(label: "关闭", rect: G.close, cut: 8 * k), hover: hv(.close), pressed: false, P, k)
    hRule(x0, x1, panel.minY + 92 * k, P.rule)

    // 待办
    drawText("// TASKS", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: x0, baseline: panel.minY + 122 * k)
    let cntFont = Typo.saira(13 * k, weight: 700, width: 112)
    drawTextRight(String(format: "%02d / %02d", s.store.doneCount, s.store.items.count),
                  TextStyle(cntFont, P.ink2), right: x1, baseline: panel.minY + 122 * k)

    if s.store.items.isEmpty {
        drawText("还没有待办，在下面输入第一条。", TextStyle(Typo.zh(13 * k, .regular), P.ink2),
                 x: x0, baseline: panel.minY + 170 * k)
    }

    for (i, row) in G.taskRows.enumerated() {
        guard i < s.store.items.count else { break }
        let item = s.store.items[i]
        let sel = (s.editing == i)
        if s.panelHover == .row(i) && !sel {
            fillRect(row.insetBy(dx: -10 * k, dy: 2 * k), P.chip.alpha(0.5))
        }
        if sel {
            fillRect(row.insetBy(dx: -10 * k, dy: 2 * k), P.chip)
            fillRect(CGRect(x: row.minX - 10 * k, y: row.minY + 2 * k, width: 3 * k, height: row.height - 4 * k), P.trace)
        }
        drawCheck(G.checks[i], checked: item.done, hot: false, P, k)
        let tf = Typo.zh(14 * k, .regular)
        drawTextIn(item.text, TextStyle(tf, item.done ? P.ink2 : P.ink),
                   rect: CGRect(x: G.checks[i].maxX + 14 * k, y: row.minY,
                                width: G.deleteButtons[i].minX - G.checks[i].maxX - 24 * k, height: row.height))
        drawButton(ButtonSpec(label: "删除", rect: G.deleteButtons[i], danger: true, cut: 6 * k),
                   hover: hv(.delete(i)), pressed: false, P, k)
        hRule(x0, x1, row.maxY, P.rule)
    }

    if G.extraCount > 0 {
        drawText("还有 \(G.extraCount) 条没显示，先删掉几条。",
                 TextStyle(Typo.zh(11.5 * k, .regular), P.ink2),
                 x: x0, baseline: G.noteBaseline)
    }

    // 输入框外框（真正的输入控件叠在上面）
    strokeRect(G.input, P.ink.alpha(0.8), 1)
    drawButton(ButtonSpec(label: s.editing == nil ? "添加" : "保存", rect: G.add, primary: true, cut: 7 * k),
               hover: hv(.add), pressed: false, P, k)

    // 背景图片
    drawText("// WALLPAPER", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: x0, baseline: G.pick.minY - 20 * k)
    drawText("背景图片", TextStyle(Typo.zh(14 * k, .semibold), P.ink),
             x: x0, baseline: G.pick.midY + 5 * k)
    let path = s.settings.bgImage.isEmpty ? "未设置" : (s.settings.bgImage as NSString).lastPathComponent
    drawTextIn(path, TextStyle(Typo.zh(13 * k, .regular), P.ink2),
               rect: CGRect(x: x0 + 84 * k, y: G.pick.minY, width: G.clear.minX - x0 - 94 * k, height: G.pick.height))
    drawButton(ButtonSpec(label: "选择图片", rect: G.pick, cut: 7 * k), hover: hv(.pick), pressed: false, P, k)
    drawButton(ButtonSpec(label: "清除", rect: G.clear, cut: 7 * k), hover: hv(.clear), pressed: false, P, k)

    hRule(x0, x1, G.pick.maxY + 28 * k, P.rule)

    // 外观 + 提示音
    drawText("// APPEARANCE", TextStyle(eyeFont, P.ink2, tracking: 11 * k * 0.14),
             x: x0, baseline: G.themeChips[0].minY - 20 * k)
    drawText("外观", TextStyle(Typo.zh(14 * k, .semibold), P.ink),
             x: x0, baseline: G.themeChips[0].midY + 5 * k)
    for (i, r) in G.themeChips.enumerated() {
        drawSegChip(r, label: i == 0 ? "浅色" : "深色", on: s.theme.rawValue == i, P, k)
    }
    drawText("提示音", TextStyle(Typo.zh(14 * k, .semibold), P.ink),
             x: G.soundChips[0].minX - 76 * k, baseline: G.themeChips[0].midY + 5 * k)
    for (i, r) in G.soundChips.enumerated() {
        drawSegChip(r, label: i == 0 ? "开" : "关", on: (i == 0) != s.settings.muted, P, k)
    }

    // 页脚
    drawText("Esc 关闭   ·   回车添加待办", TextStyle(Typo.zh(12 * k, .regular), P.ink2),
             x: x0, baseline: panel.maxY - 30 * k)
    drawButton(ButtonSpec(label: "完成", rect: G.done, primary: true, cut: 7 * k),
               hover: hv(.done), pressed: false, P, k)

    NSGraphicsContext.restoreGraphicsState()
}

/// 分段选择：当前段墨底反白（不用黄）
private func drawSegChip(_ r: CGRect, label: String, on: Bool, _ P: Palette, _ k: CGFloat) {
    fillCut(r, 7 * k, .br, on ? P.ink : P.panel)
    strokeCut(r, 7 * k, .br, on ? P.ink : P.ink.alpha(0.45), 1)
    drawTextIn(label, TextStyle(Typo.zh(12.5 * k, .semibold), on ? P.paper : P.ink),
               rect: r.insetBy(dx: 6 * k, dy: 0), align: .center)
}
