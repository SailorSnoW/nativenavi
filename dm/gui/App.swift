// NativeNavi: a Mac app that builds The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) for this Mac from the player's own NUP, for players who do not use Terminal
// (docs/gui.md). One window (the setup, the build's progress, the finished app), a Settings window for where the build files go, and the Dock icon's progress while it builds.
// Closing the window during a build keeps the build going; quitting asks first, then stops it as the packing script's front-end contract says (SIGTERM)
import AppKit
import SwiftUI
import UserNotifications

@main
struct NativeNaviApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        Window("NativeNavi", id: "main") {
            RootView()
                .environment(delegate.model)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
        .defaultPosition(.center)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About NativeNavi") { delegate.about() }
            }
            CommandGroup(replacing: .newItem) {
                Button("Choose Game File…") { delegate.model.chooseGame() }
                    .keyboardShortcut("o")
                    .disabled(delegate.model.isBusy)
            }
            CommandGroup(after: .newItem) {
                Divider()
                Button("Show Build Log") { delegate.model.showBuildLogs() }
                Button("Show NativeNavi Log") { delegate.model.showLog() }
            }
            CommandGroup(replacing: .help) {}
        }

        Settings {
            SettingsView()
                .environment(delegate.model)
        }
    }
}

@MainActor final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel()

    func applicationDidFinishLaunching(_ notification: Notification) {
        UNUserNotificationCenter.current().delegate = Notify.shared
    }

    func application(_ application: NSApplication, open urls: [URL]) {   // a .nup dropped on the app's icon, or opened with it
        model.choose(urls.first)
        if model.screen == .finished && !model.isBusy { model.backToSetup() }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { !model.isBusy }   // a build goes on without the window

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard model.isBusy else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "Stop the build and quit?"
        alert.informativeText = "Building again later picks up where this build stopped."
        alert.addButton(withTitle: "Stop and Quit").hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return .terminateCancel }
        Task {
            await model.stopAndWait()
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    func about() {
        let credits = NSMutableAttributedString(string: "Builds the Mac app of your own copy of The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) with NativeNavi \(model.cfg.commit).\n\nNot affiliated with Nintendo.",
                                                attributes: [.font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize), .foregroundColor: NSColor.secondaryLabelColor])
        let center = NSMutableParagraphStyle()
        center.alignment = .center
        credits.addAttribute(.paragraphStyle, value: center, range: NSRange(location: 0, length: credits.length))
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
        NSApp.activate()
    }
}
