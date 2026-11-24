// Nothing is installed outside ~/Library/Developer/NativeNavi but the Metal toolchain; no administrator password, shell profile or xcode-select (DEVELOPER_DIR per tool run)
import CryptoKit
import Foundation

struct SetupConfig: Decodable, Sendable {
    struct Pin: Decodable, Sendable {
        let version: String
        let url: URL
        let sha256: String
    }
    struct KnownNUP: Decodable, Sendable, Equatable {
        let sha256: String
        let bytes: Int64
        let holds: String
    }
    let python: Pin
    let rustup: Pin
    let nups: [KnownNUP]
    let firstBuildGB: Int
    let commit: String
    let xcode: String           // own_compilers.py XCODE_VERSION
    let metal: String           // dmsh_chain.lock metal_macosx
    let xcodeDownload: URL
    let requirements: String
    let rustDir: String

    enum CodingKeys: String, CodingKey {
        case python, rustup, nups, commit, xcode, metal, requirements
        case firstBuildGB = "first_build_gb", xcodeDownload = "xcode_download", rustDir = "rust_dir"
    }

    static let current: SetupConfig = {
        guard let url = Bundle.main.url(forResource: "setup", withExtension: "json"), let data = try? Data(contentsOf: url) else {
            fatalError("Resources/setup.json is missing: pack the app with scripts/build_gui.py")
        }
        do { return try JSONDecoder().decode(SetupConfig.self, from: data) } catch { fatalError("Resources/setup.json: \(error)") }
    }()
}

enum Paths {
    static let home = FileManager.default.homeDirectoryForCurrentUser
    /// No space in any of these: the host's Makefiles take none (dm/host/rust.mk)
    static let root = home.appending(path: "Library/Developer/NativeNavi", directoryHint: .isDirectory)
    static let source = root.appending(path: "Source", directoryHint: .isDirectory)
    static let tools = root.appending(path: "Tools", directoryHint: .isDirectory)
    static let python = tools.appending(path: "python", directoryHint: .isDirectory)
    static let rustupHome = tools.appending(path: "rustup", directoryHint: .isDirectory)
    static let cargoHome = tools.appending(path: "cargo", directoryHint: .isDirectory)
    static let downloads = tools.appending(path: "Downloads", directoryHint: .isDirectory)
    static let defaultBuild = root.appending(path: "Build", directoryHint: .isDirectory)
}

struct Xcode: Hashable, Sendable {
    let app: URL
    let version: String
    var developer: URL { app.appending(path: "Contents/Developer", directoryHint: .isDirectory) }
}

struct Space: Sendable {
    let free: Int64
    let volume: String
}

enum Toolchain {
    static var fm: FileManager { .default }
    static var python3: URL { Paths.python.appending(path: "bin/python3") }
    static var cargo: URL { Paths.cargoHome.appending(path: "bin/cargo") }
    private static var pythonStamp: URL { Paths.python.appending(path: ".nativenavi-python") }

