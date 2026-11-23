// The app's state and what it does: the game file, the checks of what the build needs, the tool downloads, the build itself (scripts/build_app.py --progress json, whose events
// docs/frontends.md describes) and the app it made. The player's choices and the last app built are kept in the app's preferences
import AppKit
import Foundation
import UniformTypeIdentifiers

struct Check: Equatable {
    enum Level { case checking, ok, later, waiting, action, warning, problem }   // later: this app installs it when the build starts; waiting: on another check
    var level: Level
    var detail: String
    static let checking = Check(level: .checking, detail: "Checking…")
}

enum XcodeAction: Equatable {
    case download
    case open(URL)
}

struct Step: Identifiable, Equatable {
    enum State { case running, done, warning, failed }
    let id = UUID()
    var title: String
    var state: State = .running
    var summary = ""
}

struct Activity: Equatable {
    var title: String
    var position: String
    var fraction: Double?
    var detail = ""
    var eta: Double?
}

struct BuiltApp: Codable, Equatable {
    var path: String
    var commit: String
    var name: String
    var version: String
    var url: URL { URL(filePath: path, directoryHint: .isDirectory) }
}

/// One line of the packing script's --progress json output; each event uses some of the fields
private struct BuildEvent: Decodable {
    let ev: String
    let k: Int?, n: Int?
    let name: String?, step: String?
    let done: Double?, total: Double?, unit: String?, est: Bool?, note: String?, frac: Double?, eta: Double?, elapsed: Double?
    let ok: Bool?, summary: String?, warn: Bool?
    let msg: String?
    let version: String?, out: String?, icon: String?
}

@MainActor @Observable final class AppModel {
    enum Screen { case setup, working, finished, failed, stopped }

    let cfg = SetupConfig.current
    var screen: Screen = .setup

    /// The chosen file and what is known of it: only a NUP the profile lists (nup.tsv, by SHA-256 and size) is built from
    enum GameCheck: Equatable {
        case none
        case verifying(Double?, String)
        case verified(SetupConfig.KnownNUP)
        case problem(String)
    }
    var game: URL? { didSet { saveGame(); checkGame() } }
    var gameCheck = GameCheck.none
    var unknownGame: String?   // the alert after a file turned out to be no version this app builds
    private var gameIdentity: Toolchain.FileIdentity?
    private var verifying: Task<Void, Never>?
    var gameProblem: String? { if case .problem(let p) = gameCheck { p } else { nil } }
    var gameVerified: Bool { if case .verified = gameCheck { true } else { false } }

    var xcode: Xcode?
    var xcodeCheck = Check.checking
    var xcodeAction: XcodeAction?
    var metalCheck = Check.checking
    var toolsCheck = Check.checking
    var diskCheck = Check.checking
    private var metalReady = false
    private var refreshing: Task<Void, Never>?

    var destination: URL { didSet { UserDefaults.standard.set(destination.path, forKey: Keys.destination); checkDisk() } }
    var buildParent: URL? { didSet { UserDefaults.standard.set(buildParent?.path, forKey: Keys.buildParent); checkDisk() } }

    var steps: [Step] = []
    var activity: Activity?
    var lines: [String] = []
    var gameName: String?
    var gameIcon: URL?
    var failure: String?
    var built: BuiltApp? { didSet { saveBuilt() } }
    var started: Date?

    var confirmStop = false
    var confirmSpace = false
    private var job: Task<Void, Never>?
    private var outApp: URL?
    private var lastRed: String?
    private var stderrTail = Tail()

    private enum Keys {
        static let game = "game.bookmark", destination = "destination.path", buildParent = "build.parent", built = "built.app", hashes = "game.sha256"
    }

    init() {
        let d = UserDefaults.standard
        destination = d.string(forKey: Keys.destination).map { URL(filePath: $0, directoryHint: .isDirectory) } ?? AppModel.defaultDestination
        buildParent = d.string(forKey: Keys.buildParent).map { URL(filePath: $0, directoryHint: .isDirectory) }
        if let data = d.data(forKey: Keys.built), let b = try? JSONDecoder().decode(BuiltApp.self, from: data) { built = b }
        if let data = d.data(forKey: Keys.game) {
            var stale = false
            game = try? URL(resolvingBookmarkData: data, options: [.withoutUI], bookmarkDataIsStale: &stale)
            unknownGame = nil   // the alert is for a file chosen now; the game's row says what is wrong with the one from last time
        }
        if let built, FileManager.default.fileExists(atPath: built.path) { screen = .finished } else { built = nil }
    }

