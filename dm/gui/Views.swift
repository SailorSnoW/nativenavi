// The window's screens: the setup (the game file, what the build needs, where the app goes), the build's progress, the finished app, a stopped build; and the Settings window.
// System controls and grouped forms throughout; Liquid Glass where macOS 26 puts it, on the controls and the bars over the scrolling content (glass buttons, safeAreaBar)
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct RootView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Group {
            switch model.screen {
            case .setup: SetupView()
            case .working: WorkingView()
            case .finished: FinishedView()
            case .failed, .stopped: StoppedView()
            }
        }
        .frame(width: 620, height: 680)
        .task { await model.refresh() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in model.refreshIfIdle() }
    }
}

// MARK: - Pieces

private struct Header: View {
    var icon: NSImage?
    let title: String
    let subtitle: String

    var body: some View {
        HStack(spacing: 16) {
            Image(nsImage: icon ?? NSApp.applicationIconImage)
                .resizable()
                .interpolation(.high)
                .frame(width: 64, height: 64)
                .clipShape(.rect(cornerRadius: icon == nil ? 0 : 14, style: .continuous))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.title.weight(.semibold))
                Text(subtitle).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 28)
        .padding(.top, 4)
        .padding(.bottom, 6)
    }
}

private struct ActionBar<Leading: View, Trailing: View>: View {
    @ViewBuilder var leading: Leading
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 12) {
            leading
            Spacer(minLength: 12)
            trailing
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 14)
    }
}

private struct StatusIcon: View {
    let level: Check.Level

    var body: some View {
        Group {
            switch level {
            case .checking: ProgressView().controlSize(.small)
            case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            case .later: Image(systemName: "arrow.down.circle.fill").foregroundStyle(.blue)
            case .waiting: Image(systemName: "circle.dashed").foregroundStyle(.secondary)
            case .action: Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.orange)
            case .warning: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow)
            case .problem: Image(systemName: "xmark.circle.fill").foregroundStyle(.red)
            }
        }
        .imageScale(.large)
        .frame(width: 22)
        .accessibilityHidden(true)
    }
}

private struct RequirementRow<Accessory: View>: View {
    let title: String
    let check: Check
    @ViewBuilder var accessory: Accessory

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            StatusIcon(level: check.level)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                Text(check.detail).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 8)
            accessory
        }
        .padding(.vertical, 2)
    }
}

extension RequirementRow where Accessory == EmptyView {
    init(title: String, check: Check) { self.init(title: title, check: check) { EmptyView() } }
}

private func display(_ url: URL) -> String { (url.path as NSString).abbreviatingWithTildeInPath }

private func folderName(_ url: URL) -> String { FileManager.default.displayName(atPath: url.path) }

// MARK: - Setup

private struct SetupView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        VStack(spacing: 0) {
            Header(title: "NativeNavi", subtitle: "Turns your copy of Ocarina of Time for Nintendo Switch 2 into an app that runs natively on this Mac.")
            Form {
                Section("Your Game") {
                    GameField()
                }
                Section("What the Build Needs") {
                    XcodeRow()
                    RequirementRow(title: "Metal Toolchain", check: model.metalCheck)
                    RequirementRow(title: "Python and Rust", check: model.toolsCheck)
                    RequirementRow(title: "Disk Space", check: model.diskCheck)
                }
                Section {
                    DestinationPicker()
                }
            }
            .formStyle(.grouped)
            .safeAreaBar(edge: .bottom) {
                ActionBar {
                    Text(model.hint).font(.callout).foregroundStyle(.secondary)
                } trailing: {
                    Button { model.requestBuild() } label: { Text("Build").frame(minWidth: 72) }
                        .buttonStyle(.glassProminent)
                        .controlSize(.large)
                        .keyboardShortcut(.defaultAction)
                        .disabled(!model.canBuild)
                }
            }
        }
        .alert("This isn't a version of the game NativeNavi can build", isPresented: Binding(get: { model.unknownGame != nil }, set: { if !$0 { model.unknownGame = nil } })) {
            Button("Choose Another File…") { model.unknownGame = nil; model.chooseGame() }
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.unknownGame ?? "")
        }
        .alert("Your Mac may run out of space", isPresented: $model.confirmSpace) {
            Button("Build Anyway") { model.build() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(model.diskCheck.detail)
        }
    }
}

