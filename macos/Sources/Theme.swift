//
//  Theme.swift —— 配色与变体
//
//  变体卡（终末地风味）
//    变体：标准（yellow / neutral / standard / contours / standard / calm）
//    主视觉：超大等宽时钟 + 60 格分段圆盘（本组进度）
//    页面类型：控制台（其余面板按阅读页收紧）
//
//  三行固定：强调色只有一种，只做填充；切角 = 可按；细线分区。
//

import AppKit

enum ThemeKind: Int {
    case light = 0
    case dark = 1

    var next: ThemeKind { self == .light ? .dark : .light }
    var name: String { self == .light ? "浅色" : "深色" }
}

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255.0,
            green: CGFloat((hex >> 8) & 0xFF) / 255.0,
            blue: CGFloat(hex & 0xFF) / 255.0,
            alpha: alpha)
}

/// 把颜色按不透明度叠一档（用于 hatching、次要状态）
extension NSColor {
    func alpha(_ a: CGFloat) -> NSColor {
        usingColorSpace(.sRGB)?.withAlphaComponent(a) ?? self
    }

    func mix(_ other: NSColor, _ t: CGFloat) -> NSColor {
        let a = usingColorSpace(.sRGB) ?? self
        let b = other.usingColorSpace(.sRGB) ?? other
        return NSColor(srgbRed: a.redComponent + (b.redComponent - a.redComponent) * t,
                       green: a.greenComponent + (b.greenComponent - a.greenComponent) * t,
                       blue: a.blueComponent + (b.blueComponent - a.blueComponent) * t,
                       alpha: a.alphaComponent + (b.alphaComponent - a.alphaComponent) * t)
    }
}

struct Palette {
    var paper: NSColor
    var panel: NSColor
    var ink: NSColor
    var ink2: NSColor
    var rule: NSColor
    var chip: NSColor
    var signal: NSColor
    var onSignal: NSColor
    var trace: NSColor
    var tagBg: NSColor
    var tagFg: NSColor
    var danger: NSColor
    var scrim: NSColor
    var cmdBar: NSColor
    var cmdText: NSColor
    var isDark: Bool

    static let light = Palette(
        paper: rgb(0xF4F4F2),
        panel: rgb(0xFFFFFF),
        ink: rgb(0x161616),
        ink2: rgb(0x5C5C5C),
        rule: rgb(0xD9D9D6),
        chip: rgb(0xE4E4E0),
        signal: rgb(0xFFFA00),
        onSignal: rgb(0x161616),
        trace: rgb(0x161616),
        tagBg: rgb(0x161616),
        tagFg: rgb(0xFFFFFF),
        danger: rgb(0xB3261E),
        scrim: rgb(0x161616, 0.56),
        cmdBar: rgb(0x161616),
        cmdText: rgb(0xEDEDED),
        isDark: false)

    static let dark = Palette(
        paper: rgb(0x191919),
        panel: rgb(0x222222),
        ink: rgb(0xEDEDED),
        ink2: rgb(0xA3A3A3),
        rule: rgb(0x353535),
        chip: rgb(0x2E2E2E),
        signal: rgb(0xFFFA00),
        onSignal: rgb(0x161616),
        trace: rgb(0xFFFA00),
        tagBg: rgb(0x3A3A3A),
        tagFg: rgb(0xEDEDED),
        danger: rgb(0xFF8A7A),
        scrim: rgb(0x000000, 0.70),
        cmdBar: rgb(0x161616),
        cmdText: rgb(0xEDEDED),
        isDark: true)

    static func of(_ kind: ThemeKind) -> Palette {
        kind == .light ? .light : .dark
    }
}

/// 三色条：屏幕起始处，只出现一次
let triA = rgb(0xFF00F0)
let triB = rgb(0xFFFA00)
let triC = rgb(0x00FFA2)

// 动效曲线（snap = 出现，mech = 退出 / 擦入）
func easeSnap(_ t: Double) -> Double {
    // cubic-bezier(0.16, 1, 0.3, 1) 的近似：先快后稳
    let x = max(0, min(1, t))
    return 1 - pow(1 - x, 3.2)
}

func easeMech(_ t: Double) -> Double {
    // cubic-bezier(0.7, 0, 0.2, 1) 的近似：起手慢、收尾硬
    let x = max(0, min(1, t))
    return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2
}
