//
//  Model.swift —— 番茄钟状态机 / 待办存储 / 设置 / 提示音
//

import AppKit
import AVFoundation

// MARK: - 番茄钟

enum Phase: Int {
    case work = 0, shortBreak = 1, longBreak = 2

    var name: String {
        switch self {
        case .work: return "专注"
        case .shortBreak: return "短休息"
        case .longBreak: return "长休息"
        }
    }

    var latin: String {
        switch self {
        case .work: return "FOCUS"
        case .shortBreak: return "BREAK"
        case .longBreak: return "LONG BREAK"
        }
    }
}

struct Pomodoro {
    var phase: Phase = .work
    var remaining: Double = 25 * 60
    var total: Double = 25 * 60
    var running = false
    var completed = 0
    var workMin = 25
    var shortMin = 5
    var longMin = 15
    var longEvery = 4

    func seconds(_ p: Phase) -> Double {
        switch p {
        case .work: return Double(workMin) * 60
        case .shortBreak: return Double(shortMin) * 60
        case .longBreak: return Double(longMin) * 60
        }
    }

    mutating func reset() {
        phase = .work
        total = seconds(.work)
        remaining = total
        running = false
    }

    mutating func advance() {
        let next: Phase
        if phase == .work {
            completed += 1
            next = (completed % longEvery == 0) ? .longBreak : .shortBreak
        } else {
            next = .work
        }
        phase = next
        total = seconds(next)
        remaining = total
    }

    /// 超出的时间结转到下一阶段，不丢失
    @discardableResult
    mutating func tick(_ dt: Double) -> Bool {
        guard running, dt > 0 else { return false }
        var changed = false
        remaining -= dt
        var guardCount = 0
        while remaining <= 0 && guardCount < 10000 {
            let over = -remaining
            remaining = 0
            advance()
            changed = true
            guardCount += 1
            if total <= 0 { remaining = 0; break }
            remaining -= over
        }
        return changed
    }

    mutating func skip() { advance() }

    var nextPhase: Phase {
        if phase == .work { return (completed + 1) % longEvery == 0 ? .longBreak : .shortBreak }
        return .work
    }

    var nextPhaseMinutes: Int {
        switch nextPhase {
        case .work: return workMin
        case .shortBreak: return shortMin
        case .longBreak: return longMin
        }
    }

    /// 本轮已完成的专注数（0…longEvery）
    var groupDone: Int {
        var g = completed % longEvery
        if phase == .work && completed > 0 && g == 0 { g = longEvery }
        return g
    }

    var fraction: Double {
        total > 0 ? max(0, min(1, remaining / total)) : 0
    }
}

func fmtSeconds(_ sec: Double) -> String {
    let total = max(0, Int(ceil(sec - 1e-9)))
    return String(format: "%02d:%02d", total / 60, total % 60)
}

// MARK: - 待办

struct TodoItem {
    var text: String
    var done: Bool = false
}

final class TodoStore {
    var items: [TodoItem] = []
    var path: String = ""

    static let maxItems = 128
    static let maxChars = 160

    var doneCount: Int { items.filter { $0.done }.count }

    func load() {
        items = []
        guard !path.isEmpty, let raw = try? String(contentsOfFile: path, encoding: .utf8) else { return }
        for line in raw.split(separator: "\n", omittingEmptySubsequences: true) {
            guard items.count < Self.maxItems else { break }
            let s = String(line)
            let done = s.hasPrefix("1")
            guard let tab = s.firstIndex(of: "\t") else { continue }
            let text = String(s[s.index(after: tab)...])
            guard !text.isEmpty else { continue }
            items.append(TodoItem(text: String(text.prefix(Self.maxChars)), done: done))
        }
    }

    @discardableResult
    func save() -> Bool {
        guard !path.isEmpty else { return false }
        var out = ""
        for it in items {
            guard !it.text.isEmpty else { continue }
            out += "\(it.done ? 1 : 0)\t\(it.text)\n"
        }
        do {
            let dir = (path as NSString).deletingLastPathComponent
            if !dir.isEmpty { try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true) }
            try out.write(toFile: path, atomically: true, encoding: .utf8)
            return true
        } catch {
            return false
        }
    }

    @discardableResult
    func add(_ text: String) -> Int? {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty, items.count < Self.maxItems else { return nil }
        items.append(TodoItem(text: String(t.prefix(Self.maxChars))))
        return items.count - 1
    }

    func delete(_ i: Int) {
        guard items.indices.contains(i) else { return }
        items.remove(at: i)
    }

    func toggle(_ i: Int) {
        guard items.indices.contains(i) else { return }
        items[i].done.toggle()
    }

    func update(_ i: Int, _ text: String) {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard items.indices.contains(i), !t.isEmpty else { return }
        items[i].text = String(t.prefix(Self.maxChars))
    }
}

// MARK: - 设置

final class AppSettings {
    var theme: ThemeKind = .light
    var muted = false
    var opacityIndex = 0
    var bgImage: String = ""
    var listEvery = 30
    var listDur = 8

    private var path: String = ""

    /// --data-dir 指定的便携目录；不设就用标准位置
    static var overrideDir: String?