    // MARK: Places

    /// /Applications when this user may write there (an administrator), else ~/Applications
    static var defaultDestination: URL {
        FileManager.default.isWritableFile(atPath: "/Applications") ? URL(filePath: "/Applications", directoryHint: .isDirectory)
            : Paths.home.appending(path: "Applications", directoryHint: .isDirectory)
    }

    /// The packing script's build folder: kept from build to build, so that a later build redoes only what changed
    var buildFolder: URL { buildParent.map { $0.appending(path: "nativenavi-build", directoryHint: .isDirectory) } ?? Paths.defaultBuild }

    var isBusy: Bool { job != nil }
    var updateAvailable: Bool { built.map { $0.commit != cfg.commit } ?? false }
    var canBuild: Bool { !isBusy && gameVerified && xcodeCheck.level == .ok && metalCheck.level != .problem }

    var hint: String {
        if game == nil { return "Choose your game file to begin." }
        if case .verifying = gameCheck { return "Checking your game file…" }
        if !gameVerified { return "Choose a copy of the game that NativeNavi knows." }
        if xcodeCheck.level == .checking { return "Checking this Mac…" }
        if xcodeCheck.level != .ok { return "Xcode \(cfg.xcode) is needed first." }
        if metalCheck.level == .problem { return "This Xcode can't build the game's shaders." }
        return FileManager.default.fileExists(atPath: buildFolder.path) ? "This build reuses the earlier one's work." : "A first build takes a few hours."
    }

    // MARK: The game file

    func choose(_ url: URL?) {
        guard var url, !isBusy else { return }
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue {   // a folder: its one .nup, as the packing script takes it
            let nups = ((try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? [])
                .filter { $0.pathExtension.lowercased() == "nup" && !$0.lastPathComponent.hasPrefix("._") }
            if nups.count == 1 { url = nups[0] }
        }
        game = url
    }

    func chooseGame() {
        guard !isBusy else { return }
        Task {
            let types = UTType(filenameExtension: "nup").map { [$0] }
            guard let url = await Panels.choose(types: types, folder: false, message: "Choose the .nup file of Ocarina of Time for Nintendo Switch 2.", prompt: "Choose") else { return }
            choose(url)
            if screen != .setup { backToSetup() }
        }
    }

    func chooseDestination() {
        Task {
            if let url = await Panels.choose(folder: true, message: "Choose the folder to put the game's app in.", prompt: "Choose") { destination = url }
        }
    }

    func chooseBuildParent() {
        Task {
            if let url = await Panels.choose(folder: true, message: "Choose where to keep the build files: a folder named nativenavi-build goes in it. Pick a drive with about \(cfg.firstBuildGB) GB free.", prompt: "Choose") {
                buildParent = url
                measure()
            }
        }
    }

    /// The chosen file against the profile's list: its size first, then its SHA-256 (kept under the file's identity, so that an unchanged file is not read again).
    /// Unchanged since the last check: nothing to do
    private func checkGame() {
        guard let game else { setGameCheck(.none, nil); return }
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: game.path, isDirectory: &isDir) else {
            setGameCheck(.problem("NativeNavi can't find this file. If it is on an external drive, connect it."), nil); return
        }
        guard !isDir.boolValue, let id = Toolchain.identity(game) else {
            setGameCheck(.problem("This folder doesn't hold exactly one .nup file. Choose the file itself."), nil); return
        }
        if id == gameIdentity && gameCheck != .none { return }
        guard Toolchain.dataless(game) || Toolchain.isNUP(game) else {   // a file only in the cloud is read when it is hashed, not for its first bytes
            setGameCheck(.problem("This isn't a NUP container. The game must be a .nup file."), id); return
        }
        let candidates = cfg.nups.filter { $0.bytes == id.size }
        if candidates.isEmpty { reject(id); return }
        if let sha = verifiedHashes[id.key] {
            if let known = candidates.first(where: { $0.sha256 == sha }) { setGameCheck(.verified(known), id) } else { reject(id) }
            return
        }
        let cloud = Toolchain.dataless(game)
        let what = cloud ? "Downloading it from your cloud drive and checking it…" : "Checking that it's a copy NativeNavi knows…"
        setGameCheck(.verifying(cloud ? nil : 0, what), id)
        verifying = Task {
            do {
                let sha = try await hashFile(game, size: id.size) { f in Task { @MainActor in
                    if case .verifying = self.gameCheck, self.gameIdentity == id { self.gameCheck = .verifying(f, "Checking that it's a copy NativeNavi knows…") }
                } }
                guard gameIdentity == id else { return }
                verifiedHashes[id.key] = sha
                if let known = candidates.first(where: { $0.sha256 == sha }) { setGameCheck(.verified(known), id) } else { reject(id, afterHash: true) }
            } catch is CancellationError {
            } catch {
                guard gameIdentity == id else { return }
                setGameCheck(.problem("NativeNavi can't read this file: \(error.localizedDescription)"), id)
            }
        }
    }

