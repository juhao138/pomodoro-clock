//
//  Text.swift —— 字体（Saira 可变字体 + 苹方）与文字绘制
//

import AppKit
import CoreText

enum Typo {
    enum Zh {
        case regular, medium, semibold
        var psName: String {
            switch self {
            case .regular: return "PingFangSC-Regular"
            case .medium: return "PingFangSC-Medium"
            case .semibold: return "PingFangSC-Semibold"
            }
        }
    }

    private static var cache: [String: NSFont] = [:]
    private static var didRegister = false

    static func register() {
        if didRegister { return }
        didRegister = true
        var candidates: [URL] = []
        if let u = Bundle.main.url(forResource: "Saira", withExtension: "ttf") { candidates.append(u) }
        let exe = URL(fileURLWithPath: CommandLine.arguments.first ?? ".")
        let cwd = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        candidates.append(exe.deletingLastPathComponent().appendingPathComponent("Resources/Saira.ttf"))
        candidates.append(exe.deletingLastPathComponent().appendingPathComponent("../Resources/Saira.ttf"))
        candidates.append(cwd.appendingPathComponent("Sources/../Resources/Saira.ttf"))
        candidates.append(cwd.appendingPathComponent("Resources/Saira.ttf"))
        candidates.append(cwd.appendingPathComponent("macos/Resources/Saira.ttf"))
        for u in candidates where FileManager.default.fileExists(atPath: u.path) {
            var err: Unmanaged<CFError>?
            CTFontManagerRegisterFontsForURL(u as CFURL, .process, &err)
        }
    }

    /// 宽体拉丁：字段 112%，标签 / 字标 125%
    static func saira(_ size: CGFloat, weight: CGFloat = 600, width: CGFloat = 112, tabular: Bool = true) -> NSFont {
        register()
        let key = "s|\(size)|\(weight)|\(width)|\(tabular)"
        if let f = cache[key] { return f }
        var attrs: [NSFontDescriptor.AttributeName: Any] = [
            NSFontDescriptor.AttributeName(rawValue: kCTFontVariationAttribute as String):
                [0x77676874: weight, 0x77647468: width]
        ]
        if tabular {
            let feat: [String: Any] = [kCTFontOpenTypeFeatureTag as String: "tnum",
                                       kCTFontOpenTypeFeatureValue as String: 1]
            attrs[NSFontDescriptor.AttributeName(rawValue: kCTFontFeatureSettingsAttribute as String)] = [feat]
        }
        let desc = NSFontDescriptor(name: "Saira", size: size).addingAttributes(attrs)
        let f = NSFont(descriptor: desc, size: size) ?? NSFont.systemFont(ofSize: size, weight: .semibold)
        cache[key] = f
        return f
    }

    /// 中文黑体（苹方）
    static func zh(_ size: CGFloat, _ w: Zh = .semibold) -> NSFont {
        register()
        let key = "z|\(size)|\(w.psName)"
        if let f = cache[key] { return f }
        let f = NSFont(name: w.psName, size: size) ?? NSFont.systemFont(ofSize: size, weight: .semibold)
        cache[key] = f
        return f
    }
}

struct TextStyle {
    var font: NSFont
    var color: NSColor
    var tracking: CGFloat = 0
    var align: NSTextAlignment = .left
    var truncate: Bool = true

    init(_ font: NSFont, _ color: NSColor, tracking: CGFloat = 0) {
        self.font = font
        self.color = color
        self.tracking = tracking
    }

    var attributes: [NSAttributedString.Key: Any] {
        var a: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
        if tracking != 0 { a[.kern] = tracking }
        return a
    }
}

func attrString(_ s: String, _ st: TextStyle) -> NSAttributedString {
    NSAttributedString(string: s, attributes: st.attributes)
}

func textWidth(_ s: String, _ st: TextStyle) -> CGFloat {
    guard !s.isEmpty else { return 0 }
    return attrString(s, st).size().width
}

func textLineHeight(_ f: NSFont) -> CGFloat {
    f.ascender - f.descender + f.leading
}

/// 以基线为锚点画一行字（左对齐）
@discardableResult
func drawText(_ s: String, _ st: TextStyle, x: CGFloat, baseline: CGFloat) -> CGFloat {
    guard !s.isEmpty else { return 0 }
    let a = attrString(s, st)
    a.draw(at: CGPoint(x: x, y: baseline - st.font.ascender))
    return a.size().width
}

@discardableResult
func drawTextRight(_ s: String, _ st: TextStyle, right: CGFloat, baseline: CGFloat) -> CGFloat {
    let w = textWidth(s, st)
    drawText(s, st, x: right - w, baseline: baseline)
    return w
}

@discardableResult
func drawTextCenter(_ s: String, _ st: TextStyle, centerX: CGFloat, baseline: CGFloat) -> CGFloat {
    let w = textWidth(s, st)
    drawText(s, st, x: centerX - w / 2, baseline: baseline)
    return w
}

/// 在一个矩形里画（可截断），垂直居中
func drawTextIn(_ s: String, _ st: TextStyle, rect: CGRect, align: NSTextAlignment = .left) {
    guard !s.isEmpty, rect.width > 1 else { return }
    let p = NSMutableParagraphStyle()
    p.alignment = align
    p.lineBreakMode = st.truncate ? .byTruncatingTail : .byClipping
    var a = st.attributes
    a[.paragraphStyle] = p
    let str = NSAttributedString(string: s, attributes: a)
    let h = st.font.ascender - st.font.descender
    let r = CGRect(x: rect.minX,
                   y: rect.midY - h / 2,
                   width: rect.width,
                   height: h + 2)
    str.draw(in: r)
}

/// 垂直居中的基线位置
func centerBaseline(rect: CGRect, font: NSFont) -> CGFloat {
    rect.midY + (font.ascender + font.descender) / 2
}