    static func environment(_ xcode: Xcode?) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        let dropped: Set = ["VIRTUAL_ENV", "RUSTUP_TOOLCHAIN", "CARGO_TARGET_DIR", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET"]
        for k in Array(env.keys) where k.hasPrefix("PYTHON") || k.hasPrefix("CONDA") || dropped.contains(k) { env[k] = nil }
        env["PATH"] = ([Paths.cargoHome.appending(path: "bin").path, Paths.python.appending(path: "bin").path] + ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]).joined(separator: ":")
        if let xcode { env["DEVELOPER_DIR"] = xcode.developer.path }
        env["RUSTUP_HOME"] = Paths.rustupHome.path
        env["CARGO_HOME"] = Paths.cargoHome.path
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONNOUSERSITE"] = "1"   // a package in ~/.local must not shadow the pinned ones
        env["PYTHONUTF8"] = "1"
        env["PIP_DISABLE_PIP_VERSION_CHECK"] = "1"
        return env
    }

    // MARK: Xcode

    static func xcodes() async -> [Xcode] {
        var paths = Set<String>()
        if let found = await capture("/usr/bin/mdfind", ["kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'"]) {
            for l in found.split(separator: "\n") where l.hasSuffix(".app") && !l.contains("/.Trash") { paths.insert(String(l)) }
        }
        for dir in [URL(filePath: "/Applications"), Paths.home.appending(path: "Applications")] {
            for u in (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] where u.pathExtension == "app" && u.lastPathComponent.hasPrefix("Xcode") {
                paths.insert(u.path)
            }
        }
        if let selected = await capture("/usr/bin/xcode-select", ["-p"]), selected.hasSuffix(".app/Contents/Developer") {
            paths.insert(String(selected.dropLast("/Contents/Developer".count)))
        }
        var seen = Set<String>()
        return paths.compactMap { path -> Xcode? in
            let app = URL(filePath: path, directoryHint: .isDirectory).standardizedFileURL
            guard seen.insert(app.path).inserted, let info = NSDictionary(contentsOf: app.appending(path: "Contents/Info.plist")),
                  info["CFBundleIdentifier"] as? String == "com.apple.dt.Xcode", let version = info["CFBundleShortVersionString"] as? String else { return nil }
            return Xcode(app: app, version: version)
        }.sorted { $0.version.compare($1.version, options: .numeric) == .orderedDescending }
    }

    static func licenseAccepted(_ env: [String: String]) async -> Bool {
        (try? await runTool(URL(filePath: "/usr/bin/xcodebuild"), ["-license", "check"], env: env)) == 0
    }

    static func metalVersion(_ env: [String: String]) async -> String? {
        await capture("/usr/bin/xcrun", ["-sdk", "macosx", "metal", "--version"], env: env)?.split(separator: "\n").first.map(String.init)
    }

    static func installMetal(_ env: [String: String], progress: @escaping @MainActor @Sendable (Double?, String) -> Void) async throws {
        let tail = Tail()
        let status = try await runTool(URL(filePath: "/usr/bin/xcodebuild"), ["-downloadComponent", "metalToolchain"], env: env) { _, line in
            tail.add(line)
            let pct = line.firstMatch(of: /(\d+(?:\.\d+)?)\s*%/).flatMap { Double($0.1) }
            progress(pct.map { $0 / 100 }, line)
        }
        guard status == 0 else { throw ToolFailure("Apple's Metal toolchain could not be downloaded (xcodebuild exit status \(status)): \(tail.last)") }
    }

    // MARK: Python

    static func pythonReady(_ cfg: SetupConfig) -> Bool {
        fm.isExecutableFile(atPath: python3.path) && (try? String(contentsOf: pythonStamp, encoding: .utf8)) == cfg.python.sha256
    }

    static func installPython(_ cfg: SetupConfig, progress: @escaping @MainActor @Sendable (Double?, String) -> Void) async throws {
        let file = Paths.downloads.appending(path: "python.tar.gz")
        try await fetch(cfg.python, to: file, what: "Python", progress: progress)
        await progress(nil, "Unpacking")
        let partial = Paths.tools.appending(path: "python.partial", directoryHint: .isDirectory)
        try? fm.removeItem(at: partial)
        try fm.createDirectory(at: partial, withIntermediateDirectories: true)
        guard try await runTool(URL(filePath: "/usr/bin/tar"), ["-xzf", file.path, "-C", partial.path], env: ProcessInfo.processInfo.environment) == 0 else {
            throw ToolFailure("Python could not be unpacked.")
        }
        try? fm.removeItem(at: Paths.python)
        try fm.moveItem(at: partial.appending(path: "python"), to: Paths.python)
        try? fm.removeItem(at: partial)
        try? fm.removeItem(at: file)
        try cfg.python.sha256.write(to: pythonStamp, atomically: true, encoding: .utf8)
    }

    static func installPackages(source: URL, _ cfg: SetupConfig, env: [String: String], line: @escaping @MainActor @Sendable (String) -> Void) async throws {
        let tail = Tail()
        let req = source.appending(path: cfg.requirements)
        let status = try await runTool(python3, ["-m", "pip", "install", "--no-warn-script-location", "--progress-bar", "off", "--only-binary", ":all:", "-r", req.path], env: env) { _, text in
            tail.add(text); line(text)
        }
        guard status == 0 else { throw ToolFailure("The Python packages could not be installed (pip exit status \(status)): \(tail.last)") }
    }

    // MARK: Rust

    static var rustupReady: Bool { fm.isExecutableFile(atPath: cargo.path) && fm.isExecutableFile(atPath: Paths.cargoHome.appending(path: "bin/rustup").path) }

    static func installRustup(_ cfg: SetupConfig, env: [String: String], progress: @escaping @MainActor @Sendable (Double?, String) -> Void) async throws {
        let file = Paths.downloads.appending(path: "rustup-init")
        try await fetch(cfg.rustup, to: file, what: "rustup", progress: progress)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: file.path)
        let tail = Tail()
        let status = try await runTool(file, ["-y", "--no-modify-path", "--profile", "minimal", "--default-toolchain", "none"], env: env) { _, line in
            tail.add(line); progress(nil, line)
        }
        try? fm.removeItem(at: file)
        guard status == 0 else { throw ToolFailure("rustup could not be installed (exit status \(status)): \(tail.last)") }
    }

    static func installRust(source: URL, _ cfg: SetupConfig, env: [String: String], line: @escaping @MainActor @Sendable (String) -> Void) async throws {
        let tail = Tail()
        let status = try await runTool(cargo, ["--version"], env: env, cwd: source.appending(path: cfg.rustDir)) { _, text in
            tail.add(text); line(text)
        }
        guard status == 0 else { throw ToolFailure("Rust could not be installed (exit status \(status)): \(tail.last)") }
    }

    private static func fetch(_ pin: SetupConfig.Pin, to file: URL, what: String, progress: @escaping @MainActor @Sendable (Double?, String) -> Void) async throws {
        let ticker = Ticker()
        try await Download.fetch(pin.url, to: file) { written, expected in
            guard ticker.due() else { return }
            let text = expected > 0 ? "\(bytes(written)) of \(bytes(expected))" : bytes(written)
            Task { @MainActor in progress(expected > 0 ? Double(written) / Double(expected) : nil, text) }
        }
        guard try sha256(of: file) == pin.sha256 else {
            try? fm.removeItem(at: file)
            throw ToolFailure("The \(what) download is damaged: its SHA-256 is not the one this app expects. Try again.")
        }
    }

    // MARK: The sources

    /// NATIVENAVI_SOURCE names a checkout to use instead, for development
    static func prepareSource(_ cfg: SetupConfig) async throws -> URL {
        if let dev = ProcessInfo.processInfo.environment["NATIVENAVI_SOURCE"], !dev.isEmpty { return URL(filePath: dev, directoryHint: .isDirectory) }
        let stamp = Paths.source.appending(path: ".source-commit")   // dm/mac/pack_app.py reads it for the game app's DMCommit
        if (try? String(contentsOf: stamp, encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines) == cfg.commit { return Paths.source }
        guard let archive = Bundle.main.url(forResource: "source", withExtension: "tar.gz") else {
            throw ToolFailure("This copy of NativeNavi has no build scripts in it (Resources/source.tar.gz). Download it again.")
        }
        let fresh = Paths.root.appending(path: "Source.partial", directoryHint: .isDirectory)
        let old = Paths.root.appending(path: "Source.old", directoryHint: .isDirectory)
        try? fm.removeItem(at: fresh)
        try fm.createDirectory(at: fresh, withIntermediateDirectories: true)
        guard try await runTool(URL(filePath: "/usr/bin/tar"), ["-xzf", archive.path, "-C", fresh.path], env: ProcessInfo.processInfo.environment) == 0 else {
            throw ToolFailure("The build scripts could not be unpacked.")
        }
        try (cfg.commit + "\n").write(to: fresh.appending(path: ".source-commit"), atomically: true, encoding: .utf8)
        try? fm.removeItem(at: old)
        if fm.fileExists(atPath: Paths.source.path) { try fm.moveItem(at: Paths.source, to: old) }
        try fm.moveItem(at: fresh, to: Paths.source)
        Task.detached { try? FileManager.default.removeItem(at: old) }
        return Paths.source
    }

    // MARK: Files

    static func isNUP(_ url: URL) -> Bool {
        guard let h = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? h.close() }
        return (try? h.read(upToCount: 3)) == Data("NUP".utf8)
    }

    /// A NUP's SHA-256 is kept under it, so a file is hashed again only when it changed
    struct FileIdentity: Hashable, Sendable {
        let path: String
        let size: Int64
        let mtime: Int64
        let inode: UInt64
        var key: String { "\(path)\n\(size)\n\(mtime)\n\(inode)" }
    }

    static func identity(_ url: URL) -> FileIdentity? {
        var st = stat()
        guard stat(url.path, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG else { return nil }
        return FileIdentity(path: url.path, size: Int64(st.st_size), mtime: Int64(st.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(st.st_mtimespec.tv_nsec), inode: UInt64(st.st_ino))
    }

    /// A cloud drive's online-only file: reading it downloads it first
    static func dataless(_ url: URL) -> Bool {
        var st = stat()
        return stat(url.path, &st) == 0 && st.st_flags & 0x4000_0000 != 0   // SF_DATALESS, sys/stat.h
    }

    static func space(at url: URL) -> Space? {
        var probe = url
        while !fm.fileExists(atPath: probe.path) && probe.pathComponents.count > 1 { probe.deleteLastPathComponent() }
        guard let v = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeLocalizedNameKey]),
              let free = v.volumeAvailableCapacityForImportantUsage else { return nil }
        return Space(free: free, volume: v.volumeLocalizedName ?? "this disk")
    }

    /// APFS clones count in full, so a removal can free less
    static func size(of url: URL) -> Int64 {
        guard let e = fm.enumerator(at: url, includingPropertiesForKeys: [.totalFileAllocatedSizeKey], options: [], errorHandler: { _, _ in true }) else { return 0 }
        var total: Int64 = 0
        while let u = e.nextObject() as? URL {
            if let s = try? u.resourceValues(forKeys: [.totalFileAllocatedSizeKey]).totalFileAllocatedSize { total += Int64(s) }
        }
        return total
    }

    static func excludeFromBackup(_ url: URL) {
        var u = url
        var v = URLResourceValues()
        v.isExcludedFromBackup = true
        try? u.setResourceValues(v)
    }
}