    private func setGameCheck(_ check: GameCheck, _ id: Toolchain.FileIdentity?) {
        if id != gameIdentity { verifying?.cancel(); verifying = nil }
        gameIdentity = id
        gameCheck = check
    }

    private func reject(_ id: Toolchain.FileIdentity, afterHash: Bool = false) {   // afterHash: a check that took a while, which the player may have left
        let known = ListFormatter.localizedString(byJoining: cfg.nups.map(\.holds))
        setGameCheck(.problem("This file isn't a version of the game that NativeNavi has verified."), id)
        unknownGame = "NativeNavi builds only from the copies it was tested with: \(known). This file's contents are different: it may be another version or dump, or be incomplete or changed. Make a NUP of your game again, then choose the new file."
        if afterHash { Notify.post(title: "Your game file can't be used", body: "It isn't a version of the game that NativeNavi has verified.") }
    }

    /// SHA-256 by file identity (FileIdentity.key), for the files already hashed
    private var verifiedHashes: [String: String] {
        get { UserDefaults.standard.dictionary(forKey: Keys.hashes) as? [String: String] ?? [:] }
        set { UserDefaults.standard.set(newValue.count > 20 ? newValue.filter { $0.key.hasPrefix((game?.path ?? "") + "\n") } : newValue, forKey: Keys.hashes) }
    }

