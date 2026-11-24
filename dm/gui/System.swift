import AppKit
import UserNotifications

/// The notice when a build ends while the app is in the background
final class Notify: NSObject, UNUserNotificationCenterDelegate, Sendable {
    static let shared = Notify()

    @MainActor static func ask() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    @MainActor static func post(title: String, body: String) {
        NSApp.requestUserAttention(.informationalRequest)
        guard !NSApp.isActive else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "build", content: content, trigger: nil))
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        await MainActor.run { NSApp.activate() }
    }
}

/// The current stage's progress as a bar on the Dock icon, redrawn at most twice a second; an uncounted stage leaves the bar where it was
@MainActor enum DockProgress {
    private static var bar: NSProgressIndicator?
    private static var last = Date.distantPast

    static func show(_ fraction: Double?) {
        let tile = NSApp.dockTile
        if bar == nil {
            let size = tile.size
            let view = NSView(frame: NSRect(origin: .zero, size: size))
            let icon = NSImageView(frame: view.bounds)
            icon.image = NSApp.applicationIconImage
            view.addSubview(icon)
            let b = NSProgressIndicator(frame: NSRect(x: size.width * 0.12, y: size.height * 0.06, width: size.width * 0.76, height: 14))
            b.style = .bar
            b.isIndeterminate = false
            b.minValue = 0
            b.maxValue = 1
            view.addSubview(b)
            tile.contentView = view
            bar = b
        }
        guard let fraction, let bar, bar.doubleValue != fraction else { return }
        let now = Date.now
        guard now.timeIntervalSince(last) > 0.5 || fraction == 0 else { return }
        last = now
        bar.doubleValue = fraction
        tile.display()
    }

    static func hide() {
        guard bar != nil else { return }
        NSApp.dockTile.contentView = nil
        bar = nil
        NSApp.dockTile.display()
    }
}