    static func supportDir() -> String {
        let base: String
        if let d = overrideDir {
            base = NSString(string: d).expandingTildeInPath
        } else {
            base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
                .appendingPathComponent("PomodoroClock").path
                ?? NSString(string: "~/Library/Application Support/PomodoroClock").expandingTildeInPath
        }
        try? FileManager.default.createDirectory(atPath: base, withIntermediateDirectories: true)
        return base
    }

    func initPaths() {
        path = Self.supportDir() + "/settings.json"
    }

    func load() {
        initPaths()
        guard let data = FileManager.default.contents(atPath: path),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
        if let t = obj["theme"] as? Int, let k = ThemeKind(rawValue: t) { theme = k }
        if let m = obj["muted"] as? Bool { muted = m }
        if let o = obj["opacity"] as? Int { opacityIndex = max(0, min(3, o)) }
        if let b = obj["bgImage"] as? String { bgImage = b }
        if let e = obj["listEvery"] as? Int { listEvery = e }
        if let d = obj["listDur"] as? Int { listDur = d }
    }

    var exists: Bool { !path.isEmpty && FileManager.default.fileExists(atPath: path) }

    func save() {
        let obj: [String: Any] = [
            "theme": theme.rawValue,
            "muted": muted,
            "opacity": opacityIndex,
            "bgImage": bgImage,
            "listEvery": listEvery,
            "listDur": listDur
        ]
        if let data = try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted]) {
            try? data.write(to: URL(fileURLWithPath: path))
        }
    }
}

// MARK: - 提示音（程序内合成，不依赖音频文件）

final class Chime {
    static let shared = Chime()
    private var player: AVAudioPlayer?
    private var data: Data?

    private func build() -> Data? {
        if let d = data { return d }
        let rate = 22050
        let dur = 1.75
        let samples = Int(Double(rate) * dur)
        var pcm = Data()
        let byteCount = samples * 2
        pcm.append(contentsOf: Array("RIFF".utf8))
        pcm.append(contentsOf: le32(UInt32(36 + byteCount)))
        pcm.append(contentsOf: Array("WAVEfmt ".utf8))
        pcm.append(contentsOf: le32(16))
        pcm.append(contentsOf: le16(1))
        pcm.append(contentsOf: le16(1))
        pcm.append(contentsOf: le32(UInt32(rate)))
        pcm.append(contentsOf: le32(UInt32(rate * 2)))
        pcm.append(contentsOf: le16(2))
        pcm.append(contentsOf: le16(16))
        pcm.append(contentsOf: Array("data".utf8))
        pcm.append(contentsOf: le32(UInt32(byteCount)))

        // E5 – G5 – C6：柔和的三度琶音 + 泛音 + 指数衰减
        let freq: [Double] = [659.26, 783.99, 1046.50]
        let start: [Double] = [0.00, 0.16, 0.32]
        let gain: [Double] = [0.55, 0.42, 0.34]
        var bytes = [UInt8](repeating: 0, count: byteCount)
        for i in 0..<samples {
            let t = Double(i) / Double(rate)
            var v = 0.0
            for n in 0..<3 {
                let lt = t - start[n]
                if lt < 0 { continue }
                var env = exp(-3.1 * lt)
                if lt < 0.014 { env *= lt / 0.014 }
                let w = sin(2 * .pi * freq[n] * lt)
                    + 0.30 * sin(2 * .pi * 2 * freq[n] * lt)
                    + 0.10 * sin(2 * .pi * 3 * freq[n] * lt)
                v += gain[n] * env * w
            }
            v *= 0.34
            let clamped = max(-1, min(1, v))
            let s = Int16(clamped * 32000)
            bytes[i * 2] = UInt8(truncatingIfNeeded: Int(s))
            bytes[i * 2 + 1] = UInt8(truncatingIfNeeded: Int(s) >> 8)
        }
        pcm.append(contentsOf: bytes)
        data = pcm
        return pcm
    }

    private func le16(_ v: UInt16) -> [UInt8] { [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)] }
    private func le32(_ v: UInt32) -> [UInt8] {
        [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8((v >> 24) & 0xFF)]
    }

    func play(muted: Bool) {
        guard !muted, let d = build() else { return }
        do {
            let p = try AVAudioPlayer(data: d)
            p.volume = 0.9
            p.prepareToPlay()
            p.play()
            player = p
        } catch {
            NSSound.beep()
        }
    }

    /// --test-chime 用：返回 (毫秒, 峰值%, RMS%)
    func stats() -> (Int, Double, Double) {
        guard let d = build(), d.count > 44 else { return (0, 0, 0) }
        let n = (d.count - 44) / 2
        var peak = 0.0
        var sum = 0.0
        d.withUnsafeBytes { raw in
            let s = raw.bindMemory(to: Int16.self)
            for i in 0..<n {
                let v = Double(s[(44 / 2) + i])
                peak = max(peak, abs(v))
                sum += v * v
            }
        }
        return (n * 1000 / 22050, peak * 100 / 32768, sqrt(sum / Double(n)) * 100 / 32768)
    }
}
