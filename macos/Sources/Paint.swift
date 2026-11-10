//
//  Paint.swift —— 直角 / 切角 / 细线 / 斜线纹 / 等高线 / 按钮 / 标签
//

import AppKit
import Foundation

enum Corner {
    case tl, tr, br, bl
}

func cutPath(_ r: CGRect, _ cut: CGFloat, _ corner: Corner) -> NSBezierPath {
    let c = max(0, min(cut, min(r.width, r.height) * 0.5))
    let p = NSBezierPath()
    switch corner {
    case .br:
        p.move(to: CGPoint(x: r.minX, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.maxY - c))
        p.line(to: CGPoint(x: r.maxX - c, y: r.maxY))
        p.line(to: CGPoint(x: r.minX, y: r.maxY))
    case .bl:
        p.move(to: CGPoint(x: r.minX + c, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.maxY))
        p.line(to: CGPoint(x: r.minX, y: r.maxY))
        p.line(to: CGPoint(x: r.minX, y: r.minY + c))
    case .tr:
        p.move(to: CGPoint(x: r.minX, y: r.minY))
        p.line(to: CGPoint(x: r.maxX - c, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.minY + c))
        p.line(to: CGPoint(x: r.maxX, y: r.maxY))
        p.line(to: CGPoint(x: r.minX, y: r.maxY))
    case .tl:
        p.move(to: CGPoint(x: r.minX + c, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.minY))
        p.line(to: CGPoint(x: r.maxX, y: r.maxY))
        p.line(to: CGPoint(x: r.minX, y: r.maxY))
        p.line(to: CGPoint(x: r.minX, y: r.minY + c))
    }
    p.close()
    return p
}

func fillRect(_ r: CGRect, _ c: NSColor) {
    c.setFill()
    NSBezierPath(rect: r).fill()
}

func fillCut(_ r: CGRect, _ cut: CGFloat, _ corner: Corner, _ c: NSColor) {
    c.setFill()
    cutPath(r, cut, corner).fill()
}

func strokeCut(_ r: CGRect, _ cut: CGFloat, _ corner: Corner, _ c: NSColor, _ w: CGFloat = 1) {
    c.setStroke()
    let p = cutPath(r.insetBy(dx: w / 2, dy: w / 2), cut, corner)
    p.lineWidth = w
    p.stroke()
}

func strokeRect(_ r: CGRect, _ c: NSColor, _ w: CGFloat = 1) {
    c.setStroke()
    let p = NSBezierPath(rect: r.insetBy(dx: w / 2, dy: w / 2))
    p.lineWidth = w
    p.stroke()
}

/// 1px 细线：落在整像素上
func hRule(_ x0: CGFloat, _ x1: CGFloat, _ y: CGFloat, _ c: NSColor, _ h: CGFloat = 1) {
    fillRect(CGRect(x: x0, y: y.rounded(), width: max(0, x1 - x0), height: h), c)
}

func vRule(_ x: CGFloat, _ y0: CGFloat, _ y1: CGFloat, _ c: NSColor, _ w: CGFloat = 1) {
    fillRect(CGRect(x: x.rounded(), y: y0, width: w, height: max(0, y1 - y0)), c)
}

/// 45° 斜线纹：空状态、禁用、画框影子
func hatch(_ r: CGRect, _ color: NSColor, alpha: CGFloat = 0.22, period: CGFloat = 6.5, lineW: CGFloat = 1.5) {
    guard r.width > 1, r.height > 2 else { return }
    NSGraphicsContext.saveGraphicsState()
    NSBezierPath(rect: r).addClip()
    color.withAlphaComponent(alpha).setStroke()
    let step = period * 1.4142
    let p = NSBezierPath()
    p.lineWidth = lineW
    var x = r.minX - r.height
    while x < r.maxX + r.height {
        p.move(to: CGPoint(x: x, y: r.maxY))
        p.line(to: CGPoint(x: x + r.height, y: r.minY))
        x += step
    }
    p.stroke()
    NSGraphicsContext.restoreGraphicsState()
}

/// 屏幕起始处的三色条
func triStrip(_ r: CGRect, middle: NSColor = triB) {
    let w = r.width / 3
    fillRect(CGRect(x: r.minX, y: r.minY, width: w, height: r.height), triA)
    fillRect(CGRect(x: r.minX + w, y: r.minY, width: w, height: r.height), middle)
    fillRect(CGRect(x: r.minX + w * 2, y: r.minY, width: r.width - w * 2, height: r.height), triC)
}