private struct GameField: View {
    @Environment(AppModel.self) private var model
    @State private var targeted = false

    var body: some View {
        Group {
            if let game = model.game {
                HStack(spacing: 12) {
                    Image(nsImage: NSWorkspace.shared.icon(forFile: game.path))
                        .resizable()
                        .frame(width: 36, height: 36)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(game.lastPathComponent).lineLimit(1).truncationMode(.middle)
                        switch model.gameCheck {
                        case .problem(let problem):
                            Label(problem, systemImage: "exclamationmark.triangle.fill")
                                .font(.callout)
                                .foregroundStyle(.orange)
                                .fixedSize(horizontal: false, vertical: true)
                        case .verifying(let fraction, let what):
                            Text(what).font(.callout).foregroundStyle(.secondary)
                            if let fraction {
                                ProgressView(value: fraction).controlSize(.small)
                            } else {
                                ProgressView().progressViewStyle(.linear).controlSize(.small)
                            }
                        case .verified(let known):
                            Label("Verified copy: \(known.holds)", systemImage: "checkmark.seal.fill")
                                .font(.callout)
                                .foregroundStyle(.green)
                            Text(details(game)).font(.callout).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        case .none:
                            Text(details(game)).font(.callout).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        }
                    }
                    Spacer(minLength: 8)
                    Button("Change…") { model.chooseGame() }
                }
                .padding(.vertical, 4)
            } else {
                VStack(spacing: 6) {
                    Image(systemName: "arrow.down.doc")
                        .font(.system(size: 26, weight: .light))
                        .foregroundStyle(targeted ? Color.accentColor : .secondary)
                        .padding(.bottom, 2)
                        .accessibilityHidden(true)
                    Text("Drop your game's .nup file here").font(.headline)
                    Text("The NUP container with the game, its update and its DLC.").font(.callout).foregroundStyle(.secondary)
                    Button("Choose File…") { model.chooseGame() }
                        .padding(.top, 6)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
                .background {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(targeted ? Color.accentColor.opacity(0.08) : .clear)
                        .strokeBorder(targeted ? Color.accentColor : Color.secondary.opacity(0.35), style: StrokeStyle(lineWidth: 1.5, dash: [6, 5]))
                }
                .animation(.easeOut(duration: 0.15), value: targeted)
            }
        }
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            model.choose(url)
            return true
        } isTargeted: { targeted = $0 }
    }

    private func details(_ url: URL) -> String {
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize).map { bytes(Int64($0)) }
        return [size, display(url.deletingLastPathComponent())].compactMap { $0 }.joined(separator: " · ")
    }
}

private struct XcodeRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        RequirementRow(title: "Xcode \(model.cfg.xcode)", check: model.xcodeCheck) {
            switch model.xcodeAction {
            case .download?:
                Button("Download…") { NSWorkspace.shared.open(model.cfg.xcodeDownload) }
            case .open(let app)?:
                Button("Open Xcode") { NSWorkspace.shared.openApplication(at: app, configuration: NSWorkspace.OpenConfiguration()) }
            case nil:
                EmptyView()
            }
        }
    }
}

private struct DestinationPicker: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        LabeledContent {
            Menu {
                Button("Applications") { model.destination = URL(filePath: "/Applications", directoryHint: .isDirectory) }
                    .disabled(!FileManager.default.isWritableFile(atPath: "/Applications"))
                Button("Applications in Your Home Folder") { model.destination = Paths.home.appending(path: "Applications", directoryHint: .isDirectory) }
                Divider()
                Button("Other Folder…") { model.chooseDestination() }
            } label: {
                Label(folderName(model.destination), systemImage: "folder")
            }
            .fixedSize()
        } label: {
            Text("Put the game's app in")
            Text(display(model.destination))
        }
    }
}