func bytes(_ n: Int64) -> String { n.formatted(.byteCount(style: .file)) }

final class Tail: @unchecked Sendable {
    private var lines: [String] = []
    private let lock = NSLock()
    func add(_ s: String) {
        lock.withLock {
            lines.append(s)
            if lines.count > 20 { lines.removeFirst() }
        }
    }
    var last: String { lock.withLock { lines.last(where: { !$0.trimmingCharacters(in: .whitespaces).isEmpty }) ?? "no output" } }
}

final class Ticker: @unchecked Sendable {
    private var next = Date.distantPast
    private let lock = NSLock()
    func due() -> Bool {
        lock.withLock {
            let now = Date.now
            guard now >= next else { return false }
            next = now.addingTimeInterval(0.1)
            return true
        }
    }
}

@concurrent func hashFile(_ url: URL, size: Int64, progress: @escaping @Sendable (Double) -> Void) async throws -> String {
    let h = try FileHandle(forReadingFrom: url)
    defer { try? h.close() }
    var hasher = SHA256()
    var done: Int64 = 0
    let ticker = Ticker()
    while let data = try h.read(upToCount: 8 << 20), !data.isEmpty {
        try Task.checkCancellation()
        hasher.update(data: data)
        done += Int64(data.count)
        if size > 0 && ticker.due() { progress(Double(done) / Double(size)) }
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}
