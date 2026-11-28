import Foundation
import Combine

struct Setting: Decodable, Identifiable {
    let env, kind, label, note, def: String
    let section: String?
    let needs: String?     // "<VAR>=<value>": drawn only while that setting has that value
    let level: String?     // "basic": Basic settings; any other or none: Advanced
    let test: String?      // "sticks": a live stick test under the row (LauncherSticks.swift)
    let options: [[String]]?
    let min, max, step: Double?
    let on, off: String?   // 1 / 0 when not given
    var id: String { env }
    var sectionTitle: String { section ?? "Game" }
    var basic: Bool { level == "basic" }
    var onValue: String { on ?? "1" }
    var offValue: String { off ?? "0" }
    func isDefault(_ v: String) -> Bool {   // a slider writes 1.00 for 1.0
        kind == "range" ? abs((Double(v) ?? .nan) - (Double(def) ?? .nan)) < 1e-6 : v == def
    }
    func accepts(_ v: String) -> Bool {
        switch kind {
        case "choice": return options?.contains { $0[0] == v } ?? false
        case "range":
            guard !v.contains(where: { $0 == "x" || $0 == "X" }), let n = Double(v), n.isFinite, let lo = min, let hi = max else { return false }   // no hex
            return n >= lo && n <= hi
        case "toggle": return v == onValue || v == offValue
        default: return true
        }
    }
    enum CodingKeys: String, CodingKey { case env, kind, label, note, def = "default", section, needs, level, test, options, min, max, step, on, off }
}
struct SettingsFile: Decodable { let settings: [Setting] }

final class LauncherModel: ObservableObject {
    let settings: [Setting]
    let fixed: Set<String>      // set by an env file or the launch environment: shown, not changed
    let fromLaunch: Set<String>
    let ignoreSaved: Bool   // DM_SAVED_CHOICES=0
    @Published var values: [String: String] = [:]

    private let defaults: UserDefaults

    init(_ settings: [Setting], defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.settings = settings
        let env = ProcessInfo.processInfo.environment
        fromLaunch = Set((env["DM_ENV_LAUNCH_KEYS"] ?? "").split(separator: " ").map(String.init))
        fixed = fromLaunch.union((env["DM_ENV_FILE_KEYS"] ?? "").split(separator: " ").map(String.init))
        ignoreSaved = env["DM_SAVED_CHOICES"] == "0"
        if ignoreSaved { fputs("launcher: saved choices ignored (DM_SAVED_CHOICES=0)\n", stderr) }
        for s in settings {
            let raw = ignoreSaved ? nil : defaults.object(forKey: "launcher." + s.env)
            if fixed.contains(s.env), let v = env[s.env] {
                values[s.env] = v
                if let saved = raw as? String { fputs("launcher: saved choice \(s.env)=\(saved) not used: \(s.env)=\(v) is set (\(fromLaunch.contains(s.env) ? "launch environment" : "env file"))\n", stderr) }
                continue
            }
            values[s.env] = s.def
            guard let raw else { continue }
            if let saved = raw as? String, s.accepts(saved) { values[s.env] = saved }   // not string(forKey:), which turns a number into a string
            else if let saved = raw as? String { fputs("launcher: saved choice \(s.env)=\(saved) ignored: this build does not offer it\n", stderr) }
            else { fputs("launcher: saved choice for \(s.env) ignored: not a string\n", stderr) }
        }
    }

    // one naming a setting this game does not have stays drawn
    func visible(_ s: Setting) -> Bool {
        guard let n = s.needs, let eq = n.firstIndex(of: "=") else { return true }
        let key = String(n[n.startIndex..<eq]), want = String(n[n.index(after: eq)...])
        guard let v = values[key] else { return true }
        return v == want
    }
    func rows(in section: String) -> [Setting] { settings.filter { $0.sectionTitle == section && visible($0) } }
    func sections(basic: Bool) -> [String] {
        settings.filter { $0.basic == basic }.reduce(into: []) { if !$0.contains($1.sectionTitle) { $0.append($1.sectionTitle) } }
    }
    func rows(in section: String, basic: Bool) -> [Setting] { rows(in: section).filter { $0.basic == basic } }
    var changed: Bool { settings.contains { !fixed.contains($0.env) && !$0.isDefault(values[$0.env] ?? $0.def) } }

    // on screen only: Quit after Reset keeps the saved choices
    func reset() {
        for s in settings where !fixed.contains(s.env) { values[s.env] = s.def }
        fputs("launcher: settings reset to the game's defaults\n", stderr)
    }

    // Every setting goes into the environment, on every start. persist (Play): only a choice that differs from the default is stored, so a setting left at its default follows a
    // later build's defaults; without the window nothing is stored
    func apply(persist: Bool) {
        for s in settings {
            let v = values[s.env] ?? s.def
            if fixed.contains(s.env) {
                fputs("launcher: \(s.env) kept at \(v) (\(fromLaunch.contains(s.env) ? "launch environment" : "env file"))\n", stderr)
                continue
            }
            if persist { store(s, v) }
            setenv(s.env, v, 1)
            fputs("launcher: \(s.env)=\(v)\(s.isDefault(v) ? "" : persist ? " (changed)" : " (saved choice)")\n", stderr)
        }
    }
    private func store(_ s: Setting, _ v: String) {
        guard !ignoreSaved else { return }
        if s.isDefault(v) { defaults.removeObject(forKey: "launcher." + s.env) } else { defaults.set(v, forKey: "launcher." + s.env) }
    }
}