// MARK: - Building

private struct WorkingView: View {
    @Environment(AppModel.self) private var model
    @State private var details = false

    var body: some View {
        @Bindable var model = model
        VStack(spacing: 0) {
            Header(icon: model.gameIcon.flatMap { NSImage(contentsOf: $0) },
                   title: model.gameName.map { "Building \($0)" } ?? "Building the Game",
                   subtitle: "Your Mac stays awake until it's done. You can close this window: the build goes on, and NativeNavi lets you know when the game is ready.")
            Form {
                if let activity = model.activity {
                    Section {
                        CurrentStep(activity: activity)
                    }
                }
                Section("Progress") {
                    ForEach(model.steps) { StepRow(step: $0) }
                }
                Section {
                    DisclosureGroup("Details", isExpanded: $details) {
                        LogView(lines: model.lines)
                    }
                }
            }
            .formStyle(.grouped)
            .safeAreaBar(edge: .bottom) {
                ActionBar {
                    if let started = model.started {
                        HStack(spacing: 4) {
                            Text("Elapsed")
                            Text(started, style: .timer).monospacedDigit()
                        }
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    }
                } trailing: {
                    Button("Stop…") { model.confirmStop = true }
                        .buttonStyle(.glass)
                        .controlSize(.large)
                }
            }
        }
        .confirmationDialog("Stop the build?", isPresented: $model.confirmStop) {
            Button("Stop Build", role: .destructive) { model.stop() }
            Button("Keep Building", role: .cancel) {}
        } message: {
            Text("Building again later picks up where this build stopped.")
        }
    }
}

private struct CurrentStep: View {
    let activity: Activity

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(activity.title).font(.headline)
                Spacer()
                Text(activity.position).font(.callout).foregroundStyle(.secondary)
            }
            if let fraction = activity.fraction {
                ProgressView(value: fraction)
            } else {
                ProgressView().progressViewStyle(.linear)
            }
            HStack(alignment: .firstTextBaseline) {
                Text(activity.detail.isEmpty ? " " : activity.detail).lineLimit(1).truncationMode(.middle)
                Spacer(minLength: 12)
                if let eta = activity.eta, eta >= 1 {
                    Text("About \(AppModel.duration(eta)) left")
                }
            }
            .font(.callout)
            .foregroundStyle(.secondary)
            .monospacedDigit()
        }
        .padding(.vertical, 6)
        .accessibilityElement(children: .combine)
    }
}

private struct StepRow: View {
    let step: Step

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Group {
                switch step.state {
                case .running: ProgressView().controlSize(.mini)
                case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                case .warning: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow)
                case .failed: Image(systemName: "xmark.circle.fill").foregroundStyle(.red)
                }
            }
            .frame(width: 18)
            .accessibilityHidden(true)
            Text(step.title)
            Spacer(minLength: 12)
            Text(step.summary).font(.callout).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail).help(step.summary)
        }
    }
}

private struct LogView: View {
    let lines: [String]

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 2) {
                ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                    Text(line)
                        .font(.caption.monospaced())
                        .foregroundStyle(line.contains("[RED] ") ? AnyShapeStyle(.red) : AnyShapeStyle(.secondary))
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .textSelection(.enabled)
            .padding(.vertical, 4)
        }
        .defaultScrollAnchor(.bottom)
        .frame(height: 200)
    }
}

// MARK: - Done