/// 等高线：两角各一小片，不闭合，很淡
func contourMarks(center: CGPoint, radii: [CGFloat], from: CGFloat, to: CGFloat, color: NSColor, alpha: CGFloat = 0.09) {
    NSGraphicsContext.saveGraphicsState()
    color.withAlphaComponent(alpha).setStroke()
    for (i, r) in radii.enumerated() {
        let p = NSBezierPath()
        let shift = CGFloat(i) * 6
        p.appendArc(withCenter: CGPoint(x: center.x + shift, y: center.y + shift),
                    radius: r, startAngle: from, endAngle: to, clockwise: false)
        p.lineWidth = 1
        p.stroke()
    }
    NSGraphicsContext.restoreGraphicsState()
}

// MARK: - 按钮

struct ButtonSpec {
    var label: String
    var rect: CGRect
    var primary: Bool = false
    var toggled: Bool = false
    var danger: Bool = false
    var enabled: Bool = true
    var cut: CGFloat = 8
    var sub: String = ""
}

/// hover 0…1：黄色从左擦入；按下整体下沉 1px
func drawButton(_ b: ButtonSpec, hover: Double, pressed: Bool, _ P: Palette, _ k: CGFloat) {
    var r = b.rect
    if pressed { r = r.offsetBy(dx: 0, dy: 1) }
    let path = cutPath(r, b.cut, .br)

    // 底盘
    if b.primary {
        fillCut(r, b.cut, .br, P.ink)
    } else if b.toggled {
        fillCut(r, b.cut, .br, P.chip)
    } else {
        fillCut(r, b.cut, .br, P.panel)
    }

    // 黄色擦入
    if hover > 0.001 && b.enabled {
        NSGraphicsContext.saveGraphicsState()
        path.addClip()
        let w = r.width * CGFloat(hover) + 2
        fillRect(CGRect(x: r.minX, y: r.minY, width: w, height: r.height), P.signal)
        NSGraphicsContext.restoreGraphicsState()
    }

    // 描边
    if b.primary {
        strokeCut(r, b.cut, .br, P.ink, 1)
    } else {
        strokeCut(r, b.cut, .br, b.toggled ? P.ink : P.ink.alpha(0.55), 1)
    }

    // 字
    let lit = hover > 0.45
    let color: NSColor
    if !b.enabled {
        color = P.ink2.alpha(0.55)
    } else if lit {
        color = P.onSignal
    } else if b.primary {
        color = P.paper
    } else if b.danger {
        color = P.danger
    } else {
        color = P.ink
    }

    let f = Typo.zh(r.height > 40 * k ? 15 * k : 13 * k, .semibold)
    if b.sub.isEmpty {
        drawTextIn(b.label, TextStyle(f, color), rect: r.insetBy(dx: 8 * k, dy: 0), align: .center)
    } else {
        let fe = Typo.saira(9 * k, weight: 700, width: 125, tabular: false)
        drawTextIn(b.sub, TextStyle(fe, color.alpha(0.75), tracking: 9 * k * 0.14),
                   rect: CGRect(x: r.minX + 8 * k, y: r.minY + 5 * k, width: r.width - 16 * k, height: 12 * k),
                   align: .center)
        drawTextIn(b.label, TextStyle(f, color),
                   rect: CGRect(x: r.minX + 8 * k, y: r.minY + 16 * k, width: r.width - 16 * k, height: r.height - 20 * k),
                   align: .center)
    }
}

/// 编号标签：墨底白字，右下切角；current = 黄底墨字
func drawTag(_ text: String, rect: CGRect, current: Bool, _ P: Palette, _ k: CGFloat) {
    let cut = 7 * k
    fillCut(rect, cut, .br, current ? P.signal : P.tagBg)
    let f = Typo.saira(rect.height * 0.52, weight: 700, width: 112)
    drawTextIn(text, TextStyle(f, current ? P.onSignal : P.tagFg),
               rect: rect.insetBy(dx: 5 * k, dy: 0), align: .center)
}

/// 勾选框：直角框 = 未完成，实心 = 完成（可按，所以切角）
func drawCheck(_ rect: CGRect, checked: Bool, hot: Bool, _ P: Palette, _ k: CGFloat) {
    if checked {
        fillCut(rect, 5 * k, .br, hot ? P.ink : P.tagBg)
        let p = NSBezierPath()
        let s = rect.width
        p.lineWidth = max(1.6, 2 * k)
        p.lineCapStyle = .square
        p.move(to: CGPoint(x: rect.minX + s * 0.24, y: rect.midY + 1))
        p.line(to: CGPoint(x: rect.minX + s * 0.44, y: rect.maxY - s * 0.26))
        p.line(to: CGPoint(x: rect.minX + s * 0.78, y: rect.minY + s * 0.26))
        (P.isDark ? P.paper : P.panel).withAlphaComponent(0.95).setStroke()
        p.stroke()
    } else {
        strokeCut(rect, 5 * k, .br, hot ? P.ink : P.ink.alpha(0.5), 1)
    }
}