    private func saveGame() {
        UserDefaults.standard.set(try? game?.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil), forKey: Keys.game)
    }

    private func saveBuilt() {
        UserDefaults.standard.set(built.flatMap { try? JSONEncoder().encode($0) }, forKey: Keys.built)
    }

    // MARK: Checks

    func refreshIfIdle() {
        guard !isBusy, refreshing == nil else { return }
        refreshing = Task { await refresh(); refreshing = nil }
    }

    func refresh() async {
        checkGame()
        let want = cfg.xcode
        let all = await Toolchain.xcodes()
        if let x = all.first(where: { $0.version == want }) ?? all.first(where: { $0.version.hasPrefix(want + ".") }) {
            xcode = x
            let env = Toolchain.environment(x)
            if await Toolchain.licenseAccepted(env) {
                xcodeCheck = Check(level: .ok, detail: x.version == want ? "Installed in \(x.app.deletingLastPathComponent().path)." : "Xcode \(x.version), in \(x.app.deletingLastPathComponent().path).")
                xcodeAction = nil
                switch await Toolchain.metalVersion(env) {
                case nil:
                    metalReady = false
                    metalCheck = Check(level: .later, detail: "Downloaded from Apple when you build.")
                case cfg.metal:
                    metalReady = true
                    metalCheck = Check(level: .ok, detail: "Installed.")
                case let v?:
                    metalReady = true
                    metalCheck = Check(level: .problem, detail: "This Xcode's Metal compiler (\(v)) isn't the one the game's shaders were tested with (\(cfg.metal)). Install Xcode \(want) from Apple.")
                }
            } else {
                xcodeCheck = Check(level: .action, detail: "Xcode needs to finish setting up. Open it once, agree to its license and let it install its components, then come back.")
                xcodeAction = .open(x.app)
                metalCheck = Check(level: .waiting, detail: "Checked once Xcode is set up.")
            }
        } else {
            xcode = nil
            let others = all.map { "Xcode \($0.version)" }
            xcodeCheck = Check(level: .action, detail: others.isEmpty
                ? "Apple's free developer tools build the game's code and shaders. Download Xcode \(want) with your Apple Account, open the .xip and move Xcode to Applications."
                : "This Mac has \(ListFormatter.localizedString(byJoining: others)), but the game's shaders were tested with Xcode \(want)'s Metal compiler. Install Xcode \(want) beside it; you can keep the other one.")
            xcodeAction = .download
            metalCheck = Check(level: .waiting, detail: "Checked once Xcode \(want) is installed.")
        }
        let python = Toolchain.pythonReady(cfg), rust = Toolchain.rustupReady
        toolsCheck = python && rust ? Check(level: .ok, detail: "Python \(cfg.python.version) and Rust are ready.")
            : Check(level: .later, detail: "Downloaded into NativeNavi's own folder when you build. Nothing else on your Mac changes.")
        checkDisk()
    }

    func checkDisk() {
        let folder = buildFolder
        guard let space = Toolchain.space(at: folder) else { diskCheck = Check(level: .warning, detail: "The free space of \(folder.path) can't be read."); return }
        let need = Int64(cfg.firstBuildGB) * 1_000_000_000
        let earlier = ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).contains("extract")
        if earlier {
            diskCheck = Check(level: .ok, detail: "\(bytes(space.free)) available on \(space.volume). The earlier build's files are reused.")
        } else if space.free >= need {
            diskCheck = Check(level: .ok, detail: "\(bytes(space.free)) available on \(space.volume); a first build uses about \(cfg.firstBuildGB) GB.")
        } else {
            diskCheck = Check(level: .warning, detail: "A first build uses about \(cfg.firstBuildGB) GB, and \(space.volume) has \(bytes(space.free)) available. Free up space, or keep the build files on another drive (Settings).")
        }
    }

    // MARK: The build

    func requestBuild() {
        guard canBuild else { return }
        if diskCheck.level == .warning { confirmSpace = true } else { build() }
    }

    func build() {
        guard canBuild, let game, let xcode else { return }
        guard let id = Toolchain.identity(game), id == gameIdentity else { checkGame(); return }   // the file changed since it was checked
        let env = Toolchain.environment(xcode)
        screen = .working
        steps = []; lines = []; failure = nil; lastRed = nil; outApp = nil; stderrTail = Tail()
        gameName = nil; gameIcon = nil; activity = nil; started = .now
        Notify.ask()
        let busy = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled, .suddenTerminationDisabled], reason: "Building the game")
        job = Task {
            defer {
                ProcessInfo.processInfo.endActivity(busy)
                DockProgress.hide()
                job = nil
            }
            do {
                try await run(game: game, env: env)
            } catch is CancellationError {
                finishStep(.failed)
                screen = .stopped
            } catch {
                finishStep(.failed)
                fail(error.localizedDescription)
            }
        }
    }

    func stop() { job?.cancel() }

    func stopAndWait() async {
        job?.cancel()
        await job?.value
    }

    private func run(game: URL, env: [String: String]) async throws {
        try FileManager.default.createDirectory(at: Paths.root, withIntermediateDirectories: true)
        Toolchain.excludeFromBackup(Paths.root)
        let source = try await step("Unpacking the build scripts") { try await Toolchain.prepareSource(cfg) }
        if !metalReady {
            try await step("Downloading Apple's Metal toolchain") {
                try await Toolchain.installMetal(env) { [weak self] f, line in self?.progress(f, line) }
            }
            metalReady = true
        }
        if !Toolchain.pythonReady(cfg) {
            try await step("Downloading Python \(cfg.python.version)") {
                try await Toolchain.installPython(cfg) { [weak self] f, text in self?.progress(f, text) }
            }
        }
        try await step("Installing Python packages") {
            try await Toolchain.installPackages(source: source, cfg, env: env) { [weak self] line in self?.progress(nil, line) }
        }
        if !Toolchain.rustupReady {
            try await step("Downloading Rust") {
                try await Toolchain.installRustup(cfg, env: env) { [weak self] f, text in self?.progress(f, text) }
            }
        }
        try await step("Installing the Rust compiler") {
            try await Toolchain.installRust(source: source, cfg, env: env) { [weak self] line in self?.progress(nil, line) }
        }
        toolsCheck = Check(level: .ok, detail: "Python \(cfg.python.version) and Rust are ready.")
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: buildFolder, withIntermediateDirectories: true)
        Toolchain.excludeFromBackup(buildFolder)

        let args = ["-u", source.appending(path: "scripts/build_app.py").path, game.path, "--progress", "json",
                    "--work-dir", buildFolder.path, "-o", destination.path]
        let status = try await runTool(Toolchain.python3, args, env: env, cwd: source) { [weak self] kind, line in
            guard let self else { return }
            if kind == .out { self.event(line) } else { self.stderrTail.add(line); self.addLine(line) }
        }
        if Task.isCancelled || [129, 130, 143].contains(status) { throw CancellationError() }
        guard status == 0 else {
            throw ToolFailure(lastRed ?? "The build script stopped (exit status \(status)): \(stderrTail.last)")
        }
        guard let app = outApp ?? appIn(destination) else { throw ToolFailure("The build finished, but the app it made can't be found in \(destination.path).") }
        built = BuiltApp(path: app.path, commit: cfg.commit, name: gameName ?? app.deletingPathExtension().lastPathComponent, version: activityVersion ?? "")
        screen = .finished
        let name = built?.name ?? "The game"
        Notify.post(title: "\(name) is ready", body: "It's in \(destination.lastPathComponent). Open it to play.")
        refreshIfIdle()
    }

    private var activityVersion: String?

    private func appIn(_ folder: URL) -> URL? {   // without a game event (it always comes before the app is packed): the newest app in the folder
        let apps = ((try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []).filter { $0.pathExtension == "app" }
        return apps.max { (a, b) in
            let da = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let db = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return da < db
        }
    }

    private func fail(_ message: String) {
        failure = message
        screen = .failed
        Notify.post(title: "The build stopped", body: message)
    }

    private func step<T>(_ title: String, _ body: () async throws -> T) async throws -> T {
        try Task.checkCancellation()
        finishStep(.done)
        steps.append(Step(title: title))
        activity = Activity(title: title, position: "Getting ready")
        addLine(title)
        let r = try await body()
        try Task.checkCancellation()
        finishStep(.done)
        return r
    }

    private func finishStep(_ state: Step.State, summary: String? = nil) {
        guard let i = steps.indices.last, steps[i].state == .running else { return }
        steps[i].state = state
        if let summary { steps[i].summary = summary }
    }

    private func progress(_ fraction: Double?, _ detail: String) {
        guard activity != nil else { return }
        activity?.fraction = fraction
        activity?.detail = detail
        DockProgress.show(fraction)
    }

    private func addLine(_ line: String) {
        lines.append(line)
        if lines.count > 600 { lines.removeFirst(lines.count - 500) }
    }

    /// One event of the packing script (docs/frontends.md, Building: the packing script)
    private func event(_ line: String) {
        guard let data = line.data(using: .utf8), let e = try? JSONDecoder().decode(BuildEvent.self, from: data) else { addLine(line); return }
        switch e.ev {
        case "stage":
            finishStep(.done)
            let title = Self.capitalized(e.name ?? "")
            steps.append(Step(title: title))
            activity = Activity(title: title, position: "Step \(e.k ?? 0) of \(e.n ?? 0)", detail: e.step ?? "")
            DockProgress.show(0)
        case "step":
            activity?.detail = e.step ?? ""
            activity?.fraction = nil
            activity?.eta = nil
        case "progress":
            activity?.fraction = e.frac
            activity?.eta = e.eta
            activity?.detail = Self.count(e)
            DockProgress.show(e.frac)
        case "stage_end":
            let took = e.elapsed.map(Self.duration) ?? ""
            let summary = [e.summary ?? "", took].filter { !$0.isEmpty }.joined(separator: " · ")
            finishStep(e.ok == false ? .failed : e.warn == true ? .warning : .done, summary: Self.capitalized(summary))
        case "game":
            gameName = e.name
            activityVersion = e.version
            outApp = e.out.map { URL(filePath: $0, directoryHint: .isDirectory) }
            gameIcon = e.icon.map { URL(filePath: $0) }
        case "say":
            let msg = Self.unstamped(e.msg ?? "")
            addLine(msg)
            if let r = msg.range(of: "[RED] ") { lastRed = String(msg[r.upperBound...]) }
        default:
            break
        }
    }

    // MARK: After a build

    func play() {
        guard let built else { return }
        NSWorkspace.shared.openApplication(at: built.url, configuration: NSWorkspace.OpenConfiguration())
    }

    func reveal() {
        guard let built else { return }
        NSWorkspace.shared.activateFileViewerSelecting([built.url])
    }

    func showLog() { NSWorkspace.shared.open(Log.shared.url) }

    func showBuildLogs() {
        let logs = buildFolder.appending(path: "logs")
        NSWorkspace.shared.open(FileManager.default.fileExists(atPath: logs.path) ? logs : Log.shared.url)
    }

    /// Build again from the finished or stopped screen: at once when nothing changed that needs the player, else back to the setup
    func retry() {
        screen = .setup
        Task {
            await refresh()
            if canBuild && diskCheck.level != .warning { build() }
        }
    }

    func update() { retry() }

    /// A way out for a failure whose cause the player can remove
    var failureHint: (text: String, button: String, url: URL)? {
        guard let failure, failure.contains("Operation not permitted") || failure.contains("Permission denied") else { return nil }
        return ("macOS may have kept NativeNavi from writing a file. Allow it under Privacy & Security › App Management (to replace the game's app) or Full Disk Access, then build again.",
                "Open Privacy & Security", URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AppBundles")!)
    }

    func backToSetup() {
        guard !isBusy else { return }
        screen = .setup
        refreshIfIdle()
    }

    var buildFilesSize: Int64?
    var toolsSize: Int64?
    var removing = false

    func measure() {
        let build = buildFolder, tools = Paths.tools
        Task {
            let (b, t) = await Task.detached { (Toolchain.size(of: build), Toolchain.size(of: tools)) }.value
            buildFilesSize = b; toolsSize = t
        }
    }

    /// Deletes the build folder (or the downloaded tools); the game's app is left as it is: its large files are clones or copies, not links into the build folder
    func remove(tools: Bool) {
        guard !isBusy, !removing else { return }
        let target = tools ? Paths.tools : buildFolder
        removing = true
        Task {
            await Task.detached { try? FileManager.default.removeItem(at: target) }.value
            removing = false
            measure()
            refreshIfIdle()
        }
    }

    // MARK: Text

    private static func capitalized(_ s: String) -> String { s.prefix(1).uppercased() + s.dropFirst() }

    private static func unstamped(_ s: String) -> String {   // say's messages start with the time of day
        s.replacing(#/^\[\d{2}:\d{2}:\d{2}\] /#, with: "")
    }

    static func duration(_ seconds: Double) -> String {
        Duration.seconds(max(1, seconds.rounded())).formatted(.units(allowed: [.hours, .minutes, .seconds], width: .abbreviated, maximumUnitCount: 2))
    }

    private static func count(_ e: BuildEvent) -> String {
        var parts: [String] = []
        if let done = e.done {
            let unit = e.unit ?? ""
            let fmt: (Double) -> String = unit == "B" ? { bytes(Int64($0)) } : { Int($0).formatted() }
            let about = e.est == true ? "about " : ""
            if let total = e.total, total > 0 {
                parts.append("\(fmt(done)) of \(about)\(fmt(total))" + (unit.isEmpty || unit == "B" ? "" : " \(unit)"))
            } else {
                parts.append(fmt(done) + (unit.isEmpty || unit == "B" ? "" : " \(unit)"))
            }
        } else if let step = e.step, !step.isEmpty {
            parts.append(step)
        }
        if let note = e.note, !note.isEmpty { parts.append(note) }
        return parts.joined(separator: " · ")
    }
}