private struct FinishedView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let built = model.built {
            VStack(spacing: 0) {
                Spacer(minLength: 20)
                Image(nsImage: NSWorkspace.shared.icon(forFile: built.path))
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 144, height: 144)
                    .shadow(color: .black.opacity(0.18), radius: 14, y: 8)
                    .accessibilityHidden(true)
                Text("\(built.name) is ready")
                    .font(.largeTitle.weight(.semibold))
                    .multilineTextAlignment(.center)
                    .padding(.top, 18)
                    .padding(.horizontal, 40)
                Text("It's in \(folderName(built.url.deletingLastPathComponent())). Open it from there, from Launchpad or with Spotlight.")
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.top, 6)
                    .padding(.horizontal, 70)
                HStack(spacing: 12) {
                    Button("Show in Finder") { model.reveal() }
                        .buttonStyle(.glass)
                    Button { model.play() } label: { Label("Play", systemImage: "play.fill") }
                        .buttonStyle(.glassProminent)
                        .keyboardShortcut(.defaultAction)
                }
                .controlSize(.extraLarge)
                .padding(.top, 24)
                Text("Before the game starts, its own window lets you choose its language, controls and other settings.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.top, 16)
                    .padding(.horizontal, 80)
                if model.updateAvailable {
                    UpdateCallout()
                        .padding(.top, 24)
                        .padding(.horizontal, 48)
                }
                Spacer(minLength: 20)
            }
            .frame(maxWidth: .infinity)
            .safeAreaBar(edge: .bottom) {
                ActionBar {
                    BuildFilesSummary()
                } trailing: {
                    Button("Build Again…") { model.backToSetup() }
                }
            }
        }
    }
}

private struct UpdateCallout: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "arrow.triangle.2.circlepath")
                .font(.title2)
                .foregroundStyle(.tint)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text("An update is ready").font(.headline)
                Text("This version of NativeNavi builds the game with newer fixes. The build reuses your earlier one's work, so it's much quicker than the first.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 8)
            Button("Update") { model.update() }
                .buttonStyle(.glassProminent)
        }
        .padding(16)
        .background(.fill.quaternary, in: .rect(cornerRadius: 16, style: .continuous))
    }
}

private struct BuildFilesSummary: View {
    @Environment(AppModel.self) private var model
    @State private var confirm = false

    var body: some View {
        HStack(spacing: 8) {
            if let size = model.buildFilesSize, size > 0 {
                Text("Build files: \(bytes(size))").foregroundStyle(.secondary)
                Button("Remove…") { confirm = true }
                    .buttonStyle(.link)
                    .disabled(model.removing)
            } else if model.removing {
                ProgressView().controlSize(.small)
                Text("Removing build files…").foregroundStyle(.secondary)
            }
        }
        .font(.callout)
        .onAppear { model.measure() }
        .confirmationDialog("Remove the build files?", isPresented: $confirm) {
            Button("Remove Build Files", role: .destructive) { model.remove(tools: false) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The game's app stays as it is. Without these files, the next build (after an update of NativeNavi) starts from the beginning and takes hours again.")
        }
    }
}

// MARK: - Stopped

private struct StoppedView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let failed = model.screen == .failed
        VStack(spacing: 0) {
            Spacer(minLength: 20)
            Image(systemName: failed ? "exclamationmark.triangle.fill" : "stop.circle.fill")
                .font(.system(size: 64))
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(failed ? AnyShapeStyle(.orange) : AnyShapeStyle(.secondary))
                .accessibilityHidden(true)
            Text(failed ? "The build stopped" : "Build stopped")
                .font(.largeTitle.weight(.semibold))
                .padding(.top, 14)
            if failed, let failure = model.failure {
                ScrollView {
                    Text(failure)
                        .font(.callout)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(14)
                }
                .frame(maxHeight: 190)
                .fixedSize(horizontal: false, vertical: true)
                .background(.fill.quaternary, in: .rect(cornerRadius: 12, style: .continuous))
                .padding(.horizontal, 48)
                .padding(.top, 16)
                if let hint = model.failureHint {
                    HStack(alignment: .top, spacing: 10) {
                        Image(systemName: "lightbulb").foregroundStyle(.yellow).accessibilityHidden(true)
                        Text(hint.text).font(.callout).fixedSize(horizontal: false, vertical: true)
                        Spacer(minLength: 8)
                        Button(hint.button) { NSWorkspace.shared.open(hint.url) }
                    }
                    .padding(.horizontal, 52)
                    .padding(.top, 12)
                }
            }
            Text("Building again picks up where this build stopped.")
                .foregroundStyle(.secondary)
                .padding(.top, 14)
            HStack(spacing: 12) {
                Button("Show Log") { model.showBuildLogs() }
                    .buttonStyle(.glass)
                Button("Build Again") { model.retry() }
                    .buttonStyle(.glassProminent)
                    .keyboardShortcut(.defaultAction)
            }
            .controlSize(.extraLarge)
            .padding(.top, 22)
            Button("Back to Setup") { model.backToSetup() }
                .buttonStyle(.link)
                .padding(.top, 14)
            Spacer(minLength: 20)
        }
        .frame(maxWidth: .infinity)
    }
}

// MARK: - Settings

struct SettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var confirmBuild = false
    @State private var confirmTools = false

    var body: some View {
        Form {
            Section {
                LabeledContent {
                    Menu {
                        Button("In Your Library (Default)") { model.buildParent = nil }
                        Button("Choose Folder…") { model.chooseBuildParent() }
                    } label: {
                        Label(folderName(model.buildFolder.deletingLastPathComponent()), systemImage: "folder")
                    }
                    .fixedSize()
                } label: {
                    Text("Location")
                    Text(display(model.buildFolder))
                }
                LabeledContent("Size", value: model.buildFilesSize.flatMap { $0 > 0 ? bytes($0) : nil } ?? "None")
                HStack {
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([model.buildFolder]) }
                        .disabled(!FileManager.default.fileExists(atPath: model.buildFolder.path))
                    Spacer()
                    Button("Remove Build Files…", role: .destructive) { confirmBuild = true }
                        .disabled((model.buildFilesSize ?? 0) == 0 || model.removing)
                }
            } header: {
                Text("Build Files")
            } footer: {
                Text("The unpacked game and its translated shaders. With them, a later build redoes only what changed. A new location starts the next build from the beginning.")
            }
            Section {
                LabeledContent("Location", value: display(Paths.tools))
                LabeledContent("Size", value: model.toolsSize.flatMap { $0 > 0 ? bytes($0) : nil } ?? "None")
                HStack {
                    Spacer()
                    Button("Remove Tools…", role: .destructive) { confirmTools = true }
                        .disabled((model.toolsSize ?? 0) == 0 || model.removing)
                }
            } header: {
                Text("Downloaded Tools")
            } footer: {
                Text("Python and Rust, which only NativeNavi uses. They're downloaded again when a build needs them.")
            }
        }
        .formStyle(.grouped)
        .frame(width: 540)
        .fixedSize(horizontal: false, vertical: true)
        .disabled(model.isBusy)
        .onAppear { model.measure() }
        .confirmationDialog("Remove the build files?", isPresented: $confirmBuild) {
            Button("Remove Build Files", role: .destructive) { model.remove(tools: false) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The game's app stays as it is. The next build starts from the beginning and takes hours again.")
        }
        .confirmationDialog("Remove the downloaded tools?", isPresented: $confirmTools) {
            Button("Remove Tools", role: .destructive) { model.remove(tools: true) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("They're downloaded again at the next build.")
        }
    }
}

// MARK: - Open panels

@MainActor enum Panels {
    /// An open panel, as a sheet on the frontmost window when there is one
    static func choose(types: [UTType]? = nil, folder: Bool, message: String, prompt: String) async -> URL? {
        let panel = NSOpenPanel()
        panel.message = message
        panel.prompt = prompt
        panel.canChooseFiles = !folder
        panel.canChooseDirectories = folder
        panel.canCreateDirectories = folder
        panel.allowsMultipleSelection = false
        if let types { panel.allowedContentTypes = types }
        if let window = NSApp.keyWindow ?? NSApp.mainWindow {
            return await panel.beginSheetModal(for: window) == .OK ? panel.url : nil
        }
        return panel.runModal() == .OK ? panel.url : nil
    }
}
